// SPDX-License-Identifier: MIT
// The swap engine's front: the family's swap hooks, the pending queue, swap_begin and the frame tick.
// Part of AdrenoDisplay.cpp's translation unit: included from it in order, never compiled on its own.

// ---------------------------------------------------------------- swap engine

bool AdrenoDisplay::verify_swap(IOMFBSwapIORequest *)
{
    static uint64_t verifies = 0;
    if (++verifies <= 8)
        IOLog("AdrenoDisplay: verify_swap #%llu\n", verifies);
    return true;                                                // PV 0x125e0 (the base returns false)
}

IOReturn AdrenoDisplay::swap_layer_map(IOMFBSwapIORequest *, unsigned int layer)
{
    static uint64_t calls = 0;
    if (++calls <= 16)
        IOLog("AdrenoDisplay: swap_layer_map layer %u\n", layer);
    return kIOReturnSuccess;                                    // PV 0x126e4
}

IOReturn AdrenoDisplay::surface_layer_map(IOMFB::AP::DisplayedData *, unsigned int layer, IOSurface *surface)
{
    static uint64_t calls = 0;
    if (++calls <= 16)
        IOLog("AdrenoDisplay: surface_layer_map layer %u surface %p\n", layer, surface);
    return kIOReturnSuccess;                                    // PV 0x126f0
}

void AdrenoDisplay::surface_layer_unmap(IOMFB::AP::DisplayedData *, unsigned int)
{
    // PV 0x126fc: nothing.
}

bool AdrenoDisplay::cleanQueueGated()
{
    // PV clean_queue_gated 0x119a8: nothing while a swap is on screen; otherwise let the
    // family discard obsolete, no-op, aborted and dummy swaps, and report what is left.
    if (fCurrentSwap)
        return false;
#if ADRENO_PRESENT_EARLY_DONE
    // A swap completed at its copy is on screen until the next proceeding tick: arrivals queue as behind a current one.
    if (fScreenHeld)
        return false;
#endif
    if (!fPendingSwapFirst)
        return false;
    if (dropObsoleteSwaps_gated())
        return false;
    dropNoOpSwaps_gated();
    dropAbortedSwaps_gated(false);
    dropDummySwaps_gated();
    return fPendingSwapFirst != nullptr;
}

void AdrenoDisplay::swap_begin_gated()
{
    static uint64_t begins = 0;
    if (++begins <= 8)
        IOLog("AdrenoDisplay: swap_begin #%llu pending %s current %s\n", begins,
              fPendingSwapFirst ? "yes" : "no", fCurrentSwap ? "yes" : "no");
    // PV swap_begin_gated 0x11aac, with one change: PV cancels and re-arms its timer on every
    // call because the host completes swaps; here the timer *is* the completion, so it is
    // armed once per frame and never pushed back by a stream of submissions.
#if ADRENO_BACKLIGHT
    backlightSwapGated();   // a brightness the submitted swaps carried (backlight.cpp): one atomic load if none
#endif
#if ADRENO_PERF_FRAMES
    // F1: one clock read when a swap is first seen pending on the submission path.
    if (!fInTick && fPendingSwapFirst && !fPendingSince) {
        fPendingSince = mach_absolute_time();
#if ADRENO_PRESENT_EARLY_DONE
        // Its arrival against the last tick, when it is the only one pending (else WindowServer is ahead of the display).
        if (IOMFBSwapFields::layerMask(fPendingSwapFirst) & 1)
            adreno_early_arrival(&fEarly, fPendingSince, !IOMFBSwapFields::queueNext(fPendingSwapFirst));
#endif
    }
#endif
#if ADRENO_LATCH
    // H1 H11: R, from the last release to the first arrival after it (F1's clock read; nothing if none is pending).
    if (!fInTick && fPendingSince && fHold.released_at) adreno_hold_arrival(&fHold, fPendingSince);
#endif
#if ADRENO_BEAM
    // C6: with the beam locked, a layer-0 swap that finds nothing on screen outside a tick waits for the next vblank
    // (the timer is armed for it) instead of being copied at a random phase; cursor-only swaps still go at once (C7).
    if (!fInTick && !fCurrentSwap && fBeamRegs && fBeam.state == ADRENO_BEAM_LOCKED && cleanQueueGated() &&
        (IOMFBSwapFields::layerMask(fPendingSwapFirst) & 1)) {
        armFrameTimerGated();
        return;
    }
#endif
    while (cleanQueueGated()) {
#if ADRENO_PRESENT_SYNC
        // K1 K3 K4: a layer-0 head whose surface the GPU is still writing stays queued; the timer below is armed for it
        // (beam GO or grid), the swap on screen stays as it is. Nothing is written to the scanout.
        if (syncWaitGated(fPendingSwapFirst))
            break;
#endif
        IOMFBSwapIORequest *swap = fPendingSwapFirst;
        fCurrentSwap = swap;
        update_swap_info_state_gated(swap, kSwapStateStarted);
        // TAILQ_REMOVE(pending, swap), with the same consistency checks PV traps on.
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
            uint8_t layers = IOMFBSwapFields::layerMask(swap);
            // The frame tick is armed before the copy (docs/USERSPACE-GRAPHICS-20261019.md, "Speed batch"): the
            // period is then max(copy, kFramePeriodMS), not copy + kFramePeriodMS. The tick runs on this work loop,
            // so it can never complete a swap before its copy has returned.
            armFrameTimerGated();
#if ADRENO_PERF_FRAMES
            fPresentQueuedAt = fPendingSince;
            fPendingSince = 0;
#endif
#if ADRENO_LATCH
            if (swap == fPrecopied) fPrecopied = nullptr;   // H4: dequeued by today's path: its marker goes
#endif
            presentGated(swap);
            // PV program_hardware_gated 0x11934..0x119a4: a swap that programs no layer-0
            // surface is completed at once (tail call through vtable +0xd00,
            // swap_complete_gated), and PV's swap_begin_gated loop (0x11bc8 clean_queue_gated,
            // 0x11bd0 back to the dequeue) takes the next swap in the same call. Done here for
            // cursor-only swaps, so they never hold a frame tick ahead of a layer-0 swap; every
            // other swap still completes on the tick as before. swap_complete_gated (iomgf
            // 0x210a0) does not re-enter swap_begin_gated.
            if (!(layers & 1) && (layers & kOverlayLayers)) {
                fSwapPresented = false;
                fCursorImmediate++;
                swap_complete_gated();
                continue;
            }
#if ADRENO_PRESENT_EARLY_DONE
            // Its pixels are in the scanout and nothing reads its surface again: WindowServer gets the completion now,
            // the screen stays held (fScreenHeld) until the next tick, which then has nothing to complete.
            if ((layers & 1) && fSwapPresented && earlyDoneNowGated()) {
                fSwapPresented = false;
                fScreenHeld = true;
                adreno_early_completed(&fEarly);
                swap_complete_gated();
            }
#endif
            break;                       // on screen until the next frame tick completes it
        }
        dropSwap_gated(swap, false);     // PV: aborted/rejected swaps are dropped, not shown
        fDropCount++;
    }
    armFrameTimerGated();
}

#if ADRENO_PRESENT_EARLY_DONE
// Whether the layer-0 swap swap_begin_gated just copied is completed now (AdrenoDisplayEarlyDone.h): mode 1 always, mode 2
// while a late frame engaged it (one clock read), never with the release hold, whose precopy keeps the swap current.
bool AdrenoDisplay::earlyDoneNowGated()
{
    if (fEarly.mode == ADRENO_EARLY_OFF)
        return false;
#if ADRENO_LATCH
    if (fHold.enabled || fPrecopying)
        return false;
#endif
    return adreno_early_active(&fEarly, fEarly.mode == ADRENO_EARLY_ALWAYS ? 0 : mach_absolute_time());
}
#endif

void AdrenoDisplay::armFrameTimerGated()
{
    if (!fFrameTimer || fFrameTimerArmed)
        return;
    if (!fCurrentSwap && !fPendingSwapFirst)
        return;
#if ADRENO_BEAM
    if (beamArmGated())
        return;                              // armed on the panel's real vblank; otherwise the grid below
#endif
#if ADRENO_VSYNC60
    // S1 S2 S6: the next grid point (never in the past, missed points skipped, re-sync after a long idle), armed
    // early by the slop the thread-call timer will add. No kIOTimeOptionsWithLeeway: 26A428 would use
    // max(leeway, coalescing slop), which can only be later. One clock read per arm, as before.
    const uint64_t now = mach_absolute_time();
    const uint64_t at = adreno_vsync_arm_time(&fVsync, now, adreno_vsync_target(&fVsync, now, fInTick));
    if (fFrameTimer->wakeAtTime(0, at, 0) != kIOReturnSuccess) {
        adreno_vsync_arm_failed(&fVsync);  // S4: not armed; the next swap arms again
        return;
    }
    fFrameTimerArmed = true;
    fArmedAt = now;
#else
    fFrameTimerArmed = true;
#if ADRENO_PERF_FRAMES
    fArmedAt = mach_absolute_time();       // F9: one clock read per arm
#endif
    fFrameTimer->setTimeoutMS(kFramePeriodMS);
#endif
}

void AdrenoDisplay::vsyncGated(IOTimerEventSource *)
{
    // Frame tick (emulated vsync): the swap copied during the last frame is now "displayed".
#if ADRENO_PERF_FRAMES
    const uint64_t now = mach_absolute_time(), armed = fArmedAt, copies = fCopyTimed;   // F1: one read per tick
#if ADRENO_PRESENT_EARLY_DONE
    const bool completed = (fCurrentSwap && fSwapPresented) || fScreenHeld;
#else
    const bool completed = fCurrentSwap && fSwapPresented;
#endif
#if ADRENO_VSYNC60
    const uint64_t grid = adreno_vsync_fired(&fVsync, now);   // S7 S8: before swap_begin_gated re-arms from it
    (void)armed;
#endif
#endif
    fFrameTimerArmed = false;
#if ADRENO_BEAM
    if (beamFireGated())
        return;                          // C1 C2: re-armed for its vblank; nothing completed or copied
#endif
#if ADRENO_PRESENT_RELATCH
    relatchEndGated(true);               // RL3 RL4: a tick that proceeds ends a pending relatch (its frame has gone)
    fRelatchCandidate = nullptr;
#endif
#if ADRENO_LATCH
    // H1-H8: a GO with the hold on keeps the swap on screen current and copies the pending one; otherwise today's tick,
    // after which a layer-0 swap it put on screen gets its release (and a release armed for the swap it completed goes).
    fInTick = true;
    const bool held = holdLatchGated();
    fInTick = false;
    if (!held) {
        if (fReleaseArmed && fCurrentSwap && fSwapPresented) {
            fReleaseTimer->cancelTimeout();
            fReleaseArmed = false;
        }
#endif
    if (fCurrentSwap && fSwapPresented) {
        fSwapPresented = false;
        swap_complete_gated();           // family: notify_swap_done_gated, release, clear +0x1a40
    }
#if ADRENO_PRESENT_EARLY_DONE
    fScreenHeld = false;                 // the refresh of a swap completed at its copy ends here, as a completion would
#endif
#if ADRENO_PERF_FRAMES
    fInTick = true;
#endif
#if ADRENO_LATCH
    if (copyGatedGated()) {              // G1-G3: a fresh layer-0 swap waits one latch while the GPU has work
        fHold.gated++;
        armFrameTimerGated();
    } else
#endif
    swap_begin_gated();
#if ADRENO_PRESENT_RELATCH
    relatchArmGated();                   // RL4: this tick's sync gate deferred its head and nothing took the GO's frame
#endif
#if ADRENO_BEAM
    fBeam.go_row0 = 0;                   // a GO whose tick copied nothing: its frame is not kept for a later present
#endif
#if ADRENO_LATCH
        fInTick = false;
        if (fHold.enabled && fCurrentSwap && fSwapPresented && fBeamRegs && fBeam.state == ADRENO_BEAM_LOCKED) {
            fHold.fallbacks++;
            holdArmReleaseGated();
        }
    }
#endif
#if ADRENO_PRESENT_EARLY_DONE
    {   // A layer-0 head still queued after the tick was deferred (present sync, copy gate): not a frame WindowServer missed.
        const bool waiting = fPendingSwapFirst && (IOMFBSwapFields::layerMask(fPendingSwapFirst) & 1);
        adreno_early_tick(&fEarly, now, completed, fCopyTimed != copies || waiting);
    }
#endif
#if ADRENO_PERF_FRAMES
    fInTick = false;
    // F6: ready when a layer-0 copy started in this tick.
#if ADRENO_VSYNC60
    // S8: late against the grid point, on the 16.667 ms grid.
    adreno_pacing_tick(&fPacing, perfNs(now), grid ? perfNs(grid - fVsync.period) : 0, perfNs(fVsync.period),
                       completed, fCopyTimed != copies);
#else
    adreno_pacing_tick(&fPacing, perfNs(now), armed ? perfNs(armed) : 0, (uint64_t)kFramePeriodMS * 1000000u,
                       completed, fCopyTimed != copies);
#endif
#endif
}
