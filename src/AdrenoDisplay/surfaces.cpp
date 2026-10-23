// SPDX-License-Identifier: MIT
// Copy sources and workspaces: maintenance of GPU-written sources, surface mappings, the cursor and present buffers,
// the damage signatures and the kernel console's release.
// Part of AdrenoDisplay.cpp's translation unit: included from it in order, never compiled on its own.

// FROM_DEVICE maintenance of source bytes before the CPU reads them.
// Adreno writes are not coherent with the CPU caches. The kext already invalidates every
// GPU-written resource (render targets included) over its whole length after completion
// (adreno_resource_list_submit); this still covers a list whose FROM_DEVICE did not run,
// writers outside the resource list, and mismatched cacheable aliases of a write-combined
// surface. DC CIVAC, not DC IVAC: a CPU-drawn surface (the guest display probe) may still
// hold its only copy in dirty lines, which IVAC would discard. At EL1 with HCR_EL2.VM set
// the architecture performs IVAC as CIVAC anyway. The exported flush_dcache64 is
// `dsb sy; ret` on this coherent-I/O kernel (KDK 0xfffffe0007530080), so the kext issues
// the instructions on its own mapping.
//
// The layer-0 present maintains every row it will read in one pass, without a barrier per
// row, then issues presentBarrier once (AdrenoDisplayPresent.h): DC CIVAC is not ordered
// against later loads of Normal Non-cacheable memory without a DMB/DSB, and QuartzCore
// allocates IOMFB framebuffers write-combined. No clock here: mach_absolute_time reads
// S3_4_C15_C10_6, which traps to the monitor (docs/CLOCK-TIMER-20260919.md).
static void maintainSourceRange(void *, const void *address, uint64_t bytes)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t line = 4ull << ((ctr >> 16) & 15);            // DminLine, bytes
    uintptr_t p = (uintptr_t)address & ~(uintptr_t)(line - 1);
    uintptr_t end = (uintptr_t)address + bytes;
    for (; p < end; p += line)
        __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
}

static void presentBarrier(void *)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

static uint64_t presentClock(void *)
{
    return mach_absolute_time();
}

// The cursor capture (at most 512 rows): each row maintained and completed before it is read.
static void maintainSourceRow(void *context, const void *address, uint64_t bytes)
{
    maintainSourceRange(context, address, bytes);
    __asm__ volatile("dsb sy" ::: "memory");
}

// The stage-1 attributes the CPU really uses for a kernel mapping: AT S1E1R, then PAR_EL1
// bits 63:56 (the MAIR attribute byte: 0x44 Normal Non-cacheable, 0xff Normal Write-Back,
// 0x00 Device-nGnRnE), or 0x100 | FST when the walk faults. Interrupts are off so that no
// handler's own AT replaces PAR_EL1 in between (as xnu's mmu_kvtop); the monitor preserves
// PAR_EL1 across its exits (probes/xnuboot.c guest_pa). The physical address is not kept.
static uint64_t mappingAttribute(const void *va)
{
    uint64_t par;
    boolean_t enabled = ml_set_interrupts_enabled(FALSE);
    __asm__ volatile("at s1e1r, %1\n\tisb\n\tmrs %0, par_el1" : "=r"(par) : "r"(va) : "memory");
    ml_set_interrupts_enabled(enabled);
    return (par & 1) ? (0x100 | ((par >> 1) & 0x3f)) : (par >> 56);
}

IOMemoryMap *AdrenoDisplay::mapSurfaceGated(IOMemoryDescriptor *md, SurfaceMap *slots, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        if (slots[i].md == md && slots[i].map) {
            slots[i].lastUse = fPresentCount;
            return slots[i].map;
        }
    }
    // An empty slot, else the least recently presented one.
    uint32_t victim = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (!slots[i].md) {
            victim = i;
            break;
        }
        if (slots[i].lastUse < slots[victim].lastUse)
            victim = i;
    }
    SurfaceMap &slot = slots[victim];
    OSSafeReleaseNULL(slot.map);
    OSSafeReleaseNULL(slot.md);
    IOMemoryMap *map = md->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapReadOnly, 0, 0);
    if (!map)
        return nullptr;
    md->retain();
    slot.md = md;
    slot.map = map;
    slot.lastUse = fPresentCount;
    return map;
}

#if ADRENO_PRESENT_CACHED_READ
// PAR_EL1.ATTR (mappingAttribute) is Normal memory, inner and outer write-back (0b01RW or
// 0b11RW with RW != 0, Arm ARM MAIR encoding); 0x44 (non-cacheable), device and faults are not.
static bool attrWriteBack(uint64_t attr)
{
    if (attr > 0xff)
        return false;
    unsigned in = attr & 15, out = (attr >> 4) & 15;
    return (in & 4) && (in & 3) && (out & 4) && (out & 3);
}

// --present-cached-read (FAILURES-AND-DESIGN.md section 10). The layer-0 map, with one
// conversion attempt per new slot:
//   1. the default-mode map, as today; already write-back (M10): done;
//   2. a kIOMapCopybackCache map of the same descriptor. 26A428's memoryReferenceMap turns the
//      mode into MAP_MEM_ONLY on the surface's own named entry, and vm_object_change_wimg_mode
//      converts the whole object and every mapping of it through SPTM (10.1);
//   3. probe 1: the copyback map walks to write-back (else M3); probe 2: the default map made in
//      step 1 now walks to write-back too, so xnu changed the object and no mismatched alias
//      exists (else M1). Only then is the slot cached. The copyback map is released either way;
//      after a failed attempt the whole range is cleaned and invalidated through the default map,
//      so no line the short-lived copyback alias filled survives.
// Reads keep pass 1's CIVAC and the barrier (AdrenoDisplayPresent.h) whatever the outcome.
IOMemoryMap *AdrenoDisplay::mapSourceGated(IOSurface *surface, IOMemoryDescriptor *md, bool *cached)
{
    *cached = false;
    for (uint32_t i = 0; i < kSurfaceMapSlots; i++) {
        if (fSurfaceMaps[i].md == md && fSurfaceMaps[i].map) {
            fSurfaceMaps[i].lastUse = fPresentCount;
            *cached = fSurfaceMaps[i].cached;
            return fSurfaceMaps[i].map;
        }
    }
    uint32_t victim = 0;
    for (uint32_t i = 0; i < kSurfaceMapSlots; i++) {
        if (!fSurfaceMaps[i].md) {
            victim = i;
            break;
        }
        if (fSurfaceMaps[i].lastUse < fSurfaceMaps[victim].lastUse)
            victim = i;
    }
    SurfaceMap &slot = fSurfaceMaps[victim];
    OSSafeReleaseNULL(slot.map);                 // M6: our view only; the object keeps its attribute
    OSSafeReleaseNULL(slot.md);
    slot.cached = false;
    IOMemoryMap *map = md->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapReadOnly, 0, 0);
    if (!map)
        return nullptr;
    const void *va = (const void *)(uintptr_t)map->getVirtualAddress();
    bool writeBack = attrWriteBack(mappingAttribute(va));
    bool reverted = false;
    for (uint32_t i = 0; i < kRevertedSlots; i++)
        reverted |= fCachedReadReverted_[i] == md;
    uint32_t mode = surface->getCacheMode();
    if (writeBack) {
        fCachedReadAlready++;                    // M10
    } else if (!reverted && mode != kIOMapDefaultCache && mode != kIOMapCopybackCache &&
               mode != kIOMapCopybackInnerCache) {
        const uint64_t start = mach_absolute_time();
        IOMemoryMap *copyback = md->createMappingInTask(kernel_task, 0,
                                                        kIOMapAnywhere | kIOMapReadOnly | kIOMapCopybackCache, 0, 0);
        fCachedReadAttr = copyback ? mappingAttribute((const void *)(uintptr_t)copyback->getVirtualAddress()) : 0x200;
        fCachedReadProbeAttr = mappingAttribute(va);
        OSSafeReleaseNULL(copyback);
        if (!attrWriteBack(fCachedReadAttr)) {
            fCachedReadFallbacks++;              // M3: refused, null or not write-back
        } else if (!attrWriteBack(fCachedReadProbeAttr)) {
            fCachedReadMismatch++;               // M1: only the copyback map was write-back
        } else {
            fCachedReadConversions++;
            writeBack = true;
        }
        if (!writeBack) {
            maintainSourceRange(nullptr, va, map->getLength());
            presentBarrier(nullptr);
        }
        const uint64_t ticks = mach_absolute_time() - start;
        if (ticks > fCachedReadConvertMaxTicks) fCachedReadConvertMaxTicks = ticks;
        if (fCachedReadConversions + fCachedReadFallbacks + fCachedReadMismatch == 1)
            fPerfConversionPending = true;   // one perf line for the first outcome, after its copy
        if (fCachedReadConversions + fCachedReadFallbacks + fCachedReadMismatch <= 4)
            IOLog("AdrenoDisplay: cached read: mode %#x copyback attr %#llx default attr %#llx: %s\n", mode,
                  fCachedReadAttr, fCachedReadProbeAttr, writeBack ? "converted" : "kept the uncached path");
    }
    md->retain();
    slot.md = md;
    slot.map = map;
    slot.lastUse = fPresentCount;
    slot.cached = writeBack;
    *cached = writeBack;
    return map;
}
#endif

void AdrenoDisplay::releaseSurfaceMaps()
{
    for (uint32_t i = 0; i < kSurfaceMapSlots; i++) {
        OSSafeReleaseNULL(fSurfaceMaps[i].map);
        OSSafeReleaseNULL(fSurfaceMaps[i].md);
        fSurfaceMaps[i].lastUse = 0;
    }
    for (uint32_t i = 0; i < kCursorMapSlots; i++) {
        OSSafeReleaseNULL(fCursorMaps[i].map);
        OSSafeReleaseNULL(fCursorMaps[i].md);
        fCursorMaps[i].lastUse = 0;
    }
}

bool AdrenoDisplay::cursorBuffersGated()
{
    if (!fCursor.image)
        fCursor.image = (uint8_t *)IOMalloc(ADRENO_DISPLAY_CURSOR_BYTES);
    if (!fCursor.save)
        fCursor.save = (uint8_t *)IOMalloc(ADRENO_DISPLAY_CURSOR_BYTES);
    if (!fPresentWorkspace.next_save)
        fPresentWorkspace.next_save = (uint8_t *)IOMalloc(ADRENO_DISPLAY_CURSOR_BYTES);
    if (!fPresentWorkspace.row && fScanoutWidth) {
        fPresentWorkspace.row_bytes = (uint64_t)fScanoutWidth * 4;
        fPresentWorkspace.row = (uint8_t *)IOMalloc(fPresentWorkspace.row_bytes);
    }
    return fCursor.image && fCursor.save && fPresentWorkspace.next_save && fPresentWorkspace.row;
}

void AdrenoDisplay::releaseCursor()
{
    if (fCursor.image)
        IOFree(fCursor.image, ADRENO_DISPLAY_CURSOR_BYTES);
    if (fCursor.save)
        IOFree(fCursor.save, ADRENO_DISPLAY_CURSOR_BYTES);
    if (fPresentWorkspace.next_save)
        IOFree(fPresentWorkspace.next_save, ADRENO_DISPLAY_CURSOR_BYTES);
    if (fPresentWorkspace.row)
        IOFree(fPresentWorkspace.row, fPresentWorkspace.row_bytes);
    // The stage and the signature table outlive a cursor reset; releasePresentBuffers frees them.
    uint8_t *stage = fPresentWorkspace.stage;
    uint64_t stageRowBytes = fPresentWorkspace.stage_row_bytes;
    bzero(&fPresentWorkspace, sizeof(fPresentWorkspace));
    fPresentWorkspace.stage = stage;
    fPresentWorkspace.stage_row_bytes = stageRowBytes;
    fPresentWorkspace.damage = fDamage.sig ? &fDamage : nullptr;
    bzero(&fCursor, sizeof(fCursor));
}

bool AdrenoDisplay::presentBuffersGated()
{
    // A layer-0 copy reads each source row once into a cacheable stage (AdrenoDisplayPresent.h), so
    // the stage is required. The signature table is not: without it every source row is written.
    if (!fPresentWorkspace.stage && fScanoutWidth) {
        uint64_t rowBytes = (uint64_t)fScanoutWidth * 4;
        uint64_t bytes = rowBytes * ADRENO_DISPLAY_STAGE_ROWS;
        fPresentWorkspace.stage = (uint8_t *)IOMalloc(bytes);
        if (fPresentWorkspace.stage) {
            fStageBytes = bytes;
            fPresentWorkspace.stage_row_bytes = rowBytes;
        }
    }
    if (!fDamage.sig && fScanoutHeight) {
        uint64_t *sig = (uint64_t *)IOMalloc((uint64_t)fScanoutHeight * 2 * sizeof(uint64_t));
        uint8_t *known = (uint8_t *)IOMalloc(fScanoutHeight);
        if (sig && known) {
            bzero(known, fScanoutHeight);
            fDamage.sig = sig;
            fDamage.known = known;
            fDamage.rows = fScanoutHeight;
            fDamage.width = 0;
            fDamage.refresh_next = 0;
            fDamage.refresh_period = ADRENO_DISPLAY_REFRESH_PERIOD;
        } else {
            if (sig) IOFree(sig, (uint64_t)fScanoutHeight * 2 * sizeof(uint64_t));
            if (known) IOFree(known, fScanoutHeight);
        }
    }
    fPresentWorkspace.damage = fDamage.sig ? &fDamage : nullptr;
    return fPresentWorkspace.stage != nullptr;
}

void AdrenoDisplay::releasePresentBuffers()
{
    if (fPresentWorkspace.stage)
        IOFree(fPresentWorkspace.stage, fStageBytes);
    fPresentWorkspace.stage = nullptr;
    fPresentWorkspace.stage_row_bytes = 0;
    fPresentWorkspace.damage = nullptr;
    fStageBytes = 0;
    if (fDamage.sig)
        IOFree(fDamage.sig, fDamage.rows * 2 * sizeof(uint64_t));
    if (fDamage.known)
        IOFree(fDamage.known, fDamage.rows);
    bzero(&fDamage, sizeof(fDamage));
}

void AdrenoDisplay::forgetDamageGated()
{
    // The scanout may no longer hold what the signatures describe (start, stop, mode set, remap):
    // the next layer-0 present writes every source row.
    if (fDamage.width)
        fCopyDamageResets++;
    adreno_display_damage_forget(&fDamage);
}

bool AdrenoDisplay::disableConsoleGated()
{
    // J700 26A428: release relinquishes text/progress drawing before we own the
    // framebuffer. Disable also switches the serial selector, whose sealed store
    // faulted in PE_initialize_console in the first physical triangle run.
    if (fConsoleDisabled)
        return true;
    IOPlatformExpert *pe = getPlatform();
    if (!pe)
        return false;
    IOReturn status = pe->setConsoleInfo(nullptr, kPEReleaseScreen);
    if (status != kIOReturnSuccess) {
        IOLog("AdrenoDisplay: kernel screen release failed (%08x)\n", status);
        return false;
    }
    fConsoleDisabled = true;
    forgetDamageGated();             // the console drew into the GOP until now
    IOLog("AdrenoDisplay: kernel console released the panel (kPEReleaseScreen)\n");
    return true;
}
