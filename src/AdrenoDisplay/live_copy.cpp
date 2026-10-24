// SPDX-License-Identifier: MIT
// presentGated: a swap's layer 0 and the cursor copied into the live scanout.
// Part of AdrenoDisplay.cpp's translation unit: included from it in order, never compiled on its own.

void AdrenoDisplay::presentGated(IOMFBSwapIORequest *swap)
{
#if ADRENO_LATCH
    // H4: a precopy puts a pending swap's pixels on the scanout; it becomes current (presented) only at its release.
    if (!fPrecopying) {
        fSwapPresented = true;
        fPresentCount++;
    }
#else
    fSwapPresented = true;
    fPresentCount++;
#endif
    // PV program_hardware_gated 0x118d8: layer-enable mask at swap+0x3d8, bit0 = layer 0
    // (surface at swap+0xb80), bit1 = cursor (swap+0xb88, destRect[1] origin at
    // swap+0x61/+0x65). Layer 0 is still 1:1 from the top-left (its rectangles and scaling
    // are not applied); layers 1..3 are the cursor (updateCursorGated).
    uint8_t layers = IOMFBSwapFields::layerMask(swap);
    fLastLayerMask = IOMFBSwapFields::layerMaskWord(swap);
    fLayerMaskSeen |= fLastLayerMask;
    bool report = fPresentCount <= 4 || !(fPresentCount & 63);
    struct adreno_display_scanout d = {
        (uint8_t *)(uintptr_t)fScanout, fScanoutLength, fScanoutRowBytes, fScanoutWidth, fScanoutHeight};
    IOSurface *surface = (layers & 1) ? IOMFBSwapFields::layerSurface(swap, 0) : nullptr;
    if (layers & 1) {
        // Recorded, not applied: layer 0 is copied 1:1 from the top-left, and the IOMFB dirty
        // region has no 26A428 producer (IOMobileGraphicsFamily26A428.h, kDirtyValid).
        fLastSrc = IOMFBSwapFields::srcRect(swap, 0);
        fLastDst = IOMFBSwapFields::dstRect(swap, 0);
#if ADRENO_DISPLAY_TRACE_MODES
        traceSwapGeometryGated(surface);
#endif
        if (IOMFBSwapFields::dirtyValid(swap)) {
            fSwapDirtyFlagged++;
            fSwapDirty = IOMFBSwapFields::dirtyRect(swap);
            if (fSwapDirtyFlagged == 1) {
                IOLog("AdrenoDisplay: swap carries an IOMFB dirty region (%d,%d %dx%d); not used\n", fSwapDirty.x,
                      fSwapDirty.y, fSwapDirty.w, fSwapDirty.h);
                report = true;
            }
        }
    }
    struct adreno_display_surface source = {};
    const struct adreno_display_surface *copy = nullptr;
    int error = 0;
    if (!surface || !fScanout) {
        fBlankCount++;
    } else {
        fLastFormat = surface->getPixelFormat();
        fLastSurfaceWidth = surface->getWidth();
        fLastSurfaceHeight = surface->getHeight();
        fLastSurfaceRowBytes = surface->getBytesPerRow();
        fLastCacheMode = surface->getCacheMode();
        error = ADRENO_DISPLAY_E_FORMAT;
        if (adreno_display_bytes_per_pixel(fLastFormat)) {
            IOMemoryDescriptor *md = surface->getMemoryDescriptor();
#if ADRENO_PRESENT_CACHED_READ
            bool cached = false;
            IOMemoryMap *map = md ? mapSourceGated(surface, md, &cached) : nullptr;
#else
            IOMemoryMap *map = md ? mapSurfaceGated(md, fSurfaceMaps, kSurfaceMapSlots) : nullptr;
#endif
            error = ADRENO_DISPLAY_E_ARGS;
            if (map) {
                source = {(const uint8_t *)map->getVirtualAddress(), map->getLength(), surface->getBaseOffset(),
                    fLastSurfaceRowBytes, fLastSurfaceWidth, fLastSurfaceHeight, fLastFormat};
                struct adreno_display_plan plan;
                error = adreno_display_plan(&source, &d, &plan);
                if (!error) copy = &source;
                if (!error) fSourceAttr = mappingAttribute(source.base + source.offset);
#if ADRENO_PRESENT_CACHED_READ
                // M4: a cached slot whose object went back to an uncached attribute. Reads stay correct
                // (CIVAC, barrier, read); the descriptor is never converted again.
                fSourceCached = !error && cached && attrWriteBack(fSourceAttr);
                if (!error && cached && !fSourceCached) {
                    fCachedReadReverted++;
                    fCachedReadReverted_[fCachedReadRevertedNext++ % kRevertedSlots] = md;
                    for (uint32_t i = 0; i < kSurfaceMapSlots; i++)
                        if (fSurfaceMaps[i].md == md) fSurfaceMaps[i].cached = false;
                }
#endif
            }
        }
    }
    // Refused layer-0 input must not erase the currently visible cursor. Prepare
    // all scratch storage before accepting a new cursor or publishing any pixels.
    if (!error && fScanout && ((layers & kOverlayLayers) || fCursor.drawn || fCursor.visible) &&
        !cursorBuffersGated()) error = (int)kIOReturnNoMemory;
    if (!error && copy && !presentBuffersGated()) error = (int)kIOReturnNoMemory;
    if (!error && copy && !disableConsoleGated()) error = (int)kIOReturnNotReady;
    if (!error && fScanout && fConsoleDisabled) {
        if (layers & kOverlayLayers) report |= updateCursorGated(swap, layers);
        struct adreno_display_present_result result;
#if ADRENO_BEAM
        // C3 C7: before each chunk's first scanout write the beam is waited for and checked (adreno_beam_gate). A
        // layer-0 present takes the frame its tick's GO chose; a cursor-only one chooses for the rows it may touch.
        const bool beam = fBeamRegs && fBeam.state == ADRENO_BEAM_LOCKED;
        if (beam) {
            uint64_t lo = 0, hi = fScanoutHeight;
            if (!copy) {
                lo = fScanoutHeight; hi = 0;
                if (fCursor.drawn) { lo = fCursor.sy; hi = fCursor.sy + fCursor.sh; }
                if (fCursor.visible && fCursor.height) {
                    int64_t top = fCursor.y, bottom = (int64_t)fCursor.y + fCursor.height;
                    if (top < 0) top = 0;
                    if (bottom > (int64_t)fScanoutHeight) bottom = fScanoutHeight;
                    if (bottom > top) {
                        if ((uint64_t)top < lo) lo = (uint64_t)top;
                        if ((uint64_t)bottom > hi) hi = (uint64_t)bottom;
                    }
                }
                if (lo >= hi) { lo = 0; hi = fScanoutHeight; }
            }
            adreno_beam_begin(&fBeam, !copy, lo, hi);
        }
#endif
#if ADRENO_BEAM || ADRENO_PRESENT_SKIP_MAINT
#if ADRENO_PRESENT_SKIP_MAINT && ADRENO_BEAM
        const bool maint = fPresentMaint || !beam;   // skipped only while the beam is locked; otherwise as before
#elif ADRENO_PRESENT_SKIP_MAINT
        const bool maint = fPresentMaint;    // no CIVAC pass and no barrier: the resource list invalidated the source
#else
        const bool maint = true;
#endif
        const struct adreno_display_present_ops ops = {maint ? maintainSourceRange : nullptr,
                                                       maint ? presentBarrier : nullptr, presentClock, nullptr, nullptr,
                                                       nullptr
#if ADRENO_BEAM
                                                       , beam ? beamGate : nullptr, this
#endif
                                                      };
#else
        static const struct adreno_display_present_ops ops = {
            maintainSourceRange, presentBarrier, presentClock, nullptr, nullptr, nullptr};
#endif
        const uint64_t start = mach_absolute_time();
        error = adreno_display_present(&fCursor, &d, copy, &fPresentWorkspace, &ops, &result);
#if ADRENO_BEAM
        if (beam) {
            // C5: the fire leads the vblank by the observed lateness plus this estimate of the maintenance pass
            if (!error && copy) {
                fBeam.maint = fBeam.maint - fBeam.maint / 8 + (result.maint_ticks + result.barrier_ticks) / 8;
                adreno_beam_copied(&fBeam, mach_absolute_time() - start);
            }
            adreno_beam_end(&fBeam);
        }
#endif
        if (!error && copy) {
            const uint64_t ticks = mach_absolute_time() - start;
            fCopyTimed++;
            fCopyTicks += ticks;
            if (ticks > fCopyMaxTicks) fCopyMaxTicks = ticks;
            fCopyMaintTicks += result.maint_ticks;
            fCopyBarrierTicks += result.barrier_ticks;
            fCopyReadTicks += result.read_ticks;
            fCopyHashTicks += result.hash_ticks;
            fCopyWriteTicks += result.write_ticks;
            fCopyRowsCompared += result.rows_compared;
            fCopyRowsWritten += result.rows_written;
            fCopyRowsSkipped += result.rows_skipped;
            fCopyRowsRefreshed += result.rows_refreshed;
            fCopyLastRowsWritten = result.rows_written;
            if (result.forgot) fCopyDamageResets++;
            if (!result.rows_skipped) fCopyFullFrames++;
            if (ticks > fPerfMaxTicks) fPerfMaxTicks = ticks;
#if ADRENO_PERF_FRAMES
            {   // F4 F7 F8: the copy start is the clock read the copy already made.
                const uint64_t ibs = __atomic_load_n(&gAdrenoIBCompleted, __ATOMIC_ACQUIRE);
                const uint64_t ibAt = __atomic_load_n(&gAdrenoIBCompletedAt, __ATOMIC_RELAXED);
                adreno_pacing_copy(&fPacing, perfNs(start), fInTick, fPresentQueuedAt ? perfNs(fPresentQueuedAt) : 0, ibs,
                                   ibAt ? perfNs(ibAt) : 0);
            }
#endif
            // At most one line per kPerfEvery copies; the first conversion outcome takes its place.
            if (fPerfConversionPending) {
                fPerfConversionPending = false;
#if ADRENO_PRESENT_CACHED_READ
                perfLineGated(fCachedReadConversions ? "convert" : "reject");
#endif
            } else if (!(fCopyTimed % kPerfEvery)) {
                perfLineGated("periodic");
            }
        }
        if (!error) {
            fPublishedBytes += result.bytes_written;
            fPublishedRegions += result.regions;
            fSourceBytes += result.bytes_read;
            if (result.cursor_pixels) fCursorDraws++;
            if (copy) AdrenoEarlyStopPresented(fPresentCount);
        }
    }
    if (error) {
        fCopyErrors++;
        if (error != fLastCopyError || fCopyErrors <= 4) {
            IOLog("AdrenoDisplay: composed present %08x %llux%llu row %llu not shown (%d)\n", fLastFormat,
                  fLastSurfaceWidth, fLastSurfaceHeight, fLastSurfaceRowBytes, error);
            report = true;
        }
        fLastCopyError = error;
    }
    if (report)
        publishStatsGated();         // readable with ioreg -rc AdrenoDisplay
}
