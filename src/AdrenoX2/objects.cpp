// SPDX-License-Identifier: MIT
// The IOGPU object overrides: event machine, resource, task (WindowServer by entitlement) and the command queue,
// whose kernel commands (FENCE, FILL32, IB) go to the accelerator.
// Part of AdrenoX2.cpp's translation unit: included from it in order, never compiled on its own.

// ---------------------------------------------------------------------------------------------------
// Event machine: stamps exist (so user space can map them) but nothing writes them in stage 2.
void AdrenoEventMachine::writeStamp(int, vendevtCommandRec *, unsigned int) {}
void AdrenoEventMachine::prepareBarrier(vendevtBarrierRec *) const {}
void AdrenoEventMachine::completeBarrier(vendevtBarrierRec *) const {}
void AdrenoEventMachine::writeBarrierElement(vendevtBarrierRec *, int, unsigned int) const {}

// ---------------------------------------------------------------------------------------------------
void AdrenoResource::calculateIOSurfaceDeviceCacheVRAMBytes(unsigned long long *bytes)
{
    if (bytes) *bytes = 0;
}

IOMemoryDescriptor *AdrenoResource::getApertureMemoryDescriptor(unsigned long long *)
{
    return nullptr;   // same as AGXResource::getApertureMemoryDescriptor
}

// ---------------------------------------------------------------------------------------------------
// Resource mapping and descriptor lifetime are implemented in AdrenoMemoryMap.cpp.

// ---------------------------------------------------------------------------------------------------
// WindowServer, by two entitlements only it holds among 26A428's GPU and IOMFB clients (SkyLight's
// WindowServer; IOMFB_bics_daemon, IOMFBDiagnose and IOMFB_FDR_Loader share its iomfb.set-block, not these).
static bool entitled(task_t owner, const char *name)
{
    OSObject *value = IOUserClient::copyClientEntitlement(owner, name);
    bool yes = value == kOSBooleanTrue;
    OSSafeReleaseNULL(value);
    return yes;
}
static bool isWindowServer(task_t owner)
{
    return owner != kernel_task && entitled(owner, "com.apple.QuartzCore.secure-mode") &&
           entitled(owner, "com.apple.private.gpuwrangler");
}
static uint32_t gWindowServerTasks;
#if ADRENO_SOFTGPU
extern "C" void *get_bsdtask_info(task_t);       // XNU: the BSD proc of a task (not in the Kernel.framework headers)
extern "C" int proc_pid(void *);
static int32_t taskPid(task_t owner)
{
    void *proc = get_bsdtask_info(owner);
    return proc ? (int32_t)proc_pid(proc) : -1;
}
#endif

AdrenoTask *AdrenoTask::withTask(IOGPU *gpu, task_t owner, IORangeAllocator *allocator)
{
    if (!gpu || !owner || !allocator) return nullptr;
    AdrenoTask *t = OSTypeAlloc(AdrenoTask);
    // Native IOGPUTask retains this shared allocator and releases it in free().
    // Never seed a second free list: CB0 is shared by user and kernel tasks.
    if (t && !t->init(gpu, owner, 1, &allocator)) {
        t->release();
        t = nullptr;
    }
    if (t && AdrenoEarlyStopEnabled() && isWindowServer(owner)) {
        t->fWindowServer = true;
        t->fSequence = __atomic_add_fetch(&gWindowServerTasks, 1, __ATOMIC_RELAXED);
    }
#if ADRENO_SOFTGPU
    if (t) {        // whose quota the task's SoftGPU context counts against (softgpu-ctx.h)
        t->fSoftCtx.pid = owner == kernel_task ? 0 : taskPid(owner);
        t->fSoftCtx.ws = isWindowServer(owner) ? 1 : 0;
    }
#endif
#if ADRENO_PIPELINE
    if (t) t->fPipelineClient = isWindowServer(owner);   // agents/gpu-pipeline R4: adreno_pipeline=1 pipelines these
#endif
    return t;
}

uint64_t AdrenoTask::allocate(IOGPUMemoryMap *map)
{
    uint64_t va = IOGPUTask::allocate(map);
    if (va && __atomic_load_n(&fAllocations, __ATOMIC_RELAXED) != UINT32_MAX)
        __atomic_add_fetch(&fAllocations, 1, __ATOMIC_RELAXED);
    return va;
}

void AdrenoTask::free()
{
    // WindowServer keeps its GPU device while it runs; its task going away after it allocated is the process
    // ending. A device it opened only to probe never allocates and never counts.
    uint32_t allocations = __atomic_load_n(&fAllocations, __ATOMIC_RELAXED);
    if (fWindowServer && allocations)
        AdrenoEarlyStopClientDied((uint64_t)fSequence << 32 | allocations);
#if ADRENO_SOFTGPU
    sgpu_ctx_task_gone(&fSoftCtx, AdrenoSoftGPUCtxGone);      // the next stream destroys it, with all of its objects
#endif
    IOGPUTask::free();
}

// ---------------------------------------------------------------------------------------------------
AdrenoCommandQueue *AdrenoCommandQueue::withAccelerator(AdrenoAcceleratorX2 *accelerator)
{
    if (!accelerator) return nullptr;
    auto *queue = OSTypeAlloc(AdrenoCommandQueue);
    if (queue) { queue->fAccelerator = accelerator; accelerator->retain(); }
    return queue; // native IOGPU initializes the queue afterwards
}
void AdrenoCommandQueue::free()
{
    OSSafeReleaseNULL(fAccelerator);
    IOGPUCommandQueue::free();
}
struct AdrenoFenceContext {
    AdrenoAcceleratorX2 *accelerator;
    IOGPUSegmentResourceList *list;
#if ADRENO_PIPELINE
    IOGPUCommandQueue *queue;
#endif
};
int AdrenoCommandQueue::fence(void *context)
{
    auto *submission=static_cast<AdrenoFenceContext *>(context);
    return submission->accelerator ? submission->accelerator->submitFence(
        OSDynamicCast(AdrenoSegmentList,submission->list)) : ADRENO_SUBMIT_E_INACTIVE;
}
int AdrenoCommandQueue::fill(void *context, const struct adreno_fill32_request *request)
{
    auto *submission=static_cast<AdrenoFenceContext *>(context);
    return submission->accelerator ? submission->accelerator->submitFill(
        OSDynamicCast(AdrenoSegmentList,submission->list),request) : ADRENO_SUBMIT_E_INACTIVE;
}
int AdrenoCommandQueue::ib(void *context, const void *command, uint64_t bytes)
{
    auto *submission=static_cast<AdrenoFenceContext *>(context);
#if ADRENO_PIPELINE
    return submission->accelerator ? submission->accelerator->submitIB(
        OSDynamicCast(AdrenoSegmentList,submission->list),command,bytes,submission->queue) : ADRENO_SUBMIT_E_INACTIVE;
#else
    return submission->accelerator ? submission->accelerator->submitIB(
        OSDynamicCast(AdrenoSegmentList,submission->list),command,bytes) : ADRENO_SUBMIT_E_INACTIVE;
#endif
}
#if ADRENO_SOFTGPU
int AdrenoCommandQueue::stream(void *context, const void *command, uint64_t bytes)
{
    auto *submission=static_cast<AdrenoFenceContext *>(context);
    return submission->accelerator ? submission->accelerator->submitStream(
        OSDynamicCast(AdrenoSegmentList,submission->list),command,bytes) : ADRENO_SUBMIT_E_INACTIVE;
}
#endif
IOGPUSegmentResourceList *AdrenoCommandQueue::parseSegmentList(IOGPUDevice *device,
                                                               IOGPUSegmentResourceListHeader const *header,
                                                               IOGPUDeviceShmem *shmem)
{
    AdrenoSegmentList *list = OSTypeAlloc(AdrenoSegmentList);
    if (list && !list->initWithSharedResourceList(device, header, shmem)) {
        list->release();
        list = nullptr;
    }
    return list;   // +1; IOGPUCommandQueue::processCommandBuffer releases it
}

void AdrenoCommandQueue::processSegmentKernelCommand(IOGPUSegmentResourceList *list, IOGPUKernelCommand const *command,
                                                     IOGPUKernelCommand const *end, unsigned char const *segmentStart,
                                                     unsigned char const *segmentEnd,
                                                     IOGPUCommandQueueCommandBufferArgs const *args)
{
    uintptr_t start = reinterpret_cast<uintptr_t>(command), limit = reinterpret_cast<uintptr_t>(end);
#if ADRENO_PIPELINE
    AdrenoFenceContext submission={fAccelerator,list,this};
#else
    AdrenoFenceContext submission={fAccelerator,list};
#endif
    // FENCE, FILL32 and ADRENO_KCMD_IB, decoded by the same pure-C dispatcher the host tests drive.
#if ADRENO_SOFTGPU
    const struct adreno_kernel_ops ops={&submission,fence,fill,ib,stream};   // + SOFTGPU_KCMD_STREAM
#else
    const struct adreno_kernel_ops ops={&submission,fence,fill,ib};
#endif
    int result = adreno_kernel_command(command, limit >= start ? limit-start : 0, &ops);
    if (result == ADRENO_SUBMIT_GENERIC) {
        IOGPUCommandQueue::processSegmentKernelCommand(list, command, end, segmentStart, segmentEnd, args);
        return;
    }
    if (result) {
        // Native IOGPU uses 0xa for malformed/unsupported kernel commands; the command buffer then completes
        // with an error status instead of Completed (4).
        if (result==ADRENO_SUBMIT_E_UNCERTAIN || result==ADRENO_LIST_E_VISIBILITY) {
            // Retirement or CPU visibility could not be established. Preserve
            // preparation ownership before native queue cleanup drops list.
            if (auto *resources=OSDynamicCast(AdrenoSegmentList,list))
                resources->retainAfterTimeout();
        }
#if ADRENO_SOFTGPU
        if (result<0 && result>=SGPU_E_SKIPPED) {
            // A SoftGPU descriptor result: IOGPU's status per spec 1.6 (hang, victim, page fault, invalid).
            setSubmissionError(sgpu_guest_iogpu_status(result));
            if (fAccelerator) fAccelerator->noteCommandFailure(result);
            return;
        }
#endif
#if ADRENO_GPU_RECOVER
        // R26: the guilty command buffer of a reset gets IOGPU's hang status (3, 0xb after a fault; IOGPU counts the
        // strike), one refused while latched is a victim (5); everything else keeps 0xa.
        setSubmissionError(fAccelerator ? fAccelerator->recoverStatus(result) : 0xa);
#else
        setSubmissionError(0xa);
#endif
        if (fAccelerator) fAccelerator->noteCommandFailure(result);
        else ALOG("command submission failed %d", result);
    }
}
