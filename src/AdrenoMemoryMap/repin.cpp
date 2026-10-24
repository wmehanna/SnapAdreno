// SPDX-License-Identifier: MIT
// The re-pin's refusal state and report (agents/resident-evict) and the head rule (agents/display-modeset,
// compat/adreno-resource-tail.h). Part of AdrenoMemoryMap.cpp's translation unit: included from it in order, never
// compiled on its own.
#if ADRENO_RESIDENT_REPIN
bool AdrenoMemoryMap::cleanlyRefused() const
{
    // The state a failed commit leaves after its own unwind succeeded: reserved and bound, no pin, no leaf.
    return fResource.flags == ADRENO_RESOURCE_RESERVED && fResource.length && !fResource.mapped_length && !fBacking;
}
void AdrenoMemoryMap::reportRefused(const void *resource)
{
    if (!fLock) return;
    IORecursiveLockLock(fLock);
    const bool first = !fRefusalReported;
    fRefusalReported = 1;
    IOGPUMemory *memory = nativeMemory();
    IOMemoryDescriptor *backing = memory ? memory->get_iomd_ptr() : nullptr;
    const uint64_t descriptor = backing ? backing->getLength() : 0;
    const uint64_t address = fResource.address, length = fResource.logical_length;
    const unsigned flags = fResource.flags;
    const int32_t refusal = fRefusal;
    const uint32_t status = fRefusalStatus;
    IORecursiveLockUnlock(fLock);
    if (!first) return;
    const uint64_t n = __atomic_add_fetch(&gRepin[kRepinReports], 1, __ATOMIC_RELAXED);
    if (n > 32 && (n & 255)) return;
    // No owning process here: IOGPU exposes no accessor for a map's IOGPUTask, and reading it from IOGPUMemoryMap's
    // private layout would call through whatever a different IOGPUFamily build keeps at that offset, on a path any
    // client's submission reaches. The IOSurface ID and the map's own fields identify the buffer.
    const void *surface = resource ? AdrenoRepinResourceIOSurface(resource) : nullptr;
    const uint32_t surfaceID = surface ? AdrenoRepinIOSurfaceID(surface) : 0;
#if ADRENO_RESIDENT_HEAD_OFFSET
    MLOG("unadmitted map refused a submission: IOSurface %u address=0x%llx length=0x%llx descriptor=0x%llx "
         "flags=0x%x refusal %d native=0x%x; maps tail %llu head %llu refused %llu retried %llu recovered %llu (%llu)",
         surfaceID, address, length, descriptor, flags, refusal, status,
         __atomic_load_n(&gRepin[kRepinTail], __ATOMIC_RELAXED), __atomic_load_n(&gRepin[kRepinHead], __ATOMIC_RELAXED),
         __atomic_load_n(&gRepin[kRepinRefused], __ATOMIC_RELAXED), __atomic_load_n(&gRepin[kRepinRetried], __ATOMIC_RELAXED),
         __atomic_load_n(&gRepin[kRepinRecovered], __ATOMIC_RELAXED), n);
#else
    MLOG("unadmitted map refused a submission: IOSurface %u address=0x%llx length=0x%llx descriptor=0x%llx "
         "flags=0x%x refusal %d native=0x%x; maps tail %llu refused %llu retried %llu recovered %llu (%llu)",
         surfaceID, address, length, descriptor, flags, refusal, status,
         __atomic_load_n(&gRepin[kRepinTail], __ATOMIC_RELAXED), __atomic_load_n(&gRepin[kRepinRefused], __ATOMIC_RELAXED),
         __atomic_load_n(&gRepin[kRepinRetried], __ATOMIC_RELAXED),
         __atomic_load_n(&gRepin[kRepinRecovered], __ATOMIC_RELAXED), n);
#endif
}
#if ADRENO_RESIDENT_HEAD_OFFSET
// ---- the head rule (compat/adreno-resource-tail.h) ----
static uint32_t gHeadWanted;                            // 0 not read yet, 1 on, 2 off (boot-arg adreno_head_offset=0)
static uint32_t gHeadAdmittedLogged, gHeadRefusedLogged;
bool AdrenoMemoryMap::headOffsetWanted()
{
    uint32_t wanted = __atomic_load_n(&gHeadWanted, __ATOMIC_RELAXED);
    if (wanted) return wanted == 1;
    uint32_t arg = 1, none = 0;
    PE_parse_boot_argn("adreno_head_offset", &arg, sizeof(arg));
    wanted = arg ? 1 : 2;
    if (__atomic_compare_exchange_n(&gHeadWanted, &none, wanted, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        MLOG("head offset: %s", wanted == 1 ? "on: page-unaligned client buffers are mapped in whole pages"
                                            : "off (boot-arg adreno_head_offset=0): page-unaligned client buffers are refused");
    return wanted == 1;
}
bool AdrenoMemoryMap::admitHead(IOMemoryDescriptor *backing, IODirection direction, uint64_t bytes, uint64_t *head)
{
    // The wire fixed the pages. The first segment's offset in its page is the client address's (a 16 KiB guest page
    // maps one whole physical page), the offset IOGPU counted into the map's length; a segment anywhere else is refused
    // by adreno_head_admit or, page by page, by adreno_head_run.
    IOByteCount span = 0;
    const uint64_t pa = backing->getPhysicalSegment(0, &span, kIOMemoryMapperNone);
    *head = pa & (ADRENO_TAIL_PAGE - 1);
    if (pa && span && adreno_head_admit(*head, bytes, fResource.logical_length)) {
        if (*head) {
            repinCount(kRepinHead);
            if (const uint32_t n = repinLogDue(&gHeadAdmittedLogged))
                MLOG("head offset: admitted address=0x%llx length=0x%llx descriptor=0x%llx head=0x%llx (%u)",
                     fResource.address, fResource.logical_length, bytes, *head, n);
        }
        return true;
    }
    if (const uint32_t n = repinLogDue(&gHeadRefusedLogged))
        MLOG("head offset: refused address=0x%llx length=0x%llx descriptor=0x%llx head=0x%llx (%u)",
             fResource.address, fResource.logical_length, bytes, *head, n);
    // Nothing was mapped: undo the wire. A failed complete leaves the pages wired (a leak, never a stale translation).
    const IOReturn done = backing->complete(direction);
    if (done != kIOReturnSuccess) MLOG("head offset: complete after a refusal failed 0x%x", done);
    backing->release();
    fLastNativeStatus = static_cast<uint32_t>(kIOReturnBadArgument);
    *head = 0;
    return false;
}
int AdrenoMemoryMap::headRange(uint64_t offset, uint64_t bytes, uint64_t *mapOffset) const
{
    if (!fBackingHead) { *mapOffset = offset; return 0; }
    return adreno_head_range(fBackingHead, fBackingBytes, offset, bytes, mapOffset) ? 0 : ADRENO_RESOURCE_E_RANGE;
}
#endif
#endif
