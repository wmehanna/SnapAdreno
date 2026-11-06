// SPDX-License-Identifier: MIT
// The AdrenoDisplayStats property (ioreg -rc AdrenoDisplay) and the perf console line.
// Part of AdrenoDisplay.cpp's translation unit: included from it in order, never compiled on its own.

void AdrenoDisplay::publishStatsGated()
{
    OSDictionary *stats = OSDictionary::withCapacity(80);
    if (!stats)
        return;
    bool ok = putNum(stats, "Presents", fPresentCount, 64) && putNum(stats, "Drops", fDropCount, 64) &&
              putNum(stats, "Blank", fBlankCount, 64) && putNum(stats, "CopyErrors", fCopyErrors, 64) &&
              putNum(stats, "LastCopyError", (uint32_t)fLastCopyError) &&
              putNum(stats, "LastFormat", fLastFormat) &&
              putNum(stats, "LastWidth", fLastSurfaceWidth, 64) &&
              putNum(stats, "LastHeight", fLastSurfaceHeight, 64) &&
              putNum(stats, "LastRowBytes", fLastSurfaceRowBytes, 64) &&
              stats->setObject("ConsoleDisabled", fConsoleDisabled ? kOSBooleanTrue : kOSBooleanFalse);
    ok = ok && putNum(stats, "ScanoutBase", (uint64_t)(uintptr_t)fScanout, 64) &&
         putNum(stats, "ScanoutPhys", fScanoutPhys, 64) && putNum(stats, "ScanoutBytes", fScanoutLength, 64) &&
         putNum(stats, "ScanoutRowBytes", fScanoutRowBytes, 64);
    // Cursor layer (docs/ADRENO-DISPLAY-CURSOR-20261018.md). Positions are int32 shown as
    // their 32-bit two's complement (0xfffffffb is -5).
    uint64_t copyNs = 0, copyMaxNs = 0, maintNs = 0;
    absolutetime_to_nanoseconds(fCopyTicks, &copyNs);
    absolutetime_to_nanoseconds(fCopyMaxTicks, &copyMaxNs);
    absolutetime_to_nanoseconds(fCopyMaintTicks, &maintNs);
    ok = ok && putNum(stats, "CopyTimed", fCopyTimed, 64) && putNum(stats, "CopyMicros", copyNs / 1000, 64) &&
         putNum(stats, "CopyMaxMicros", copyMaxNs / 1000, 64) && putNum(stats, "CopyMaintMicros", maintNs / 1000, 64);
    // The rest of the split (AdrenoDisplayPresent.h): CopyMicros ~ Maint + Barrier + Read + Hash + Write.
    uint64_t barrierNs = 0, readNs = 0, hashNs = 0, writeNs = 0;
    absolutetime_to_nanoseconds(fCopyBarrierTicks, &barrierNs);
    absolutetime_to_nanoseconds(fCopyReadTicks, &readNs);
    absolutetime_to_nanoseconds(fCopyHashTicks, &hashNs);
    absolutetime_to_nanoseconds(fCopyWriteTicks, &writeNs);
    ok = ok && putNum(stats, "CopyBarrierMicros", barrierNs / 1000, 64) &&
         putNum(stats, "CopyReadMicros", readNs / 1000, 64) && putNum(stats, "CopyHashMicros", hashNs / 1000, 64) &&
         putNum(stats, "CopyWriteMicros", writeNs / 1000, 64) &&
         putNum(stats, "CopyRowsCompared", fCopyRowsCompared, 64) &&
         putNum(stats, "CopyRowsWritten", fCopyRowsWritten, 64) &&
         putNum(stats, "CopyRowsSkipped", fCopyRowsSkipped, 64) &&
         putNum(stats, "CopyRowsRefreshed", fCopyRowsRefreshed, 64) &&
         putNum(stats, "CopyDamageResets", fCopyDamageResets, 64) &&
         putNum(stats, "CopyFullFrames", fCopyFullFrames, 64) &&
         putNum(stats, "CopyLastRowsWritten", fCopyLastRowsWritten, 64) &&
         putNum(stats, "CopyTracking", fPresentWorkspace.damage ? 1 : 0) &&
         putNum(stats, "LastCacheMode", fLastCacheMode) && putNum(stats, "SourceAttr", fSourceAttr, 64) &&
         putNum(stats, "ScanoutAttr", fScanoutAttr, 64) &&
         putNum(stats, "LastSrcX", (uint32_t)fLastSrc.x) && putNum(stats, "LastSrcY", (uint32_t)fLastSrc.y) &&
         putNum(stats, "LastSrcWidth", (uint32_t)fLastSrc.w) && putNum(stats, "LastSrcHeight", (uint32_t)fLastSrc.h) &&
         putNum(stats, "LastDstX", (uint32_t)fLastDst.x) && putNum(stats, "LastDstY", (uint32_t)fLastDst.y) &&
         putNum(stats, "LastDstWidth", (uint32_t)fLastDst.w) && putNum(stats, "LastDstHeight", (uint32_t)fLastDst.h) &&
         putNum(stats, "SwapDirtyFlagged", fSwapDirtyFlagged, 64) &&
         putNum(stats, "SwapDirtyX", (uint32_t)fSwapDirty.x) && putNum(stats, "SwapDirtyY", (uint32_t)fSwapDirty.y) &&
         putNum(stats, "SwapDirtyWidth", (uint32_t)fSwapDirty.w) &&
         putNum(stats, "SwapDirtyHeight", (uint32_t)fSwapDirty.h);
#if ADRENO_PRESENT_CACHED_READ
    uint64_t convertNs = 0;
    absolutetime_to_nanoseconds(fCachedReadConvertMaxTicks, &convertNs);
    ok = ok && putNum(stats, "CachedRead", 1) && putNum(stats, "CachedReadConversions", fCachedReadConversions, 64) &&
         putNum(stats, "CachedReadFallbacks", fCachedReadFallbacks, 64) &&
         putNum(stats, "CachedReadMismatch", fCachedReadMismatch, 64) &&
         putNum(stats, "CachedReadReverted", fCachedReadReverted, 64) &&
         putNum(stats, "CachedReadAlready", fCachedReadAlready, 64) &&
         putNum(stats, "CachedReadAttr", fCachedReadAttr, 64) &&
         putNum(stats, "CachedReadProbeAttr", fCachedReadProbeAttr, 64) &&
         putNum(stats, "CachedReadConvertMaxMicros", convertNs / 1000, 64) &&
         putNum(stats, "SourceCached", fSourceCached ? 1 : 0);
#endif
    ok = ok && putNum(stats, "PublishedBytes", fPublishedBytes, 64) &&
         putNum(stats, "PublishedRegions", fPublishedRegions, 64) && putNum(stats, "SourceBytes", fSourceBytes, 64);
    ok = ok && putNum(stats, "LayerMask", fLastLayerMask) && putNum(stats, "LayerMaskSeen", fLayerMaskSeen) &&
         putNum(stats, "CursorSwaps", fCursorSwaps, 64) && putNum(stats, "CursorOnlySwaps", fCursorOnlySwaps, 64) &&
         putNum(stats, "CursorImmediate", fCursorImmediate, 64) &&
         putNum(stats, "CursorUpdates", fCursorUpdates, 64) && putNum(stats, "CursorDraws", fCursorDraws, 64) &&
         putNum(stats, "CursorHides", fCursorHides, 64) &&
         putNum(stats, "CursorFormatSkips", fCursorFormatSkips, 64) &&
         putNum(stats, "CursorErrors", fCursorErrors, 64) && putNum(stats, "CursorScaled", fCursorScaled, 64) &&
         putNum(stats, "LastCursorError", (uint32_t)fLastCursorError) &&
         putNum(stats, "CursorLayer", fCursorLayer) && putNum(stats, "CursorFormat", fCursorFormat) &&
         putNum(stats, "CursorSurfaceWidth", fCursorSurfaceWidth, 64) &&
         putNum(stats, "CursorSurfaceHeight", fCursorSurfaceHeight, 64) &&
         putNum(stats, "CursorSurfaceRowBytes", fCursorSurfaceRowBytes, 64) &&
         putNum(stats, "CursorX", (uint32_t)fCursorDst.x) && putNum(stats, "CursorY", (uint32_t)fCursorDst.y) &&
         putNum(stats, "CursorDstWidth", (uint32_t)fCursorDst.w) &&
         putNum(stats, "CursorDstHeight", (uint32_t)fCursorDst.h) &&
         putNum(stats, "CursorSrcX", (uint32_t)fCursorSrc.x) && putNum(stats, "CursorSrcY", (uint32_t)fCursorSrc.y) &&
         putNum(stats, "CursorSrcWidth", (uint32_t)fCursorSrc.w) &&
         putNum(stats, "CursorSrcHeight", (uint32_t)fCursorSrc.h) &&
         putNum(stats, "CursorImageWidth", fCursor.width) && putNum(stats, "CursorImageHeight", fCursor.height) &&
         stats->setObject("CursorVisible", fCursor.visible ? kOSBooleanTrue : kOSBooleanFalse) &&
         stats->setObject("CursorDrawn", fCursor.drawn ? kOSBooleanTrue : kOSBooleanFalse);
    if (ok)
        setProperty("AdrenoDisplayStats", stats);
    stats->release();
}

static uint64_t perfMicros(uint64_t ticks)
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(ticks, &ns);
    return ns / 1000;
}

// One bounded console line with the numbers this experiment is decided by (the run keeps the
// whole kernel console; the stats dictionary is not captured). Called every kPerfEvery layer-0
// copies and once for the first cached-read conversion outcome, never from the row loops.
//   AdrenoDisplay: perf why=<periodic|convert|reject> presents=P copies=C win=N
//     mean_us=M max_us=X all_mean_us=AM all_max_us=AX
//     maint_us=a barrier_us=b read_us=r hash_us=h write_us=w            (per copy, this window)
//     rows=W/K skipped=S refreshed=F                                   (this window)
//     mode=0x<getCacheMode> attr=0x<SourceAttr> cached=<build>/<this source>
//     conv=<conversions>/<fallbacks>/<mismatch>/<reverted>/<already> errors=E last_error=L
void AdrenoDisplay::perfLineGated(const char *why)
{
    PerfMark now = {fCopyTimed, fCopyTicks, fCopyMaintTicks, fCopyBarrierTicks, fCopyReadTicks, fCopyHashTicks,
                    fCopyWriteTicks, fCopyRowsCompared, fCopyRowsWritten, fCopyRowsSkipped, fCopyRowsRefreshed};
    uint64_t n = now.copies - fPerfMark.copies, d = n ? n : 1, all = now.copies ? now.copies : 1;
#if ADRENO_PRESENT_CACHED_READ
    const unsigned build = 1, sourceCached = fSourceCached ? 1 : 0;
    const uint64_t conv[5] = {fCachedReadConversions, fCachedReadFallbacks, fCachedReadMismatch, fCachedReadReverted,
                              fCachedReadAlready};
#else
    const unsigned build = 0, sourceCached = 0;
    const uint64_t conv[5] = {0, 0, 0, 0, 0};
#endif
#if ADRENO_PERF_FRAMES
    // F2 F3 F11: the same numbers as the single line below, split so none passes the console's 255 bytes, then
    // the window's pacing and GPU-map counters; one s=<seq> joins them.
    char line[ADRENO_PERF_LINE_MAX + 16];
    const uint64_t seq = ++fPerfSeq;
    const uint64_t head[ADRENO_PERF_HEAD_FIELDS] = {fPresentCount, now.copies, n, perfMicros(now.ticks - fPerfMark.ticks) / d,
                                                    perfMicros(fPerfMaxTicks), perfMicros(now.ticks) / all,
                                                    perfMicros(fCopyMaxTicks)};
    adreno_perf_line_head(line, sizeof line, why, seq, head);
    IOLog("%s\n", line);
    const uint64_t split[ADRENO_PERF_SPLIT_FIELDS] = {perfMicros(now.maint - fPerfMark.maint) / d,
        perfMicros(now.barrier - fPerfMark.barrier) / d, perfMicros(now.read - fPerfMark.read) / d,
        perfMicros(now.hash - fPerfMark.hash) / d, perfMicros(now.write - fPerfMark.write) / d,
        now.written - fPerfMark.written, now.compared - fPerfMark.compared, now.skipped - fPerfMark.skipped,
        now.refreshed - fPerfMark.refreshed};
    adreno_perf_line_split(line, sizeof line, seq, split);
    IOLog("%s\n", line);
    adreno_perf_line_src(line, sizeof line, seq, fLastCacheMode, fSourceAttr, build, sourceCached, conv, fCopyErrors,
                         fLastCopyError);
    IOLog("%s\n", line);
    struct adreno_pacing_summary pace;
    adreno_pacing_summarize(&fPacing, &pace);
    adreno_pacing_next_window(&fPacing);
    adreno_perf_line_pace(line, sizeof line, seq, &pace);
    IOLog("%s\n", line);
#if ADRENO_VSYNC60
    struct adreno_vsync_summary vsync;
    adreno_vsync_summarize(&fVsync, perfNs, &vsync);
    adreno_vsync_next_window(&fVsync);
    adreno_perf_line_vsync(line, sizeof line, seq, &vsync);
    IOLog("%s\n", line);
#endif
#if ADRENO_LATCH
    {   // H9: the latch and the hold this window (before the beam line resets the slack's window values)
        struct adreno_latch_summary ls;
        memset(&ls, 0, sizeof ls);
        ls.slack_us = perfNs(fBeam.slack) / 1000u;
        ls.slack_peak_us = perfNs(fBeam.slack_peak) / 1000u;
        ls.off = fBeam.latch_off;
        ls.hold_us = fHold.hold_n ? perfNs(fHold.hold_sum / fHold.hold_n) / 1000u : 0;
        ls.rb_us = perfNs(fHold.rb_last) / 1000u;
        ls.r_n = fHold.r_n;
        ls.r_us = fHold.r_n ? perfNs(fHold.r_sum / fHold.r_n) / 1000u : 0;
        ls.r_max_us = perfNs(fHold.r_max) / 1000u;
        ls.precopies = fHold.precopies;
        ls.rel_successor = fHold.rel_successor; ls.rel_lone = fHold.rel_lone;
        ls.rel_deferred = fHold.rel_deferred; ls.rel_overdue = fHold.rel_overdue;
        ls.misses = fHold.misses; ls.ib_after = fHold.ib_after; ls.fallbacks = fHold.fallbacks; ls.gated = fHold.gated;
        ls.raise_ovt = fBeam.slack_ovt; ls.raise_late = fBeam.slack_late;
        adreno_hold_next_window(&fHold);
        adreno_perf_line_latch(line, sizeof line, seq, &ls);
        IOLog("%s\n", line);
    }
#endif
#if ADRENO_PRESENT_SYNC
    {   // K9: the gate's decisions this window and the kext's marks (window deltas)
        uint64_t total[3];
        AdrenoPresentSyncCounters(&total[0], &total[1], &total[2]);
        const struct adreno_sync_summary ss = {fSync.defer, fSync.swaps, fSync.forced, perfNs(fSync.max_waited) / 1000u,
                                               total[0] - fSyncMark[0], fSync.busy, total[1] - fSyncMark[1],
                                               fSync.stale_seen, total[2] - fSyncMark[2]};
        memcpy(fSyncMark, total, sizeof fSyncMark);
        adreno_sync_next_window(&fSync);
        adreno_perf_line_sync(line, sizeof line, seq, &ss);
        IOLog("%s\n", line);
    }
#endif
#if ADRENO_PIPELINE
    {   // agents/gpu-pipeline O4: the IB pipeline this window (window deltas of AdrenoX2's totals)
        uint64_t total[ADRENO_PIPE_COUNTERS];
        const uint32_t state = AdrenoPipelineCounters(total);
        struct adreno_pipe_summary ps;
        adreno_pipe_window(&ps, state, total, fPipeMark);
        memcpy(fPipeMark, total, sizeof fPipeMark);
        adreno_perf_line_pipe(line, sizeof line, seq, &ps);
        IOLog("%s\n", line);
    }
#endif
#if ADRENO_PRESENT_RELATCH
    {   // OP1: the relatch this window, and AdrenoX2's early-release totals (window deltas)
        uint64_t total[ADRENO_RELATCH_EARLY_FIELDS], early[ADRENO_RELATCH_EARLY_FIELDS];
        const uint32_t state = AdrenoPipelineEarlyCounters(total);   // bit 0 early release, bit 1 urgent polling
        for (unsigned i = 0; i < ADRENO_RELATCH_EARLY_FIELDS; i++) early[i] = total[i] - fRelatchEarlyMark[i];
        memcpy(fRelatchEarlyMark, total, sizeof fRelatchEarlyMark);
        struct adreno_relatch_summary rs;
        adreno_relatch_summarize(&fRelatch, perfNs, (fRelatch.enabled && fRelatchTimer ? 1u : 0u) | (uint64_t)(state & 3u) << 1,
                                 early, &rs);
        adreno_relatch_next_window(&fRelatch);
        adreno_perf_line_relatch(line, sizeof line, seq, &rs);
        IOLog("%s\n", line);
    }
#endif
#if ADRENO_PRESENT_EARLY_DONE
    {   // AdrenoDisplayEarlyDone.h: completions at the copy, late frames and WindowServer's arrivals against the ticks this window
        const int on = adreno_early_active(&fEarly, fEarly.mode == ADRENO_EARLY_ADAPTIVE ? mach_absolute_time() : 0);
        adreno_perf_line_done(line, sizeof line, seq, &fEarly, on);
        adreno_early_next_window(&fEarly);
        IOLog("%s\n", line);
    }
#endif
#if ADRENO_BEAM
    {   // E4: the beam's state, timing and this window's decisions
        struct adreno_beam_summary beam;
        adreno_beam_summarize(&fBeam, perfNs, &beam);
        adreno_beam_next_window(&fBeam);
        adreno_perf_line_beam(line, sizeof line, seq, &beam);
        IOLog("%s\n", line);
    }
#endif
#if ADRENO_RESOURCE_POOL
    uint64_t map[ADRENO_PERF_MAP_FIELDS], win[ADRENO_PERF_MAP_FIELDS];
    AdrenoGPUMapCounters(map);
    map[ADRENO_MAP_IBS] = __atomic_load_n(&gAdrenoIBCompleted, __ATOMIC_ACQUIRE);
    for (unsigned i = 0; i < ADRENO_PERF_MAP_FIELDS; i++)
        win[i] = i == ADRENO_MAP_POOL_LIVE ? map[i] : map[i] - fMapMark[i];
    memcpy(fMapMark, map, sizeof fMapMark);
    adreno_perf_line_map(line, sizeof line, seq, win);
    IOLog("%s\n", line);
#if ADRENO_VA_RECLAIM
    {   // G16: levels as they are (bytes to MiB), counters as window deltas
        uint64_t total[ADRENO_PERF_VA_FIELDS], va[ADRENO_PERF_VA_FIELDS];
        AdrenoGPUVACounters(total);
        for (unsigned i = 0; i < ADRENO_PERF_VA_FIELDS; i++)
            va[i] = adreno_perf_va_level(i) ? total[i] : total[i] - fVaMark[i];
        memcpy(fVaMark, total, sizeof fVaMark);
        va[ADRENO_VA_HELD] >>= 20; va[ADRENO_VA_PEAK] >>= 20; va[ADRENO_VA_POOLED] >>= 20;
        va[ADRENO_VA_FROZEN] >>= 20; va[ADRENO_VA_DRAINED_BYTES] >>= 20;
        adreno_perf_line_va(line, sizeof line, seq, va);
        IOLog("%s\n", line);
    }
#endif
#endif
#if ADRENO_PERF_GPU
    {   // P3: totals read without a lock (window deltas); the IB maximum since the last line is taken by exchange.
        uint64_t total[ADRENO_PERF_GPU_FIELDS], gpu[ADRENO_PERF_GPU_FIELDS];
        AdrenoGPUWorkCounters(total);
        total[ADRENO_GPU_IB_TICKS] = __atomic_load_n(&gAdrenoIBGpuTicks, __ATOMIC_RELAXED);
        total[ADRENO_GPU_IB_TICKS_MAX] = __atomic_exchange_n(&gAdrenoIBGpuTicksMax, 0, __ATOMIC_RELAXED);
        total[ADRENO_GPU_SUBMIT_TICKS] = __atomic_load_n(&gAdrenoIBSubmitTicks, __ATOMIC_RELAXED);
        total[ADRENO_GPU_LOCK_TICKS] = __atomic_load_n(&gAdrenoIBLockTicks, __ATOMIC_RELAXED);
        for (unsigned i = 0; i < ADRENO_PERF_GPU_FIELDS; i++)
            gpu[i] = i == ADRENO_GPU_IB_TICKS_MAX || i == ADRENO_GPU_RES_BYTES || i == ADRENO_GPU_RES_BUDGET ?
                total[i] : total[i] - fGpuMark[i];
        memcpy(fGpuMark, total, sizeof fGpuMark);
        // P5: mach ticks to us; bytes to MiB. The IB ticks stay in the monitor's counter units.
        static const unsigned us[] = {ADRENO_GPU_PREPARE_TICKS, ADRENO_GPU_UNWIRE_TICKS, ADRENO_GPU_CACHE_TICKS,
                                      ADRENO_GPU_SUBMIT_TICKS, ADRENO_GPU_LOCK_TICKS};
        for (unsigned i = 0; i < sizeof us / sizeof us[0]; i++) gpu[us[i]] = perfMicros(gpu[us[i]]);
        gpu[ADRENO_GPU_CACHE_BYTES] >>= 20;
        gpu[ADRENO_GPU_RES_BYTES] >>= 20;
        gpu[ADRENO_GPU_RES_BUDGET] >>= 20;
        adreno_perf_line_gpu(line, sizeof line, seq, gpu);
        IOLog("%s\n", line);
    }
#endif
#else
    IOLog("AdrenoDisplay: perf why=%s presents=%llu copies=%llu win=%llu mean_us=%llu max_us=%llu all_mean_us=%llu "
          "all_max_us=%llu maint_us=%llu barrier_us=%llu read_us=%llu hash_us=%llu write_us=%llu rows=%llu/%llu "
          "skipped=%llu refreshed=%llu mode=0x%x attr=0x%llx cached=%u/%u conv=%llu/%llu/%llu/%llu/%llu errors=%llu "
          "last_error=%d\n",
          why, fPresentCount, now.copies, n, perfMicros(now.ticks - fPerfMark.ticks) / d, perfMicros(fPerfMaxTicks),
          perfMicros(now.ticks) / all, perfMicros(fCopyMaxTicks), perfMicros(now.maint - fPerfMark.maint) / d,
          perfMicros(now.barrier - fPerfMark.barrier) / d, perfMicros(now.read - fPerfMark.read) / d,
          perfMicros(now.hash - fPerfMark.hash) / d, perfMicros(now.write - fPerfMark.write) / d,
          now.written - fPerfMark.written, now.compared - fPerfMark.compared, now.skipped - fPerfMark.skipped,
          now.refreshed - fPerfMark.refreshed, fLastCacheMode, fSourceAttr, build, sourceCached, conv[0], conv[1],
          conv[2], conv[3], conv[4], fCopyErrors, fLastCopyError);
#endif
#if ADRENO_DISPLAY_TRACE_MODES
    traceWindowGated();
#endif
    fPerfMark = now;
    fPerfMaxTicks = 0;
}
