// SPDX-License-Identifier: MIT
// The hardware: start_hardware and stop_hardware_gated, the digital-out mode, the dot pitch and the scanout mapping.
// Part of AdrenoDisplay.cpp's translation unit: included from it in order, never compiled on its own.

// ---------------------------------------------------------------- hardware

bool AdrenoDisplay::start_hardware()
{
    // PV start_hardware 0xe9e8: under the family's command gate, set up the device, set the
    // four "display on" words at +0x1c0c..+0x1c18 to 1, create a 1-shot timer whose action
    // calls swap_begin_gated, add it to the work loop, enable it.
    if (!fLegacyCommandGate)
        return false;
    IOReturn ret = fLegacyCommandGate->runActionBlock(^IOReturn {
        if (!mapScanout())
            return kIOReturnNoDevice;
        forgetDamageGated();
        uint32_t *on = (uint32_t *)((uint8_t *)this + 0x1c0c);
        on[0] = on[1] = on[2] = on[3] = 1;
        fDisplayWidth = kWidth;
        fDisplayHeight = kHeight;
        // AppleParavirtDisplay's set_digital_out_mode writes the selected geometry into the Legacy
        // ivars at +0x1bf0/+0x1bf4 (0xa268, 0xa270), which Legacy::get_display_size reads back.
        // Without them the mode the family reports stays empty and Core Animation calls the display
        // unready: "current mode (16383 x 16383)", "disables updates", no swap.
        *(uint32_t *)((uint8_t *)this + 0x1bf0) = kWidth;
        *(uint32_t *)((uint8_t *)this + 0x1bf4) = kHeight;
        // Legacy keeps its hotplug state (what get_digital_out_state returns) at +0x1db8, zeroed at
        // init and written by set_hotPlug_state_gated (0x28b4c). A zero there says the panel is not
        // attached, which is the other half of Core Animation treating the display as unready.
        *(uint32_t *)((uint8_t *)this + 0x1db8) = 1;
#if ADRENO_VSYNC60
        // S5: the grid in abstime, once and in 64 bits, before the timer exists.
        uint64_t period = 0, lead = 0;
        nanoseconds_to_absolutetime(kVsyncPeriodNs, &period);
        nanoseconds_to_absolutetime(kVsyncLeadMaxNs, &lead);
        adreno_vsync_init(&fVsync, period, lead, 4 * period);
#endif
#if ADRENO_BEAM
        beamMapGated();                  // never fails the start: a refusal keeps the timer path (B1 D3)
#endif
#if ADRENO_LATCH
        {   // L5: adreno_latch_us=0 is today's aim, adreno_hold=0 today's completion at the tick; both 0 = the beam kext
            uint32_t slackUs = 3000, hold = 0, marginUs = 1500, gateUs = 4000;   // b12v11: the hold is opt-in
            PE_parse_boot_argn("adreno_latch_us", &slackUs, sizeof(slackUs));
            PE_parse_boot_argn("adreno_gate_us", &gateUs, sizeof(gateUs));
            if (gateUs > 16000) gateUs = 16000;
            fGateTicks = 0;
            nanoseconds_to_absolutetime((uint64_t)gateUs * 1000u, &fGateTicks);
            PE_parse_boot_argn("adreno_hold", &hold, sizeof(hold));
            PE_parse_boot_argn("adreno_hold_margin_us", &marginUs, sizeof(marginUs));
            if (slackUs > 9000) slackUs = 9000;
            if (marginUs > 16000) marginUs = 16000;
            fLatchSlackUs = slackUs;
            fHoldMarginUs = marginUs;
            uint64_t slack = 0, margin = 0, floor = 0;
            nanoseconds_to_absolutetime((uint64_t)slackUs * 1000u, &slack);
            nanoseconds_to_absolutetime((uint64_t)marginUs * 1000u, &margin);
            nanoseconds_to_absolutetime((uint64_t)ADRENO_HOLD_FLOOR_US * 1000u, &floor);
            adreno_beam_latch(&fBeam, slack);
            adreno_hold_init(&fHold, hold != 0, margin, floor);
            fReleaseArmed = fPrecopying = false;
            fPrecopied = nullptr;
            IOLog("AdrenoDisplay: latch slack %u us%s, release hold %s (margin %u us), copy gate %u us\n", slackUs,
                  slackUs ? "" : " (today's aim)", hold ? "on" : "off", marginUs, gateUs);
        }
#endif
#if ADRENO_PRESENT_SYNC
        {   // K2: the bound per swap (0 = off); marks older than 2 s are lost ones
#if ADRENO_PIPELINE
            // agents/gpu-pipeline D2: a swap now waits for its whole IB; only a lost unmark may reach the bound
            uint32_t maxUs = 1100000;
            PE_parse_boot_argn("adreno_sync_max_us", &maxUs, sizeof(maxUs));
            if (maxUs > 1999000) maxUs = 1999000;
            AdrenoPipelineCounters(fPipeMark);
#else
            uint32_t maxUs = 50000;
            PE_parse_boot_argn("adreno_sync_max_us", &maxUs, sizeof(maxUs));
            if (maxUs > 500000) maxUs = 500000;
#endif
            uint64_t maxWait = 0, stale = 0;
            nanoseconds_to_absolutetime((uint64_t)maxUs * 1000u, &maxWait);
            nanoseconds_to_absolutetime(2000000000ull, &stale);
            adreno_sync_gate_init(&fSync, maxWait, stale);
            fSyncForcedLogged = 0;
            AdrenoPresentSyncCounters(&fSyncMark[0], &fSyncMark[1], &fSyncMark[2]);
            IOLog("AdrenoDisplay: present sync %s: a layer-0 copy waits for GPU writes to its surface, at most %u us\n",
                  maxUs ? "on" : "off", maxUs);
        }
#endif
#if ADRENO_PRESENT_SKIP_MAINT
        {
            uint32_t keep = 0;
            fPresentMaint = PE_parse_boot_argn("adreno_present_maint", &keep, sizeof(keep)) && keep;
            IOLog("AdrenoDisplay: layer-0 maintenance pass %s\n", fPresentMaint ? "kept (adreno_present_maint=1)" :
                  ADRENO_BEAM ? "skipped while the beam is locked: the resource list invalidates GPU-written surfaces" :
                  "skipped: the resource list invalidates GPU-written surfaces");
        }
#endif
        fFrameTimer = IOTimerEventSource::timerEventSource(kIOTimerEventSourceOptionsPriorityKernel, this, ^(IOTimerEventSource *sender) {
            vsyncGated(sender);
        });
        if (!fFrameTimer || getWorkLoop()->addEventSource(fFrameTimer) != kIOReturnSuccess) {
            OSSafeReleaseNULL(fFrameTimer);
            return kIOReturnNoResources;
        }
        fFrameTimer->enable();
#if ADRENO_LATCH
        // H2 H7: the release, on the same work loop as the tick. Without it the hold stays off (today's completion).
        fReleaseTimer = IOTimerEventSource::timerEventSource(kIOTimerEventSourceOptionsPriorityKernel, this,
                                                             ^(IOTimerEventSource *) { holdReleaseGated(false); });
        if (fReleaseTimer && getWorkLoop()->addEventSource(fReleaseTimer) != kIOReturnSuccess)
            OSSafeReleaseNULL(fReleaseTimer);
        if (fReleaseTimer)
            fReleaseTimer->enable();
        else
            fHold.enabled = 0;
#endif
#if ADRENO_PRESENT_EARLY_DONE
        {   // adreno_present_early_done: 0 measures only (the default), 1 always, 2 while behind for the hold; never with
            // the release hold (its precopy keeps the swap current until the release)
            uint32_t mode = ADRENO_EARLY_OFF, holdMs = 2000;
            PE_parse_boot_argn("adreno_present_early_done", &mode, sizeof(mode));
            PE_parse_boot_argn("adreno_present_early_hold_ms", &holdMs, sizeof(holdMs));
            if (mode > ADRENO_EARLY_ADAPTIVE) mode = ADRENO_EARLY_OFF;
            if (holdMs < 100) holdMs = 100;
            if (holdMs > 10000) holdMs = 10000;
#if ADRENO_LATCH
            const bool hold = fHold.enabled;
#else
            const bool hold = false;
#endif
            uint64_t period = 0, holdTicks = 0, ms = 0;
            nanoseconds_to_absolutetime(kVsyncPeriodNs, &period);
            nanoseconds_to_absolutetime((uint64_t)holdMs * 1000000u, &holdTicks);
            nanoseconds_to_absolutetime(1000000, &ms);
            adreno_early_init(&fEarly, hold ? (uint32_t)ADRENO_EARLY_OFF : mode, period, holdTicks, ms);
            fScreenHeld = false;
            IOLog("AdrenoDisplay: early completion %s (mode %u%s, hold %u ms); arrivals measured\n",
                  fEarly.mode == ADRENO_EARLY_ALWAYS ? "at every copy" : fEarly.mode == ADRENO_EARLY_ADAPTIVE ?
                  "while WindowServer is behind" : "off", mode, hold && mode ? ", off with the release hold" : "", holdMs);
        }
#endif
#if ADRENO_PRESENT_RELATCH
        {   // RL1 RL6 OP3: adreno_relatch=0 measures only; the poll and the guard in mach ticks
            uint32_t relatch = 1, pollUs = 200, guardUs = 1000;
            PE_parse_boot_argn("adreno_relatch", &relatch, sizeof(relatch));
            PE_parse_boot_argn("adreno_relatch_poll_us", &pollUs, sizeof(pollUs));
            PE_parse_boot_argn("adreno_relatch_guard_us", &guardUs, sizeof(guardUs));
            if (pollUs < 50) pollUs = 50;
            if (pollUs > 2000) pollUs = 2000;
            if (guardUs > 8000) guardUs = 8000;
            uint64_t poll = 0, guard = 0, ms = 0;
            nanoseconds_to_absolutetime((uint64_t)pollUs * 1000u, &poll);
            nanoseconds_to_absolutetime((uint64_t)guardUs * 1000u, &guard);
            nanoseconds_to_absolutetime(1000000, &ms);
            adreno_relatch_init(&fRelatch, relatch != 0, poll, guard, ms);
            fRelatchArmed = false;
            fRelatchCandidate = nullptr;
            fRelatchTimer = IOTimerEventSource::timerEventSource(kIOTimerEventSourceOptionsPriorityKernel, this,
                                                                 ^(IOTimerEventSource *) { relatchFireGated(); });
            if (fRelatchTimer && getWorkLoop()->addEventSource(fRelatchTimer) != kIOReturnSuccess)
                OSSafeReleaseNULL(fRelatchTimer);
            if (fRelatchTimer)
                fRelatchTimer->enable();
            AdrenoPipelineEarlyCounters(fRelatchEarlyMark);
            IOLog("AdrenoDisplay: relatch %s (poll %u us, guard %u us)\n",
                  !fRelatchTimer ? "off (no timer)" : relatch ? "on" : "measuring only", pollUs, guardUs);
        }
#endif
        // The user-client GetCanvasSizes selector answers kIOReturnBadArgument (0xe00002c2 in the
        // guest canvas probe, all four words zero) while the display has no canvas; set the panel's.
        IOMobileFramebufferLegacy::set_canvas_size(kWidth, kHeight);
        if (!publishDisplayAttributes())
            return kIOReturnNoMemory;
        return kIOReturnSuccess;
    });
    if (ret != kIOReturnSuccess)
        IOLog("AdrenoDisplay: start_hardware failed %08x\n", ret);
    return ret == kIOReturnSuccess;
}

void AdrenoDisplay::stop_hardware_gated()
{
    // PV stop_hardware_gated 0xee30: disable, cancel, remove and release the timer.
    if (fFrameTimer) {
        fFrameTimer->disable();
        fFrameTimer->cancelTimeout();
        getWorkLoop()->removeEventSource(fFrameTimer);
        OSSafeReleaseNULL(fFrameTimer);
    }
    fFrameTimerArmed = false;
#if ADRENO_PRESENT_SYNC
    adreno_sync_forget(&fSync);              // K11: no waiting head across a stop
#endif
#if ADRENO_LATCH
    holdForgetGated();                       // H7: no release after the stop
    if (fReleaseTimer) {
        fReleaseTimer->disable();
        getWorkLoop()->removeEventSource(fReleaseTimer);
        OSSafeReleaseNULL(fReleaseTimer);
    }
#endif
#if ADRENO_PRESENT_RELATCH
    relatchEndGated(false);                  // RL3 UR1: no relatch after the stop, the urgent flag cleared
    fRelatchCandidate = nullptr;
    if (fRelatchTimer) {
        fRelatchTimer->disable();
        fRelatchTimer->cancelTimeout();
        getWorkLoop()->removeEventSource(fRelatchTimer);
        OSSafeReleaseNULL(fRelatchTimer);
    }
#endif
#if ADRENO_VSYNC60
    adreno_vsync_reset(&fVsync);             // S4: a restarted display starts a new grid
#endif
#if ADRENO_PRESENT_EARLY_DONE
    adreno_early_reset(&fEarly);             // no tick to measure arrivals from, nothing engaged, nothing held
    fScreenHeld = false;
#endif
#if ADRENO_BEAM
    adreno_beam_reset(&fBeam);               // D1: nothing armed; the next start maps and calibrates again
    beamUnmap();                             // after the timer is gone, so no fire can read the page
#endif
    releaseSurfaceMaps();
    fCursor.visible = fCursor.drawn = 0;     // a restarted display starts without a cursor
    forgetDamageGated();                     // and with every row written by its first frame
    withdrawDisplayAttributes();
}

#if ADRENO_DISPLAY_TRACE_MODES
IOReturn AdrenoDisplay::digitalOutMode(unsigned int colorID, unsigned int timingID)   // traced by set_digital_out_mode
#else
IOReturn AdrenoDisplay::set_digital_out_mode(unsigned int colorID, unsigned int timingID)
#endif
{
    // The native API and selector23 pass color first, timing second. Four
    // generation55 WindowServer corpses retain the request (100, 1000).
    if (!fLegacyCommandGate)
        return kIOReturnNotReady;
    return fLegacyCommandGate->runActionBlock(^IOReturn {
        if (!fAttributesPublished || !fTimingElements || !fColorElements)
            return kIOReturnNotReady;
        // Only the published pair exists: the 8-bit SDR color element on the one timing.
        if (timingID != kTimingID || colorID != kColorIDSDR8)
            return kIOReturnNotFound;
        // PV's successful gated path (0xa264..0xa340) publishes its selected
        // geometry/timing and returns success. Its Legacy fallback is only for
        // failure: Legacy::set_digital_out_mode always returns Unsupported.
        // This panel has one fixed scanout mode; all published colors use it.
        if (!setProperty("DisplayWidth", kWidth, 32) ||
            !setProperty("DisplayHeight", kHeight, 32) ||
            !setProperty("DPTimingModeId", timingID, 64))
            return kIOReturnNoMemory;
        fDisplayWidth = kWidth;
        fDisplayHeight = kHeight;
        *(uint32_t *)((uint8_t *)this + 0x1bf0) = kWidth;    // Legacy::get_display_size reads these two
        *(uint32_t *)((uint8_t *)this + 0x1bf4) = kHeight;
        forgetDamageGated();         // the first frame after a mode set is written in full
#if ADRENO_VSYNC60
        adreno_vsync_reset(&fVsync); // S4: and the tick grid restarts
#endif
#if ADRENO_BEAM
        adreno_beam_reset(&fBeam);   // D2: and the beam recalibrates at the next arm
#endif
#if ADRENO_LATCH
        fPrecopied = nullptr;        // H4: a precopied swap is copied again at its latch
#endif
#if ADRENO_PRESENT_RELATCH
        relatchEndGated(false);      // RL3: the beam's positions are renumbered at the recalibration
#endif
#if ADRENO_PRESENT_EARLY_DONE
        adreno_early_reset(&fEarly); // and the arrivals measure from the new grid's first tick
#endif
        return kIOReturnSuccess;
    });
}

IOReturn AdrenoDisplay::get_dot_pitch_float(float *pitch)
{
    // Legacy returns kIOReturnUnsupported for this slot [295]; report the same pixels-per-inch
    // as get_dot_pitch, as a float, for clients that ask the float form.
    if (!pitch)
        return kIOReturnBadArgument;
    *pitch = (float)(kWidth * 254 + kPanelWidthMM * 5) / (float)(kPanelWidthMM * 10);
    return kIOReturnSuccess;
}

IOReturn AdrenoDisplay::get_dot_pitch(unsigned int *pitch)
{
    // IOMobileFramebufferLegacy::start (iomgf 0x1f120..0x1f128) keeps
    // OSDynamicCast(AppleARMIODevice, provider) at +0x1b80 and only logs when it is NULL.
    // Legacy::get_dot_pitch (0x29b00..0x29b04) then does ldr x0,[x0,#0x1b80]; ldr x16,[x0]
    // with no check, and get_dot_pitch_float (0x29c0c) and get_display_area (0x29c80) call
    // it through the vtable, all reachable from user-client selectors. Started with the
    // accelerator as provider, that pointer is NULL, so the base must never run. PV overrides
    // it too (0xf494: pixels / mm * 25.4 rounded), falling back to the base only with a
    // real AppleARMIODevice provider.
    if (!pitch)
        return kIOReturnBadArgument;
    *pitch = (kWidth * 254 + kPanelWidthMM * 5) / (kPanelWidthMM * 10);   // pixels per inch
    return kIOReturnSuccess;
}

bool AdrenoDisplay::mapScanout()
{
    if (fScanout)
        return true;
    // PE_current_console/PE_state are not exported to kexts on 26A428 (the checker's export
    // scan); IOBootFramebuffer's route is the platform expert's virtual getConsoleInfo.
    PE_Video video = {};
    IOPlatformExpert *pe = getPlatform();
    if (!pe || pe->getConsoleInfo(&video) != kIOReturnSuccess || !video.v_baseAddr || !video.v_rowBytes) {
        IOLog("AdrenoDisplay: no boot framebuffer in PE_Video\n");
        return false;
    }
    fScanoutRowBytes = (uint32_t)video.v_rowBytes;
    fScanoutWidth = (uint32_t)video.v_width;
    fScanoutHeight = (uint32_t)video.v_height;
    fScanoutDepth = (uint32_t)video.v_depth;
    if (fScanoutWidth < kWidth || fScanoutHeight < kHeight || fScanoutDepth != 32)
        IOLog("AdrenoDisplay: firmware framebuffer %ux%u@%u differs from the %ux%u mode\n",
              fScanoutWidth, fScanoutHeight, fScanoutDepth, kWidth, kHeight);
    uint64_t length = (uint64_t)fScanoutRowBytes * fScanoutHeight;
    uint64_t addr = video.v_baseAddr + video.v_offset;
    fScanoutLength = length;
    if ((addr >> 48) == 0xffff) {
        // Already a kernel virtual address (the console's own mapping).
        fScanout = (volatile uint8_t *)addr;
        fScanoutPhys = 0;
        return true;
    }
    // arm64 PE_state.video.v_baseAddr is boot_args->Video.v_baseAddr, a physical address
    // (A16 GOP: 0xf9a20000-ish, 11520 bytes per row). Map it write-combined.
    fScanoutPhys = addr;
    fScanoutMD = IOMemoryDescriptor::withPhysicalAddress(addr, length, kIODirectionInOut);
    if (fScanoutMD)
        fScanoutMap = fScanoutMD->createMappingInTask(kernel_task, 0,
                                                      kIOMapAnywhere | kIOMapWriteCombineCache, 0, length);
    if (!fScanoutMap) {
        IOLog("AdrenoDisplay: cannot map framebuffer %llx+%llx\n", addr, length);
        OSSafeReleaseNULL(fScanoutMD);
        return false;
    }
    fScanout = (volatile uint8_t *)fScanoutMap->getVirtualAddress();
    fScanoutAttr = mappingAttribute((const void *)(uintptr_t)fScanout);
    setProperty("AdrenoScanoutPhysical", fScanoutPhys, 64);
    setProperty("AdrenoScanoutRowBytes", fScanoutRowBytes, 32);
    return true;
}

void AdrenoDisplay::unmapScanout()
{
    forgetDamageGated();
    fScanout = nullptr;
    fScanoutLength = 0;
    OSSafeReleaseNULL(fScanoutMap);
    OSSafeReleaseNULL(fScanoutMD);
}

// Block type 0x41 is what IOMobileFramebufferGetDigitalOutMode reads: IOMobileFramebuffer's
// 0x18feff424 dispatches 0x41 with a 44-byte output buffer and, on success, copies the two u32 at
// +4 and +8 into its colour and timing out-parameters. The family's slot [333] is an inline
// kIOReturnUnsupported stub, so WindowServer's "fetching current digital mode" fails until the
// published pair comes back.
static IOReturn adrenoDigitalOutModeBlock(unsigned int type, unsigned int index, unsigned char *data,
                                          unsigned long bytes)
{
    if (type != 0x41 || index != 0)
        return kIOReturnUnsupported;
    if (!data || bytes < 44)
        return kIOReturnBadArgument;
    for (unsigned long i = 0; i < 44; i++)
        data[i] = 0;
    ((uint32_t *)data)[1] = 100;    // the published colour element (kColorIDSDR8)
    ((uint32_t *)data)[2] = 1000;   // the one published timing (kTimingID)
    return kIOReturnSuccess;
}

#if !ADRENO_DISPLAY_TRACE_MODES
IOReturn AdrenoDisplay::set_block(task *a0, unsigned int a1, unsigned int a2, unsigned long long const *a3,
                                  unsigned int a4, unsigned char const *a5, unsigned long a6)
{
    return IOMobileFramebufferLegacy::set_block(a0, a1, a2, a3, a4, a5, a6);
}

IOReturn AdrenoDisplay::get_block(task *a0, unsigned int a1, unsigned int a2, unsigned long long const *a3,
                                  unsigned int a4, unsigned char *a5, unsigned long a6) const
{
    const IOReturn r = adrenoDigitalOutModeBlock(a1, a2, a5, a6);
    if (r != kIOReturnUnsupported)
        return r;
    return IOMobileFramebufferLegacy::get_block(a0, a1, a2, a3, a4, a5, a6);
}

IOReturn AdrenoDisplay::get_block_size(task *a0, unsigned int a1, unsigned int *a2)
{
    if (a1 == 0x41) {
        if (!a2)
            return kIOReturnBadArgument;
        *a2 = 44;
        return kIOReturnSuccess;
    }
    return IOMobileFramebufferLegacy::get_block_size(a0, a1, a2);
}
#endif
