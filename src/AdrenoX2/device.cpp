// SPDX-License-Identifier: MIT
// Device configuration (provider ABIs, GPU VA, stamps, mapper) and the optional draw-submission resources: the IB
// work, the IB arena and the monitor's IB ABI.
// Part of AdrenoX2.cpp's translation unit: included from it in order, never compiled on its own.

// ---------------------------------------------------------------------------------------------------
bool AdrenoAcceleratorX2::configureDevice(IOService *provider)
{
    // Called by IOGPU::start before the config and the event machine exist.
    if (fGPUVA || fStamps || fMapper || fSubmitLock) return false;
    if (!provider || !provider->metaCast("AppleARMIODevice")) {
        ALOG("Adreno device provider missing");
        return false;
    }
    OSData *chip = OSDynamicCast(OSData, provider->getProperty("chip-id"));
    OSData *parent = OSDynamicCast(OSData, provider->getProperty("iommu-parent"));
    OSData *fence = OSDynamicCast(OSData, provider->getProperty(ADRENO_GUEST_PROPERTY));
    OSData *unmap = OSDynamicCast(OSData, provider->getProperty(ADRENO_GUEST_UNMAP_PROPERTY));
    OSData *cache = OSDynamicCast(OSData, provider->getProperty(ADRENO_GUEST_CACHE_PROPERTY));
    OSData *fill = OSDynamicCast(OSData, provider->getProperty(ADRENO_GUEST_FILL32_PROPERTY));
    uint32_t chipID = 0, mapperID = 0, fenceABI = 0, unmapABI = 0, cacheABI = 0;
    uint32_t fillABI = 0;
    if (chip && chip->getLength() == sizeof(chipID))
        bcopy(chip->getBytesNoCopy(), &chipID, sizeof(chipID));
    if (parent && parent->getLength() == sizeof(mapperID))
        bcopy(parent->getBytesNoCopy(), &mapperID, sizeof(mapperID));
    if (fence && fence->getLength() == sizeof(fenceABI))
        bcopy(fence->getBytesNoCopy(), &fenceABI, sizeof(fenceABI));
    if (unmap && unmap->getLength() == sizeof(unmapABI))
        bcopy(unmap->getBytesNoCopy(), &unmapABI, sizeof(unmapABI));
    if (cache && cache->getLength() == sizeof(cacheABI))
        bcopy(cache->getBytesNoCopy(), &cacheABI, sizeof(cacheABI));
    if (fill && fill->getLength() == sizeof(fillABI))
        bcopy(fill->getBytesNoCopy(), &fillABI, sizeof(fillABI));
    if (chipID != 0x44070041 || !mapperID || fenceABI != ADRENO_GUEST_ABI ||
        unmapABI != ADRENO_GUEST_ABI || cacheABI != ADRENO_GUEST_ABI || fillABI != ADRENO_GUEST_ABI) {
        ALOG("unqualified Adreno chip, mapper identity or guest fence/clear/cache/fill ABI");
        return false;
    }
#if ADRENO_VA_WIDE
    // agents/gpu-va-wide V12: the 16 GiB window needs the wide monitor and tree; only that tree names the width.
    OSData *bits = OSDynamicCast(OSData, provider->getProperty(ADRENO_GPU_VA_BITS_PROPERTY));
    uint32_t vaBits = 0;
    if (bits && bits->getLength() == sizeof(vaBits))
        bcopy(bits->getBytesNoCopy(), &vaBits, sizeof(vaBits));
    if (vaBits != 64 - ADRENO_CB0_T0SZ) {
        ALOG("GPU VA width %u in the tree, %u in this kext: a wide kext needs the wide monitor", vaBits,
             64 - ADRENO_CB0_T0SZ);
        return false;
    }
#endif
    AdrenoEarlyStopConfigure(provider);   // opt-in per launch; otherwise inert
    // Resolve only this node's iommu-parent. No system-mapper fallback.
    fMapper = IOMapper::copyMapperForDevice(provider);
    if (!fMapper || fMapper->getPageSize() != kAdrenoGPUVAAlign) {
        ALOG("Adreno mapper missing or unsupported page size");
        OSSafeReleaseNULL(fMapper);
        return false;
    }
    // Native tasks have independent locks. The common CB0 free list needs its
    // own lock, and is populated once for the lifetime of this accelerator.
    fGPUVA = IORangeAllocator::withRange(0, kAdrenoGPUVAAlign, 0, IORangeAllocator::kLocking);
    if (!fGPUVA) {
        ALOG("GPU address allocator creation failed");
        OSSafeReleaseNULL(fMapper);
        return false;
    }
    fGPUVA->deallocate(kAdrenoGPUVABase, kAdrenoGPUVASize);
    const size_t len = round_page(ADRENO_NUM_STAMPS * ADRENO_STAMP_BYTES);
    fStamps = IOBufferMemoryDescriptor::withOptions(kIODirectionInOut | kIOMemoryKernelUserShared, len, page_size);
    if (!fStamps) {
        ALOG("stamp page allocation failed");
        OSSafeReleaseNULL(fGPUVA);
        OSSafeReleaseNULL(fMapper);
        return false;
    }
    bzero(fStamps->getBytesNoCopy(), fStamps->getLength());
#if ADRENO_PRESENT_SYNC
    if (!__atomic_load_n(&gAdrenoSyncLock, __ATOMIC_ACQUIRE)) {
        IOSimpleLock *lock = IOSimpleLockAlloc();     // kept for the boot: the display may ask at any time
        __atomic_store_n(&gAdrenoSyncLock, lock, __ATOMIC_RELEASE);
        ALOG("present sync: %s", lock ? "IB and FILL32 mark the IOSurfaces they write" : "no lock, nothing marked");
    }
#endif
    fSubmitLock = IOLockAlloc();
    bool ready = fSubmitLock && prepareStampDMA() && !probeGuestFence();
    if (ready) {
        configureGuestIB(provider);   // optional; only an UNCERTAIN IB probe (latched) stops the device
        ready = !fStampCleanupFailed;
    }
    if (!ready) {
        if (releaseIBArena() && releaseStampDMA()) {
            freeIBWork();
            OSSafeReleaseNULL(fStamps);
            OSSafeReleaseNULL(fGPUVA);
            OSSafeReleaseNULL(fMapper);
        }
        if (fSubmitLock) { IOLockFree(fSubmitLock); fSubmitLock = nullptr; }
        return false;
    }
    ALOG("device mapper 0x%x bound, page size 0x%llx", mapperID, kAdrenoGPUVAAlign);
    return true;
}

// ---------------------------------------------------------------------------------------------------
// Draw submission (design 3.4-3.6). Everything here is optional: without the monitor's IB ABI, or when the
// arena or the probe fails before a doorbell, the device starts with FENCE/FILL32 only and without the
// "AdrenoIBABI" property, so the plug-in keeps refusing render pipelines (explicitly).
static int guestCacheRange(uint64_t address, uint64_t bytes, unsigned direction)
{
    register uint64_t x0 __asm__("x0") = ADRENO_GUEST_CACHE;
    register uint64_t x1 __asm__("x1") = address;
    register uint64_t x2 __asm__("x2") = bytes;
    register uint64_t x3 __asm__("x3") = direction;
    __asm__ volatile("dsb ish\n\thvc %4" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                     : "i"(ADRENO_GUEST_HVC) : "memory", "cc");
    return (x0 || x1 != bytes || x2) ? -1 : 0;
}

bool AdrenoAcceleratorX2::allocateIBWork()
{
    if (fIB) return false;
    fIB = static_cast<struct adreno_ib_work *>(IOMalloc(sizeof(*fIB)));
    fIBTables = static_cast<struct adreno_ib_tables *>(IOMalloc(sizeof(*fIBTables)));
    fIBRelocs = static_cast<struct adreno_ib_reloc *>(IOMalloc(ADRENO_IB_MAX_RELOCS * sizeof(*fIBRelocs)));
    fIBOrder = static_cast<uint16_t *>(IOMalloc(ADRENO_IB_MAX_RELOCS * sizeof(*fIBOrder)));
    fIBUsed = static_cast<uint8_t *>(IOMalloc(ADRENO_IB_MAX_RELOCS * sizeof(*fIBUsed)));
    fIBTargets = static_cast<uint64_t *>(IOMalloc(ADRENO_IB_TARGET_WORDS * sizeof(*fIBTargets)));
    if (!fIB || !fIBTables || !fIBRelocs || !fIBOrder || !fIBUsed || !fIBTargets) return false;
    bzero(fIB, sizeof(*fIB));   // state 0: every submission is refused until adreno_ib_work_init
    int error = adreno_ib_tables_init(fIBTables);
    if (error) ALOG("IB allowlist tables inconsistent (%d): regenerate AdrenoA8xxRegs.h / adreno-ib-tables.h", error);
    return !error;
}

void AdrenoAcceleratorX2::freeIBWork()
{
    if (fIB) IOFree(fIB, sizeof(*fIB));
    if (fIBTables) IOFree(fIBTables, sizeof(*fIBTables));
    if (fIBRelocs) IOFree(fIBRelocs, ADRENO_IB_MAX_RELOCS * sizeof(*fIBRelocs));
    if (fIBOrder) IOFree(fIBOrder, ADRENO_IB_MAX_RELOCS * sizeof(*fIBOrder));
    if (fIBUsed) IOFree(fIBUsed, ADRENO_IB_MAX_RELOCS * sizeof(*fIBUsed));
    if (fIBTargets) IOFree(fIBTargets, ADRENO_IB_TARGET_WORDS * sizeof(*fIBTargets));
    fIB = nullptr; fIBTables = nullptr; fIBRelocs = nullptr; fIBOrder = nullptr; fIBUsed = nullptr; fIBTargets = nullptr;
#if ADRENO_PIPELINE
    pipelineFreeSlot1();
#endif
}

// Kernel-only and GPU-read-only: kIODirectionOut makes the device mapper map the arena without write access
// (INFERRED for the 26A428 IODARTMapper; the monitor requires a no-write leaf for every package page, so a wrong
// guess is refused by the start-time probe with ADRENO_E_TARGET before any doorbell).
bool AdrenoAcceleratorX2::prepareIBArena()
{
    if (!fGPUVA || !fMapper || !fIB || fIBArena || fIBDMA || fStampCleanupFailed) return false;
#if ADRENO_PIPELINE
    const uint64_t length = ADRENO_PIPE_ARENA_BYTES;   // agents/gpu-pipeline Q2: two [zero page][package] slots
#else
    const uint64_t length = ADRENO_IB_ARENA_BYTES;
#endif
    fIBArena = IOBufferMemoryDescriptor::withOptions(kIODirectionOut, length, kAdrenoGPUVAAlign);
    if (!fIBArena) {
        ALOG("IB arena allocation failed");
        return false;
    }
    bzero(fIBArena->getBytesNoCopy(), length);
    IOReturn result = fIBArena->prepare(kIODirectionOut);
    if (result != kIOReturnSuccess) {
        ALOG("IB arena wiring failed: 0x%x", result);
        return false;
    }
    fIBWired = true;
    fIBDMA = IODMACommand::withSpecification(kIODMACommandOutputHost64, 32, length,
        IODMACommand::kMapped, length, kAdrenoGPUVAAlign, fMapper);
    if (!fIBDMA) {
        ALOG("IB arena DMA command allocation failed");
        return false;
    }
    result = fIBDMA->setMemoryDescriptor(fIBArena, false);
    if (result != kIOReturnSuccess) {
        ALOG("IB arena DMA descriptor failed: 0x%x", result);
        return false;
    }
    result = fIBDMA->prepare(0, length);
    fIBDMAActive = fIBDMA->getPreparedOffsetAndLength(nullptr, nullptr) == kIOReturnSuccess;
    if (result != kIOReturnSuccess || !fIBDMAActive) {
        ALOG("IB arena DMA preparation failed: 0x%x", result);
        return false;
    }
    IODMACommand::Segment64 segment = {};
    UInt64 offset = 0;
    UInt32 count = 1;
    result = fIBDMA->gen64IOVMSegments(&offset, &segment, &count);
    if (result != kIOReturnSuccess || count != 1 || offset != length || segment.fLength != length ||
        segment.fIOVMAddr < kAdrenoGPUVABase || segment.fIOVMAddr >= ADRENO_WINDOW_END ||
        segment.fIOVMAddr % kAdrenoGPUVAAlign || length > ADRENO_WINDOW_END - segment.fIOVMAddr) {
        ALOG("IB arena segment rejected: result 0x%x, count %u, offset 0x%llx, address 0x%llx, length 0x%llx",
             result, count, offset, segment.fIOVMAddr, segment.fLength);
        return false;
    }
    if (!fGPUVA->allocateRange(segment.fIOVMAddr, length)) {
        ALOG("IB arena address collides with shared task allocator");
        return false;
    }
    fIBGPUAddress = segment.fIOVMAddr;
    fIBGPULength = length;
#if ADRENO_PIPELINE
    int error = adreno_ib_work_init(fIB, fIBTables, static_cast<uint8_t *>(fIBArena->getBytesNoCopy()), fIBGPUAddress,
                                    ADRENO_PIPE_SLOT_BYTES, fIBRelocs, fIBOrder, fIBUsed, ADRENO_IB_MAX_RELOCS, fIBTargets);
#else
    int error = adreno_ib_work_init(fIB, fIBTables, static_cast<uint8_t *>(fIBArena->getBytesNoCopy()), fIBGPUAddress,
                                    length, fIBRelocs, fIBOrder, fIBUsed, ADRENO_IB_MAX_RELOCS, fIBTargets);
#endif
    if (error) {
        ALOG("IB work binding refused %d", error);
        return false;
    }
    ALOG("IB arena mapped: address 0x%llx, length 0x%llx", fIBGPUAddress, fIBGPULength);
    return true;
}

bool AdrenoAcceleratorX2::releaseIBArena()
{
    // A package the CP may still fetch keeps its arena, mapping and address until reboot (fail-fast leak).
    if (fStampCleanupFailed || (fIB && (fIB->state & ADRENO_IB_STATE_UNCERTAIN))) return false;
#if ADRENO_PIPELINE
    if (fIB2 && (fIB2->state & ADRENO_IB_STATE_UNCERTAIN)) return false;
    if (fIB2) fIB2->state = 0;
#endif
    IOReturn result = kIOReturnSuccess;
    if (fIB) fIB->state = 0;
    if (fIBDMAActive) {
        result = fIBDMA->complete();
        fIBDMAActive = false;
        if (result != kIOReturnSuccess) {
            fStampCleanupFailed = true;
            ALOG("IB arena DMA completion failed: 0x%x; retaining resources", result);
            return false;
        }
    }
    if (fIBDMA) {
        result = fIBDMA->clearMemoryDescriptor(false);
        if (result != kIOReturnSuccess) {
            fStampCleanupFailed = true;
            ALOG("IB arena DMA descriptor cleanup failed: 0x%x; retaining resources", result);
            return false;
        }
        OSSafeReleaseNULL(fIBDMA);
    }
    if (fIBGPUAddress) {
        fGPUVA->deallocate(fIBGPUAddress, fIBGPULength);
        fIBGPUAddress = 0;
        fIBGPULength = 0;
    }
    if (fIBWired) {
        result = fIBArena->complete(kIODirectionOut);
        if (result != kIOReturnSuccess) {
            fStampCleanupFailed = true;
            ALOG("IB arena unwiring failed: 0x%x; retaining backing", result);
            return false;
        }
        fIBWired = false;
    }
    OSSafeReleaseNULL(fIBArena);
    return true;
}

void AdrenoAcceleratorX2::configureGuestIB(IOService *provider)
{
    OSData *abi = OSDynamicCast(OSData, provider->getProperty(ADRENO_GUEST_IB_PROPERTY));
    uint32_t ibABI = 0;
    if (abi && abi->getLength() == sizeof(ibABI)) bcopy(abi->getBytesNoCopy(), &ibABI, sizeof(ibABI));
    if (ibABI != ADRENO_GUEST_IB_ABI) {
        ALOG("monitor has no guest IB ABI (%u): draw submission off", ibABI);
        return;
    }
    if (!allocateIBWork() || !prepareIBArena()) {
        ALOG("IB arena unavailable: draw submission off");
        if (releaseIBArena()) freeIBWork();
        return;
    }
    // R1: the zero page (border colours, empty bindless sets) reaches PoC once; the GPU never snoops it.
    uint64_t va = 0, dwords = 0, bytes = 0;
    int error = guestCacheRange(fIBGPUAddress, ADRENO_IB_ZERO_BYTES, ADRENO_CACHE_TO_DEVICE);
    if (!error) error = adreno_ib_probe(fIB, &va, &dwords, &bytes);
    if (error) {
        ALOG("IB zero page or probe package refused %d: draw submission off", error);
        if (releaseIBArena()) freeIBWork();   // nothing reached the ring
        return;
    }
#if ADRENO_IB_ASYNC
    {
        const struct adreno_ib_async_ops ops = {nullptr, ibAsyncHVC, ibAsyncDelay};
        gAdrenoIBAsync = adreno_ib_async_probe(&ops) ? 1 : 0;
        ALOG("IB completion: %s", gAdrenoIBAsync ? "asynchronous (monitor IB_START/IB_POLL, waits off its token)"
                                                 : "synchronous (the monitor answers no IB_POLL)");
    }
#endif
#if ADRENO_GPU_RECOVER
    if (gAdrenoIBAsync && adreno_recover_guest_probe(&kAdrenoRecoverOps, &gAdrenoRecover))
        ALOG("GPU recovery: monitor level %u, hard IB limit %u s (async bound %llu us), epoch %llu%s", gAdrenoRecover.level,
             gAdrenoRecover.hard_s, adreno_recover_guest_limit_us(&gAdrenoRecover), gAdrenoRecover.epoch,
             gAdrenoRecover.dead ? ", gave up" : "");
    else
        ALOG("GPU recovery: the monitor has none (QUERY refused): a GPU latch lasts the boot");
#endif
    struct adreno_ib_reply reply = {};
    ibRing(this, va, dwords, bytes, &reply);
    fIB->reply = reply;
    int result = adreno_ib_classify(&reply);
    if (result == ADRENO_SUBMIT_E_UNCERTAIN) {
        fIB->state |= ADRENO_IB_STATE_UNCERTAIN;
        fStampCleanupFailed = true;   // as the fence probe: keep every DMA mapping until reboot
        ALOG("IB probe uncertain status=%lld marker=0x%llx ticks=%llu wptr=%llu; retaining DMA",
             reply.status, reply.marker, reply.ticks, reply.wptr);
        return;
    }
    if (result) {
        ALOG("IB probe refused status=%lld (%s): draw submission off", reply.status,
             result == ADRENO_IB_E_GPU_ERROR ? "GPU error after completion, GPU idle" : "before any ring write");
        if (releaseIBArena()) freeIBWork();   // refused, or retired and idle: the arena is not referenced
        return;
    }
#if ADRENO_IB_CS_SAMPLERS
    // CoreImage's multi-input kernels (the folder, document and Safari icons in System Settings panes) load one S# per
    // sampled input, more than the single CS sampler unit the validator has always admitted. Only with the boot-arg
    // adreno_cs_samplers=1 does the validator admit up to ADRENO_IB_CS_SAMPLER_UNITS_MAX and the registry say so; an
    // ADRENO_SETTINGS_ICONS plug-in encodes those kernels only when it reads AdrenoIBComputeSamplers. Both happen here,
    // in configureDevice, before start registers the accelerator and before ADRENO_IB_REGISTRY_PROPERTY: no client can
    // submit an IB, or see the IB path, before the limit is final. Without the boot-arg nothing differs from a build
    // without ADRENO_IB_CS_SAMPLERS.
    uint32_t csSamplers = 0;
    if (PE_parse_boot_argn("adreno_cs_samplers", &csSamplers, sizeof(csSamplers)) && csSamplers) {
        adreno_ib_set_cs_sampler_units(ADRENO_IB_CS_SAMPLER_UNITS_MAX);
        setProperty("AdrenoIBComputeSamplers", (unsigned long long)ADRENO_IB_CS_SAMPLER_UNITS_MAX, 32);
        ALOG("IB: CS sampler loads of up to %u units admitted (adreno_cs_samplers)", ADRENO_IB_CS_SAMPLER_UNITS_MAX);
    }
#endif
    setProperty(ADRENO_IB_REGISTRY_PROPERTY, (unsigned long long)ADRENO_GUEST_IB_ABI, 32);
    setProperty("AdrenoIBCopyABI", (unsigned long long)1, 32);
    ALOG("IB probe completed: package 0x%llx, marker 0x%llx, ticks %llu, wptr %llu", va, reply.marker,
         reply.ticks, reply.wptr);
#if ADRENO_IB_ASYNC
    if (gAdrenoIBAsync)
        ALOG("IB probe through IB_START/IB_POLL: %llu pending, %llu polls, %llu us waited", gAdrenoIBAsyncStats.pending,
             gAdrenoIBAsyncStats.polls, gAdrenoIBAsyncStats.waited_us);
#endif
#if ADRENO_PIPELINE
    if (gAdrenoIBAsync && !pipelineBindSlot1())
        ALOG("pipeline: second package slot unavailable; IB submissions stay synchronous");
#endif
}
