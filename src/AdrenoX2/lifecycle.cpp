// SPDX-License-Identifier: MIT
// The failure latch and GPU recovery, the stamp DMA, and the accelerator's IOService and IOGPU lifecycle overrides.
// Part of AdrenoX2.cpp's translation unit: included from it in order, never compiled on its own.

void AdrenoAcceleratorX2::noteCommandFailure(int result)
{
    // After the fail-fast latch every command buffer fails the same way: log the first ones, then rarely.
    uint64_t n = __atomic_add_fetch(&fFailedCommands, 1, __ATOMIC_RELAXED);
    if (n <= 16 || !(n & 4095))
        ALOG("command submission failed %d (%llu failures; latched %u)", result, n, fStampCleanupFailed);
}

#if ADRENO_GPU_RECOVER
uint32_t AdrenoAcceleratorX2::recoverStatus(int result) const
{
    return adreno_recover_guest_status(result, fStampCleanupFailed || gAdrenoRecover.dead);
}
// R27, R28: only a latch a GPU result set (the stamp DMA, and the IB arena's when there is one, still prepared: a failed
// DMA completion at teardown clears them first), only after the monitor's SETTLE answered 0, at most every 100 ms.
void AdrenoAcceleratorX2::recoverLatched()
{
    if (!fStampCleanupFailed || !gAdrenoRecover.abi || gAdrenoRecover.dead) return;
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
#if ADRENO_PIPELINE
    struct adreno_ib_work *const works[2] = {fIB, fIB2};
#else
    struct adreno_ib_work *const works[2] = {fIB, nullptr};
#endif
    const int dmaOK = fStampDMAActive && (!fIB || fIBDMAActive);
    if (adreno_recover_guest_unlatch(&kAdrenoRecoverOps, &gAdrenoRecover, ns / 1000u, &fStampCleanupFailed, works, 2, dmaOK))
        ALOG("GPU recovered: the monitor proved it idle (epoch %llu, %s); latch cleared after %llu failed commands",
             gAdrenoRecover.epoch, gAdrenoRecover.settle_resets ? "after a reset" : "no reset needed", fFailedCommands);
    else if (gAdrenoRecover.dead)
        ALOG("GPU recovery: the monitor gave up for this boot; the latch stays");
}
#endif

bool AdrenoAcceleratorX2::prepareStampDMA()
{
    if (!fStamps || !fGPUVA || !fMapper || fStampDMA || fStampWired || fStampCleanupFailed)
        return false;
    const uint64_t length = fStamps->getLength();
    if (!length || length % kAdrenoGPUVAAlign) return false;
    IOReturn result = fStamps->prepare(kIODirectionInOut);
    if (result != kIOReturnSuccess) {
        ALOG("stamp wiring failed: 0x%x", result);
        return false;
    }
    fStampWired = true;
    fStampDMA = IODMACommand::withSpecification(kIODMACommandOutputHost64, 32, length,
        IODMACommand::kMapped, length, kAdrenoGPUVAAlign, fMapper);
    if (!fStampDMA) {
        ALOG("stamp DMA command allocation failed");
        return false;
    }
    // Explicit preparation lets teardown account for each successful step.
    result = fStampDMA->setMemoryDescriptor(fStamps, false);
    if (result != kIOReturnSuccess) {
        ALOG("stamp DMA descriptor failed: 0x%x", result);
        return false;
    }
    result = fStampDMA->prepare(0, length);
    // Native prepare increments its active count before attempting the map,
    // and can return a mapping error while still active. Balance that state too.
    fStampDMAActive = fStampDMA->getPreparedOffsetAndLength(nullptr, nullptr) == kIOReturnSuccess;
    if (result != kIOReturnSuccess || !fStampDMAActive) {
        ALOG("stamp DMA preparation failed: 0x%x", result);
        return false;
    }
    IODMACommand::Segment64 segment = {};
    UInt64 offset = 0;
    UInt32 count = 1;
    result = fStampDMA->gen64IOVMSegments(&offset, &segment, &count);
    if (result != kIOReturnSuccess || count != 1 || offset != length ||
        segment.fLength != length || segment.fIOVMAddr < kAdrenoGPUVABase ||
        segment.fIOVMAddr >= ADRENO_WINDOW_END ||
        segment.fIOVMAddr % kAdrenoGPUVAAlign ||
        length > ADRENO_WINDOW_END - segment.fIOVMAddr) {
        ALOG("stamp DMA segment rejected: result 0x%x, count %u, offset 0x%llx, address 0x%llx, length 0x%llx",
             result, count, offset, segment.fIOVMAddr, segment.fLength);
        return false;
    }
    if (!fGPUVA->allocateRange(segment.fIOVMAddr, length)) {
        ALOG("stamp DMA address collides with shared task allocator");
        return false;
    }
    fStampGPUAddress = segment.fIOVMAddr;
    fStampGPULength = length;
    ALOG("stamp DMA mapped: address 0x%llx, length 0x%llx", fStampGPUAddress, fStampGPULength);
    return true;
}

bool AdrenoAcceleratorX2::releaseStampDMA()
{
    // A failed completion cannot establish that the mapper released the pages.
    // Preserve all owned resources in that case, even if free() is called later.
    if (fStampCleanupFailed) return false;
    IOReturn result = kIOReturnSuccess;
    if (fStampDMAActive) {
        result = fStampDMA->complete();
        fStampDMAActive = false;
        if (result != kIOReturnSuccess) {
            fStampCleanupFailed = true;
            ALOG("stamp DMA completion failed: 0x%x; retaining resources", result);
            return false;
        }
    }
    if (fStampDMA) {
        result = fStampDMA->clearMemoryDescriptor(false);
        if (result != kIOReturnSuccess) {
            fStampCleanupFailed = true;
            ALOG("stamp DMA descriptor cleanup failed: 0x%x; retaining resources", result);
            return false;
        }
        OSSafeReleaseNULL(fStampDMA);
    }
    if (fStampGPUAddress) {
        fGPUVA->deallocate(fStampGPUAddress, fStampGPULength);
        fStampGPUAddress = 0;
        fStampGPULength = 0;
    }
    if (fStampWired) {
        result = fStamps->complete(kIODirectionInOut);
        if (result != kIOReturnSuccess) {
            fStampCleanupFailed = true;
            ALOG("stamp unwiring failed: 0x%x; retaining backing", result);
            return false;
        }
        fStampWired = false;
    }
    return true;
}

void AdrenoAcceleratorX2::teardownDevice(IOService *) {}

void AdrenoAcceleratorX2::populateAccelConfig(IOGPUConfig *config)
{
    // config == this + 0x228 (IOGPU::start 0xfffffe000a8850ac). IOGPU::initializeConfigStructure has
    // already written the defaults; only the fields below differ from them.
    uint8_t *c = reinterpret_cast<uint8_t *>(config);
    *reinterpret_cast<const char **>(c + 0x00) = ADRENO_ACCEL_NAME;                // accelName (s_get_name)
    *reinterpret_cast<uint32_t *>(c + 0x08) |= ADRENO_RESOURCE_AUTO_MAP;           // native resource mapping
    *reinterpret_cast<uint32_t *>(c + 0x18) = ADRENO_NUM_STAMPS;                   // numEventChannels
    *reinterpret_cast<uint32_t *>(c + 0x4c) = ADRENO_RESOURCE_ARGS_KERNEL_TAIL;    // resource-args tail
    *reinterpret_cast<uint32_t *>(c + 0x54) = ADRENO_WAIT_FOR_GPU_TIMEOUT_S;       // waitForGPUTimeoutSeconds
    *reinterpret_cast<uint32_t *>(c + 0x90) = ADRENO_QUEUE_ARGS_SIZE;              // queue-args size (sel 7)
}

bool AdrenoAcceleratorX2::start(IOService *provider)
{
#if ADRENO_RESIDENT_BUDGET_MIB
    AdrenoGPUMapStart();   // the residency budget, before IOGPU::start makes the first map (R13)
#endif
    if (!IOGPU::start(provider)) {
        ALOG("IOGPU::start failed");
        return false;
    }
    IOGPUEventMachine *em = *reinterpret_cast<IOGPUEventMachine **>(reinterpret_cast<uint8_t *>(this) +
                                                                    kIOGPUEventMachineOffset);
    if (!em || !fStamps) {
        ALOG("no event machine (%p) or stamp page (%p)", em, fStamps);
        IOGPU::stop(provider);
        return false;
    }
    // Only AGX calls this (AGXAccelerator::start 0xfffffe0008a312e4); IOGPUFamily never does, and
    // testEvent dereferences the table it builds as soon as an event carries a stamp.
    em->setStampBaseAddress(reinterpret_cast<volatile unsigned int *>(fStamps->getBytesNoCopy()));
#if ADRENO_PIPELINE
    pipelineStart(em);   // agents/gpu-pipeline: optional; off leaves every IB synchronous
#endif

    setProperty("model", "Adreno X2-90");
    if (AdrenoEarlyStopEnabled())   // before the publish notification the plug-in's device creation follows
        setProperty(ADRENO_EARLY_STOP_REGISTRY_PROPERTY, 1ULL, 32);
    enableAccelerator();   // accelerator+0x220 bit 0
    registerService();     // Metal waits for the IOAccelerator publish notification; IOGPU::start never registers
    { extern bool AdrenoDisplayAttach(IOService *); AdrenoDisplayAttach(this); }   // Stage 8 panel child (AdrenoDisplay.cpp); never fatal
    ALOG("started: %u stamps, queue args 0x%x, synchronous GPU fence transport, IB %s", ADRENO_NUM_STAMPS,
         ADRENO_QUEUE_ARGS_SIZE, fIB && (fIB->state & ADRENO_IB_STATE_READY) ? "ready" : "off");
#if ADRENO_RESIDENT_REPIN
    ALOG("GPU mappings resident across unwire: budget %llu MiB, contiguous-run inserts",
         (unsigned long long)(AdrenoGPUMapBudget() >> 20));   // agents/resident-evict: boot-arg adreno_resident_mib
#else
    ALOG("GPU mappings resident across unwire: budget %llu MiB, contiguous-run inserts",
         (unsigned long long)(kAdrenoResidentBudget >> 20));
#endif
    return true;
}

void AdrenoAcceleratorX2::free()
{
#if ADRENO_PIPELINE
    pipelineStop();      // O2: no flight left on the arena below
#endif
    // The IB arena shares fGPUVA and fMapper: after an uncertain submission both it and they stay (leak).
    if (releaseIBArena() && releaseStampDMA()) {
        freeIBWork();
        OSSafeReleaseNULL(fStamps);
        OSSafeReleaseNULL(fGPUVA);
        OSSafeReleaseNULL(fMapper);
    }
    if (fSubmitLock) { IOLockFree(fSubmitLock); fSubmitLock = nullptr; }
    IOGPU::free();
}

// The plug-in's unrecoverable-failure report (AdrenoContract.h ADRENO_EARLY_STOP_FAILURE_KEY): exactly that key,
// a 64-bit number, only while this launch enabled early stop. Every other property change keeps IOGPU's answer.
IOReturn AdrenoAcceleratorX2::setProperties(OSObject *properties)
{
    OSDictionary *dictionary = OSDynamicCast(OSDictionary, properties);
    OSObject *report = dictionary ? dictionary->getObject(ADRENO_EARLY_STOP_FAILURE_KEY) : nullptr;
    if (!report) return IOGPU::setProperties(properties);
    OSNumber *value = OSDynamicCast(OSNumber, report);
    if (dictionary->getCount() != 1 || !value || value->numberOfBits() != 64) return kIOReturnBadArgument;
    if (!AdrenoEarlyStopEnabled()) return kIOReturnUnsupported;
    int status = AdrenoEarlyStopRequest(EARLY_STOP_REASON_PLUGIN_FAILURE, value->unsigned64BitValue());
    return status < 0 ? kIOReturnNotPermitted : kIOReturnSuccess;
}

IOMemoryDescriptor *AdrenoAcceleratorX2::getStampMemory(unsigned int *offset)
{
    // IOGPUDeviceUserClient::deviceUserClientStart retains the result and maps it read-only into the
    // client (options 0x04001001); NULL there fails every Metal device open.
    if (offset) *offset = 0;
    return fStamps;
}

// Factories: IOGPU's create* wrappers run init() on what these return (IOGPU::createEventMachine,
// createSysMemory, createResource, createMemoryMap, s_new_command_queue), so these only allocate.
IOGPUEventMachine *AdrenoAcceleratorX2::newEventMachine() { return OSTypeAlloc(AdrenoEventMachine); }
IOGPUSysMemory *AdrenoAcceleratorX2::newSysMemory() { return OSTypeAlloc(AdrenoSysMemory); }
IOGPUResource *AdrenoAcceleratorX2::newResource() { return OSTypeAlloc(AdrenoResource); }
IOGPUMemoryMap *AdrenoAcceleratorX2::newMemoryMap() { return AdrenoMemoryMap::withMapper(fMapper); }
IOGPUCommandQueue *AdrenoAcceleratorX2::newCommandQueue() { return AdrenoCommandQueue::withAccelerator(this); }

// Task factories return an initialised +1 task, as AGXAccelerator::create{User,Kernel}GPUTask do.
IOGPUTask *AdrenoAcceleratorX2::createUserGPUTask(task *owner) { return AdrenoTask::withTask(this, owner, fGPUVA); }
IOGPUTask *AdrenoAcceleratorX2::createKernelGPUTask() { return AdrenoTask::withTask(this, kernel_task, fGPUVA); }
