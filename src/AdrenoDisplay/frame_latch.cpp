// SPDX-License-Identifier: MIT
// When a frame latches, each behind its build switch: the beam (ADRENO_BEAM), the release hold (ADRENO_LATCH),
// present sync and the relatch (ADRENO_PRESENT_SYNC, ADRENO_PRESENT_RELATCH).
// Part of AdrenoDisplay.cpp's translation unit: included from it in order, never compiled on its own.

#if ADRENO_BEAM
// ---------------------------------------------------------------- beam (--present-beam, AdrenoDisplayBeam.h)

uint32_t AdrenoDisplay::beamRead(void *ctx, uint32_t offset)
{
    // A1 A3: one 32-bit load inside the one mapped page; every caller passes a DPU_INTF_* constant.
    const AdrenoDisplay *self = (const AdrenoDisplay *)ctx;
    return offset < DPU_INTF_PAGE && !(offset & 3) ? self->fBeamRegs[offset / 4] : 0;
}

uint64_t AdrenoDisplay::beamNow(void *)
{
    return mach_absolute_time();
}

void AdrenoDisplay::beamGate(void *ctx, uint64_t, uint64_t y0, uint64_t y1)
{
    AdrenoDisplay *self = (AdrenoDisplay *)ctx;
    const struct adreno_beam_io io = {beamRead, beamNow, self};
    adreno_beam_gate(&self->fBeam, &io, y0, y1);
}

void AdrenoDisplay::beamUnmap()
{
    fBeamRegs = nullptr;
    OSSafeReleaseNULL(fBeamMap);
    OSSafeReleaseNULL(fBeamMD);
}

// A1-A4 B1 D3: the page named by the GPU node (no properties: an ordinary launch, nothing is mapped or read), mapped
// read-only and uncached, its stage-1 attribute checked, the timing decoded and calibrated. A refusal unmaps the page
// and keeps the VSYNC60 timer path; the registers and the outcome go to the console once.
void AdrenoDisplay::beamMapGated()
{
    uint64_t ms = 0;
    nanoseconds_to_absolutetime(1000000, &ms);
    beamUnmap();
    adreno_beam_init(&fBeam, ms, kHeight);
    IOService *accelerator = getProvider(), *node = accelerator ? accelerator->getProvider() : nullptr;
    OSData *abi = node ? OSDynamicCast(OSData, node->getProperty(DPU_INTF_ABI_PROPERTY)) : nullptr;
    OSData *pa = node ? OSDynamicCast(OSData, node->getProperty(DPU_INTF_PA_PROPERTY)) : nullptr;
    if (!abi && !pa)
        return;                                      // state OFF: the monitor maps no INTF page
    int reason = adreno_beam_check_property(abi ? abi->getBytesNoCopy() : nullptr, abi ? abi->getLength() : 0,
                                            pa ? pa->getBytesNoCopy() : nullptr, pa ? pa->getLength() : 0);
    if (!reason) {
        fBeamMD = IOMemoryDescriptor::withPhysicalAddress(DPU_INTF_PA, DPU_INTF_PAGE, kIODirectionIn);
        if (fBeamMD)
            fBeamMap = fBeamMD->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly,
                                                    0, DPU_INTF_PAGE);
        if (!fBeamMap)
            reason = ADRENO_BEAM_R_MAP;
    }
    if (!reason) {
        fBeamAttr = mappingAttribute((const void *)(uintptr_t)fBeamMap->getVirtualAddress());
        if (!adreno_beam_attr_ok(fBeamAttr))
            reason = ADRENO_BEAM_R_ATTR;
    }
    if (reason) {
        IOLog("AdrenoDisplay: beam refused (%d) before any read: attr 0x%llx\n", reason, fBeamAttr);
        adreno_beam_refuse(&fBeam, (uint32_t)reason);
        beamUnmap();
        return;
    }
    fBeamRegs = (const volatile uint32_t *)(uintptr_t)fBeamMap->getVirtualAddress();
    const struct adreno_beam_io io = {beamRead, beamNow, this};
    struct adreno_beam_regs r;
    adreno_beam_read_regs(&io, &r);
    reason = adreno_beam_decode(&r, kWidth, kHeight, &fBeam.t);
    IOLog("AdrenoDisplay: beam INTF_5 en=%x status=%x cfg=%x/%x hsync=%x vperiod=%x v=%x-%x act=%x-%x hctl=%x/%x "
          "count_en=%x avr=%x attr=0x%llx: vtotal %u active %u-%u width %u%s (%d)\n", r.engine, r.status, r.config,
          r.config2, r.hsync_ctl, r.vsync_period, r.disp_v_start, r.disp_v_end, r.act_v_start, r.act_v_end, r.disp_hctl,
          r.act_hctl, r.count_en, r.avr, fBeamAttr, fBeam.t.vtotal, fBeam.t.a0, fBeam.t.a1, fBeam.t.width,
          fBeam.t.width_ok ? "" : " (width differs)", reason);
    if (reason) {
        adreno_beam_refuse(&fBeam, (uint32_t)reason);
        beamUnmap();
        return;
    }
    fBeam.state = ADRENO_BEAM_UNCAL;
    beamCalibrateGated();
}

// B2 B3 D4: up to 250 ms of LINE_COUNT reads. The first failure refuses the beam for this start; after a lock, a
// failed recalibration stalls it for ADRENO_BEAM_STALL_TICKS timer ticks, and the fourth in a row refuses it.
bool AdrenoDisplay::beamCalibrateGated()
{
    if (!fBeamRegs)
        return false;
    const struct adreno_beam_io io = {beamRead, beamNow, this};
    const bool locked_before = fBeam.frame != 0;
    const int reason = adreno_beam_calibrate(&fBeam, &io);
    if (!reason) {
        fBeam.recal_failures = 0;
    } else if (!locked_before || ++fBeam.recal_failures >= ADRENO_BEAM_RETRIES) {
        adreno_beam_refuse(&fBeam, locked_before ? (uint32_t)ADRENO_BEAM_R_RETRIES : (uint32_t)reason);
    } else {
        fBeam.state = ADRENO_BEAM_STALLED;
        fBeam.reason = (uint32_t)reason;
        fBeam.stalled_ticks = 0;
        fBeam.stalls++;
    }
    IOLog("AdrenoDisplay: beam calibration %s (%d): frame %llu ns, line base %u, frame counter %s at line %u, "
          "%u vblank(s) per tick\n", reason ? "failed" : "locked", reason, perfNs(fBeam.frame), fBeam.line_base,
          fBeam.fc_ok ? "tracks" : "unused", fBeam.fc_line, fBeam.n);
    return !reason;
}

// C1 C6 D4: arm the frame timer `lead` before the targeted vblank, or return false for the VSYNC60 grid (no page,
// refused, stalled, a glitched sample, drift found by the sample, or a timer refusal).
bool AdrenoDisplay::beamArmGated()
{
    if (!fBeamRegs)
        return false;
    if (fBeam.state == ADRENO_BEAM_STALLED && ++fBeam.stalled_ticks >= ADRENO_BEAM_STALL_TICKS)
        fBeam.state = ADRENO_BEAM_UNCAL;
    if (fBeam.state == ADRENO_BEAM_UNCAL && !beamCalibrateGated())
        return false;
    if (fBeam.state != ADRENO_BEAM_LOCKED)
        return false;
    const struct adreno_beam_io io = {beamRead, beamNow, this};
    uint64_t now = 0;
    const uint64_t pos = adreno_beam_sample(&fBeam, &io, &now);
    if (!pos || fBeam.state != ADRENO_BEAM_LOCKED) {
        fBeam.target = 0;
        return false;
    }
    if (!fBeam.target)
        adreno_beam_target(&fBeam, pos, fInTick);    // EARLY and LATE keep the vblank they chose
    const uint64_t fire = adreno_beam_fire_time(&fBeam, now, pos);
    const uint64_t at = adreno_vsync_arm_time(&fVsync, now, fire);    // S6: absorb the timer's coalescing slop
    if (fFrameTimer->wakeAtTime(0, at, 0) != kIOReturnSuccess) {
        fBeam.target = fBeam.armed_fire = 0;
        return false;
    }
    fVsync.deadline = fire;                           // the vsync line reports fire lateness against it
    fVsync.arms++;
    fFrameTimerArmed = true;
    fArmedAt = now;
    return true;
}

// C1 C2: the fire on the beam. EARLY and LATE re-arm for their vblank and return true (the tick does nothing else);
// GO, FORCED, UNARMED and FAILED return false and the tick completes and copies as before.
bool AdrenoDisplay::beamFireGated()
{
    if (!fBeamRegs || fBeam.state != ADRENO_BEAM_LOCKED || (!fBeam.target && !fBeam.armed_fire))
        return false;                                 // armed by the grid: an ordinary tick
    const struct adreno_beam_io io = {beamRead, beamNow, this};
    uint64_t now = 0;
    const uint64_t pos = adreno_beam_sample(&fBeam, &io, &now);
    const int outcome = adreno_beam_fire(&fBeam, now, pos);
    if (outcome != ADRENO_BEAM_EARLY && outcome != ADRENO_BEAM_LATE)
        return false;
    armFrameTimerGated();
    return true;
}
#endif

#if ADRENO_LATCH
// ---------------------------------------------------------------- the release hold (--present-latch, AdrenoDisplayLatch.h)

// IOMFBSwapIORequest +0x3d4: the swap ID swap_start_gated assigns (iomgf 0x23794 str w8,[x2,#0x3d4]), monotonic per
// display. H4: with the pointer it names a precopied swap without dereferencing a request the family may have freed.
static inline uint32_t swapID(const IOMFBSwapIORequest *swap)
{
    return *(const uint32_t *)((const uint8_t *)swap + 0x3d4);
}

// H1-H8: at a GO with the hold on, the swap on screen stays current and the pending layer-0 head is copied into the
// GO's frame; the release then completes the former Rb before the next latch and makes the head current without a
// second copy. Returns false where today's tick runs instead: no hold, the beam not locked, not a GO, nothing on screen,
// or a head that is cursor-only or not programmable (H5).
bool AdrenoDisplay::holdLatchGated()
{
    if (!fHold.enabled || !fReleaseTimer || !fBeamRegs || fBeam.state != ADRENO_BEAM_LOCKED || !fBeam.go_row0)
        return false;
    if (fReleaseArmed) {                                   // H7: the release is overdue: it runs first
        fReleaseTimer->cancelTimeout();
        fReleaseArmed = false;
        fHold.rel_overdue++;
        holdReleaseGated(true);
    }
    if (!fCurrentSwap || !fSwapPresented)
        return false;                                      // nothing on screen: today's tick dequeues and copies
    IOMFBSwapIORequest *head = fPendingSwapFirst;
    if (head && (!(IOMFBSwapFields::layerMask(head) & 1) || (IOMFBSwapFields::state(head) & ~8u) != 0 ||
                 (head == fPrecopied && swapID(head) == fPrecopiedID))) {
        fHold.fallbacks++;
        return false;                                      // H5
    }
#if ADRENO_PRESENT_SYNC
    if (!head || copyGatedGated() || syncWaitGated(head)) {   // K8: the precopy asks the same question
#else
    if (!head || copyGatedGated()) {
#endif
        if (head)
            fHold.gated++;                                 // G2: the head waits a latch, the current stays
        else
            adreno_hold_missed(&fHold, mach_absolute_time());  // H1: nothing new for this frame; the current stays
        fBeam.go_row0 = 0;
        armFrameTimerGated();
        holdArmReleaseGated();
        return true;
    }
    fPresentQueuedAt = fPendingSince;                      // H9: q from the head's first sight, as today's dequeue
    fPendingSince = 0;
    if (fPresentQueuedAt && __atomic_load_n(&gAdrenoIBCompletedAt, __ATOMIC_RELAXED) > fPresentQueuedAt)
        fHold.ib_after++;                                  // H6: hardware evidence
    armFrameTimerGated();                                  // the next latch first, as today's tick (in tick: last GO + n)
    fPrecopying = true;
    presentGated(head);                                    // into the GO's frame (adreno_beam_begin takes go_row0)
    fPrecopying = false;
    fPrecopied = head;
    fPrecopiedID = swapID(head);
    fHold.precopies++;
    fBeam.go_row0 = 0;
    holdArmReleaseGated();
    return true;
}

// H1 H2: arm the release Rb before the next latch's fire time; without an armed beam latch there is no hold for this
// frame (the next tick completes the swap, as today); a release time already reached releases at once.
void AdrenoDisplay::holdArmReleaseGated()
{
    if (!fHold.enabled || !fReleaseTimer || !fCurrentSwap || !fSwapPresented)
        return;
    if (fReleaseArmed) {
        fReleaseTimer->cancelTimeout();
        fReleaseArmed = false;
    }
    if (!fFrameTimerArmed || fBeam.state != ADRENO_BEAM_LOCKED || !fBeam.armed_fire || !fBeam.frame)
        return;
    const uint64_t now = mach_absolute_time();
    const uint64_t at = adreno_hold_release_time(&fHold, now, fBeam.armed_fire, fBeam.frame);
    if (at <= now) {
        holdReleaseGated(false);
        return;
    }
    // S6: armed early by the slop 26A428's thread-call timer adds (adreno_vsync_arm_time's rule), never in the past.
    uint64_t lead = (at - now) / 9;
    if (lead > fVsync.lead_max) lead = fVsync.lead_max;
    if (fReleaseTimer->wakeAtTime(0, at - lead, 0) != kIOReturnSuccess) {
        holdReleaseGated(false);
        return;
    }
    fReleaseArmed = true;
}

// H2 H3 H8: complete the swap on screen unless a successor is pending that was not copied yet (the next latch copies it);
// then make the precopied head current, already on the scanout. In submission order: the current first.
void AdrenoDisplay::holdReleaseGated(bool overdue)
{
    (void)overdue;
    fReleaseArmed = false;
    if (!fCurrentSwap || !fSwapPresented)
        return;
    IOMFBSwapIORequest *head = fPendingSwapFirst;
    const bool precopied = head && head == fPrecopied && swapID(head) == fPrecopiedID;
    if (head && !precopied) {
        fHold.rel_deferred++;                              // H3
        return;
    }
    fSwapPresented = false;
    swap_complete_gated();                                 // family: notify_swap_done_gated, release, clear +0x1a40
    if (precopied) {
        fHold.rel_successor++;
        // H4: the family may have dropped it (obsolete, no-op, aborted) meanwhile: only the live head is taken.
        if (cleanQueueGated() && fPendingSwapFirst == fPrecopied && swapID(fPendingSwapFirst) == fPrecopiedID) {
            // swap_begin_gated's dequeue, step for step (current, started, TAILQ_REMOVE with PV's checks).
            IOMFBSwapIORequest *swap = fPendingSwapFirst;
            fCurrentSwap = swap;
            update_swap_info_state_gated(swap, kSwapStateStarted);
            IOMFBSwapIORequest *next = IOMFBSwapFields::queueNext(swap);
            IOMFBSwapIORequest **prev = IOMFBSwapFields::queuePrev(swap);
            if (next) {
                if (IOMFBSwapFields::queuePrev(next) != &IOMFBSwapFields::queueNext(swap) || *prev != swap)
                    panic("AdrenoDisplay: corrupt swap queue at %p", swap);
                IOMFBSwapFields::queuePrev(next) = prev;
            } else {
                if (*prev != swap)
                    panic("AdrenoDisplay: corrupt swap queue tail at %p", swap);
                fPendingSwapLast = prev;
            }
            *prev = next;
            if ((IOMFBSwapFields::state(swap) & ~8u) == 0) {
                fSwapPresented = true;                     // its pixels went out at the latch
                fPresentCount++;
            } else {
                dropSwap_gated(swap, false);               // aborted after its copy: dropped, as swap_begin_gated does
                fDropCount++;
            }
        }
        fPrecopied = nullptr;
    } else {
        fHold.rel_lone++;
    }
    if (!fPendingSwapFirst)
        adreno_hold_released(&fHold, mach_absolute_time());   // H11: R only when nothing else is waiting
    if (!fCurrentSwap && fPendingSwapFirst)
        armFrameTimerGated();                              // H2: a swap still waits for a latch
}

// G1-G3 (b12v11: WindowServer's swap can arrive before its frame's IB completes): a layer-0 head first seen less than
// adreno_gate_us ago is not copied while a GPU submission (IB, FILL32) is in the kext, waiting for the submit lock or
// running. It waits one latch: by the next one it is no longer fresh, so a swap is deferred at most once. Submissions
// IOGPU has queued but not yet handed to the kext are not visible here.
bool AdrenoDisplay::copyGatedGated()
{
    if (!fGateTicks || !fBeamRegs || fBeam.state != ADRENO_BEAM_LOCKED)
        return false;
    IOMFBSwapIORequest *head = fPendingSwapFirst;
    if (!head || !(IOMFBSwapFields::layerMask(head) & 1) || !fPendingSince)
        return false;
    if (!__atomic_load_n(&gAdrenoGPUBusy, __ATOMIC_ACQUIRE))
        return false;
    return mach_absolute_time() - fPendingSince < fGateTicks;
}

// H7: stop and restart: no release, no marker.
void AdrenoDisplay::holdForgetGated()
{
    if (fReleaseTimer && fReleaseArmed)
        fReleaseTimer->cancelTimeout();
    fReleaseArmed = false;
    fPrecopied = nullptr;
    fHold.released_at = 0;
}
#endif

#if ADRENO_PRESENT_SYNC
// ---------------------------------------------------------------- present sync (agents/present-sync, AdrenoDisplaySync.h)

// K1-K4 K11: true when this layer-0 head must stay queued because an IB or FILL32 in the kext still writes its surface.
// The head is named by its pointer and swap ID (+0x3d4), compared and never dereferenced beyond the swap's own fields. No
// clock read unless the surface is marked or a head is waiting. At the bound the head is copied anyway (FORCED, logged).
static inline uint32_t syncSwapID(const IOMFBSwapIORequest *swap)
{
    return *(const uint32_t *)((const uint8_t *)swap + 0x3d4);
}
#if ADRENO_PIPELINE
// agents/gpu-pipeline D1 D3: as below, for every layer the swap carries (layer 0 and the cursor layers 1-3, which are
// captured at the swap): the head stays queued while any of their surfaces has a writer in flight.
bool AdrenoDisplay::syncWaitGated(IOMFBSwapIORequest *head)
{
    const uint8_t mask = head ? IOMFBSwapFields::layerMask(head) : 0;
    if (!fSync.max_wait || !head || !mask || (IOMFBSwapFields::state(head) & ~8u) != 0)
        return false;
    uint32_t writers = 0, written = 0;
    uint64_t oldest = 0;
    for (uint32_t l = 0; l < IOMFBSwapFields::kLayerCount; l++) {
        IOSurface *surface = (mask & (1u << l)) ? IOMFBSwapFields::layerSurface(head, l) : nullptr;
        if (!surface)
            continue;
        uint64_t since = 0;
        const uint32_t w = AdrenoPresentSyncQuery(surface->getSurfaceID(), &since);
        if (!w)
            continue;
        writers += w;
        if (!written) written = surface->getSurfaceID();
        if (since && (!oldest || since < oldest)) oldest = since;
    }
    if (!writers && !fSync.head)
        return false;                                       // the common case: nothing written, nobody waiting
    const uint64_t now = mach_absolute_time();
    const int r = adreno_sync_decide(&fSync, head, syncSwapID(head), writers, oldest, now);
    if (r == ADRENO_SYNC_FORCED && (++fSyncForcedLogged <= 8 || !(fSyncForcedLogged & 255)))
        IOLog("AdrenoDisplay: present sync: copied IOSurface %u with %u GPU writes in flight after the %llu us bound (%llu times)\n",
              written, writers, (unsigned long long)(perfNs(fSync.max_wait) / 1000u),
              (unsigned long long)fSyncForcedLogged);
#if ADRENO_PRESENT_RELATCH
    if (r == ADRENO_SYNC_WAIT && fInTick) {                 // RL4: the head a tick deferred (vsyncGated may relatch it)
        fRelatchCandidate = head;
        fRelatchCandidateID = syncSwapID(head);
    }
#endif
    return r == ADRENO_SYNC_WAIT;
}
#if ADRENO_PRESENT_RELATCH
// ---------------------------------------------------------------- relatch (agents/present-pacing, AdrenoDisplayRelatch.h)

// The writers syncWaitGated asks for (layer 0 and the cursor layers of the swap's mask), without a decision (RL2).
uint32_t AdrenoDisplay::relatchWritersGated(IOMFBSwapIORequest *head)
{
    const uint8_t mask = head ? IOMFBSwapFields::layerMask(head) : 0;
    uint32_t writers = 0;
    for (uint32_t l = 0; l < IOMFBSwapFields::kLayerCount; l++) {
        IOSurface *surface = (mask & (1u << l)) ? IOMFBSwapFields::layerSurface(head, l) : nullptr;
        if (surface)
            writers += AdrenoPresentSyncQuery(surface->getSurfaceID(), nullptr);
    }
    return writers;
}

extern "C" uint32_t gAdrenoPresentUrgent;                  // AdrenoX2.cpp: the completion timer's urgent period (UR1)
void AdrenoDisplay::relatchUrgent(bool on)
{
    __atomic_store_n(&gAdrenoPresentUrgent, on ? 1u : 0u, __ATOMIC_RELAXED);
}

// RL1 RL4: after a tick's swap_begin_gated: the tick's GO chose a frame (go_row0 still set: no layer-0 copy took it),
// nothing is on screen as current, and the sync gate deferred the live head in this tick. Not with the hold (RL5).
void AdrenoDisplay::relatchArmGated()
{
    IOMFBSwapIORequest *head = fPendingSwapFirst;
    if (!fRelatchTimer || !fRelatchCandidate || head != fRelatchCandidate || syncSwapID(head) != fRelatchCandidateID ||
        fCurrentSwap || fHold.enabled || !fBeamRegs || fBeam.state != ADRENO_BEAM_LOCKED || !fBeam.go_row0)
        return;
    const uint64_t now = mach_absolute_time();
    adreno_relatch_arm(&fRelatch, &fBeam, head, fRelatchCandidateID, fBeam.go_row0, now);
    if (fRelatch.enabled)
        relatchUrgent(true);
    relatchScheduleGated(now);
}

// The next poll: every poll ticks while the frame can still take the copy, every millisecond after (histogram only).
// Armed early by the slop 26A428's thread-call timer adds (adreno_vsync_arm_time's rule), never in the past.
void AdrenoDisplay::relatchScheduleGated(uint64_t now)
{
    if (!fRelatchTimer || !fRelatch.pending)
        return;
    uint64_t ms = 0;
    nanoseconds_to_absolutetime(1000000, &ms);
    const uint64_t at = now + (fRelatch.open ? fRelatch.poll : (fRelatch.poll > ms ? fRelatch.poll : ms));
    uint64_t lead = (at - now) / 9;
    if (lead > fVsync.lead_max) lead = fVsync.lead_max;
    if (fRelatchTimer->wakeAtTime(0, at - lead, 0) != kIOReturnSuccess) {
        relatchEndGated(false);
        return;
    }
    fRelatchArmed = true;
}

// RL1-RL3: one poll on the work loop. A released head before the deadline is copied as the GO's copy would have been:
// go_row0 = the GO's frame, swap_begin_gated in tick context (syncWaitGated asks again, every chunk is beam-gated).
void AdrenoDisplay::relatchFireGated()
{
    fRelatchArmed = false;
    if (!fRelatch.pending)
        return;
    IOMFBSwapIORequest *head = fPendingSwapFirst;
    if (fCurrentSwap || !head || !fBeamRegs || fBeam.state != ADRENO_BEAM_LOCKED) {
        fRelatch.gone++;
        relatchEndGated(false);
        return;
    }
    const struct adreno_beam_io io = {beamRead, beamNow, this};
    uint64_t now = 0;
    const uint64_t pos = adreno_beam_sample(&fBeam, &io, &now);
    if (!pos || fBeam.state != ADRENO_BEAM_LOCKED) {
        fRelatch.gone++;
        relatchEndGated(false);
        return;
    }
    int d = adreno_relatch_check(&fRelatch, head, syncSwapID(head), pos, relatchWritersGated(head), now);
    if (d == ADRENO_RELATCH_GO && copyGatedGated()) {      // G1: asked as the tick asks it
        adreno_relatch_gated(&fRelatch);
        d = ADRENO_RELATCH_WAIT;
    }
    if (d == ADRENO_RELATCH_WAIT) {
        if (!fRelatch.open)
            relatchUrgent(false);                           // the frame has gone: only the histogram waits
        relatchScheduleGated(now);
        return;
    }
    relatchUrgent(false);
    if (d != ADRENO_RELATCH_GO)
        return;
    const uint64_t row0 = fRelatch.row0, ovt = fBeam.ovt, since = fRelatch.since;
    const bool tick = fInTick;
    fBeam.go_row0 = row0;
    fInTick = true;
    swap_begin_gated();
    fInTick = tick;
#if ADRENO_PRESENT_EARLY_DONE
    // the present took the GO's frame (a swap completed at its copy is no longer current, its screen is held)
    const bool copied = !fBeam.go_row0 && ((fCurrentSwap && fSwapPresented) || fScreenHeld);
#else
    const bool copied = !fBeam.go_row0 && fCurrentSwap && fSwapPresented;   // the present took the GO's frame
#endif
    fBeam.go_row0 = 0;
    adreno_relatch_copied(&fRelatch, copied, row0 > pos ? adreno_beam_ticks(&fBeam, row0 - pos) : 0,
                          now > since ? now - since : 0, fBeam.ovt - ovt);
}

// End a pending relatch: at a tick that proceeds (tick: its histogram bin), or a stop, a mode set, a failed arm.
void AdrenoDisplay::relatchEndGated(bool tick)
{
    if (fRelatch.pending) {
        if (tick)
            adreno_relatch_tick(&fRelatch);
        else
            adreno_relatch_cancel(&fRelatch);
    }
    if (fRelatchArmed && fRelatchTimer)
        fRelatchTimer->cancelTimeout();
    fRelatchArmed = false;
    relatchUrgent(false);
}
#endif
#else
bool AdrenoDisplay::syncWaitGated(IOMFBSwapIORequest *head)
{
    if (!fSync.max_wait || !head || !(IOMFBSwapFields::layerMask(head) & 1) || (IOMFBSwapFields::state(head) & ~8u) != 0)
        return false;
    IOSurface *surface = IOMFBSwapFields::layerSurface(head, 0);
    if (!surface)
        return false;
    uint64_t oldest = 0;
    const uint32_t writers = AdrenoPresentSyncQuery(surface->getSurfaceID(), &oldest);
    if (!writers && !fSync.head)
        return false;                                       // the common case: nothing written, nobody waiting
    const uint64_t now = mach_absolute_time();
    const int r = adreno_sync_decide(&fSync, head, syncSwapID(head), writers, oldest, now);
    if (r == ADRENO_SYNC_FORCED && (++fSyncForcedLogged <= 8 || !(fSyncForcedLogged & 255)))
        IOLog("AdrenoDisplay: present sync: copied IOSurface %u with %u GPU writes in flight after the %llu us bound (%llu times)\n",
              surface->getSurfaceID(), writers, (unsigned long long)(perfNs(fSync.max_wait) / 1000u),
              (unsigned long long)fSyncForcedLogged);
    return r == ADRENO_SYNC_WAIT;
}
#endif
#endif
