// SPDX-License-Identifier: MIT
// The re-pin's refusal state and report (agents/resident-evict). Part of AdrenoMemoryMap.cpp's translation unit:
// included from it in order, never compiled on its own.
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
    MLOG("unadmitted map refused a submission: IOSurface %u address=0x%llx length=0x%llx descriptor=0x%llx "
         "flags=0x%x refusal %d native=0x%x; maps tail %llu refused %llu retried %llu recovered %llu (%llu)",
         surfaceID, address, length, descriptor, flags, refusal, status,
         __atomic_load_n(&gRepin[kRepinTail], __ATOMIC_RELAXED), __atomic_load_n(&gRepin[kRepinRefused], __ATOMIC_RELAXED),
         __atomic_load_n(&gRepin[kRepinRetried], __ATOMIC_RELAXED),
         __atomic_load_n(&gRepin[kRepinRecovered], __ATOMIC_RELAXED), n);
}
#endif
