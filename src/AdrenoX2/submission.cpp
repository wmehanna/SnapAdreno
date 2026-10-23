// SPDX-License-Identifier: MIT
// Command submission under fSubmitLock: the guest fence probe, FENCE, FILL32, the IB ring and submitIB.
// Part of AdrenoX2.cpp's translation unit: included from it in order, never compiled on its own.

int AdrenoAcceleratorX2::probeGuestFence()
{
    // Probe padding after the public stamp slots, before publishing the device.
    const uint64_t offset = ADRENO_NUM_STAMPS * ADRENO_STAMP_BYTES;
    const uint32_t value = 0x534e5001;
    if (!fStampDMAActive || !fStampGPUAddress || fStampGPULength < offset + sizeof(value))
        return ADRENO_SUBMIT_E_INACTIVE;
    volatile uint32_t *probe = reinterpret_cast<volatile uint32_t *>(
        static_cast<uint8_t *>(fStamps->getBytesNoCopy()) + offset);
    *probe = ~value;
    register uint64_t x0 __asm__("x0") = ADRENO_GUEST_FENCE;
    register uint64_t x1 __asm__("x1") = fStampGPUAddress + offset;
    register uint64_t x2 __asm__("x2") = value;
    register uint64_t x3 __asm__("x3") = 0;
    __asm__ volatile("dsb ish\n\thvc %4" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                     : "i"(ADRENO_GUEST_HVC) : "memory", "cc");
    uint32_t observed = *probe;
    // Ring-wrap aware (design S1): x3 is the new WPTR and moves backwards across a wrap, so it is only
    // logged. Only refusals that precede every ring write are clean; the monitor reports a doorbell dropped
    // after a wrap's padding (or after the fence words) as ADRENO_E_RING or ADRENO_E_FENCE, so those, like
    // any unknown status or inconsistent reply, are UNCERTAIN.
    int result = adreno_fence_classify((int64_t)x0, x1, value, observed);
    if (result == ADRENO_SUBMIT_E_UNCERTAIN) {
        // A failed poll does not prove the CP is stopped. Keep this IOVA and
        // its backing until reboot, including when IOGPU calls free().
        fStampCleanupFailed = true;
        ALOG("guest fence failed status=0x%llx reply=0x%llx observed=0x%x ticks=%llu wptr=%llu; retaining DMA",
             x0, x1, observed, x2, x3);
        return result;
    }
    if (result) {
        ALOG("guest fence refused before any ring write status=%lld wptr=%llu", (int64_t)x0, x3);
        return result;
    }
    if (!fSubmittedFences)
        ALOG("GPU fence verified iova=0x%llx value=0x%x ticks=%llu wptr=%llu",
             fStampGPUAddress + offset, observed, x2, x3);
    return 0;
}

int AdrenoAcceleratorX2::fenceWork(void *context)
{
    // 0, ADRENO_SUBMIT_E_INACTIVE (the list completes without FROM_DEVICE and is not frozen) or UNCERTAIN.
    return static_cast<AdrenoAcceleratorX2 *>(context)->probeGuestFence();
}
struct AdrenoGatedSubmission {
    AdrenoAcceleratorX2 *accelerator;
    AdrenoSegmentList *list;
    const struct adreno_fill32_request *fill;
    const void *command;
    uint64_t bytes;
};
int AdrenoAcceleratorX2::fenceGated(void *context)
{
    auto *s = static_cast<AdrenoGatedSubmission *>(context);
    return s->list->submit(fenceWork, s->accelerator);
}
int AdrenoAcceleratorX2::fillGated(void *context)
{
    auto *s = static_cast<AdrenoGatedSubmission *>(context);
    return s->list->fill(s->fill, fillWork, s->accelerator);
}
int AdrenoAcceleratorX2::ibGated(void *context)
{
    auto *s = static_cast<AdrenoGatedSubmission *>(context);
    return s->list->ib(s->accelerator->fIB, s->command, s->bytes, ibRing, s->accelerator);
}
int AdrenoAcceleratorX2::submitFence(AdrenoSegmentList *resources)
{
    if (!resources) return ADRENO_LIST_E_ARGUMENT;
    if (!fSubmitLock) return ADRENO_SUBMIT_E_INACTIVE;
    IOLockLock(fSubmitLock);
#if ADRENO_GPU_RECOVER
    recoverLatched();
#endif
#if ADRENO_PIPELINE
    // agents/gpu-pipeline Q3: with an IB in flight the FENCE folds into it: its list is validated, nothing is rung; the
    // command buffer completes through the queue event, which holds the flight's stamp.
    if (fPipeOn && !fStampCleanupFailed && adreno_pipe_fold(&fPipe)) {
        const int folded = resources->pipelineValidate();
        IOLockUnlock(fSubmitLock);
        return folded;
    }
#endif
    AdrenoGatedSubmission submission = {this, resources, nullptr, nullptr, 0};
    // Fail-fast: after an UNCERTAIN/VISIBILITY result no FENCE, FILL32 or IB reaches the monitor again.
    int result = adreno_submit_gated(&fStampCleanupFailed, fenceGated, &submission);
    if (!result) fSubmittedFences++;
    IOLockUnlock(fSubmitLock);
    return result;
}

int AdrenoAcceleratorX2::fillWork(void *, uint64_t address, uint64_t bytes, uint32_t pattern)
{
    register uint64_t x0 __asm__("x0")=ADRENO_GUEST_FILL32;
    register uint64_t x1 __asm__("x1")=address;
    register uint64_t x2 __asm__("x2")=bytes;
    register uint64_t x3 __asm__("x3")=pattern;
    __asm__ volatile("dsb ish\n\thvc %4" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                     : "i"(ADRENO_GUEST_HVC) : "memory", "cc");
    int64_t status=(int64_t)x0;
    if (!status) return x1==bytes ? 0 : ADRENO_FILL32_E_UNCERTAIN;
    // Only these documented helper refusals precede a possible doorbell.
    // Unknown status or an inconsistent error reply retains ownership.
    if (x1) return ADRENO_FILL32_E_UNCERTAIN;
    switch (status) {
    case ADRENO_E_PARAMS: case ADRENO_E_WINDOW: case ADRENO_E_TARGET:
    case ADRENO_E_CX: case ADRENO_E_GX: case ADRENO_E_HALT:
    case ADRENO_E_CB0: case ADRENO_E_FAULT: case ADRENO_E_SQE: case ADRENO_E_RING:
        return (int)status;
    default: return ADRENO_FILL32_E_UNCERTAIN;
    }
}
int AdrenoAcceleratorX2::submitFill(AdrenoSegmentList *resources,
                                   const struct adreno_fill32_request *request)
{
    if (!resources || !request) return ADRENO_LIST_E_ARGUMENT;
    if (!fSubmitLock) return ADRENO_SUBMIT_E_INACTIVE;
#if ADRENO_LATCH
    const AdrenoGPUBusy busy;                // G1: counted from before the lock wait to the return
#endif
#if ADRENO_PRESENT_SYNC
    const AdrenoSurfaceWrites writes(resources);   // K1: its written IOSurfaces, until the return (after the unlock)
#endif
    IOLockLock(fSubmitLock);
#if ADRENO_GPU_RECOVER
    recoverLatched();
#endif
#if ADRENO_PIPELINE
    AdrenoFlightRecord *post[4];
    unsigned posts = 0;
    if (fPipeOn) pipelineFinishLocked(post, &posts);   // agents/gpu-pipeline Q4: the flight first (one ring user)
#endif
    AdrenoGatedSubmission submission = {this, resources, request, nullptr, 0};
    int result = adreno_submit_gated(&fStampCleanupFailed, fillGated, &submission);
    if (!result) {
        fSubmittedFills++;
        if (fSubmittedFills==1)
            ALOG("GPU resource fill completed id=%u offset=0x%llx bytes=0x%llx pattern=0x%x",
                 request->resource_id,request->offset,request->bytes,request->pattern);
    }
    IOLockUnlock(fSubmitLock);
#if ADRENO_PIPELINE
    pipelinePost(post, posts);
#endif
    return result;
}

// ADRENO_GUEST_IB (compat/adreno-guest.h): the package is already copied, relocated and validated in the arena.
int AdrenoAcceleratorX2::ibRing(void *, uint64_t package, uint64_t dwords, uint64_t bytes,
                                struct adreno_ib_reply *reply)
{
#if ADRENO_IB_ASYNC
    if (gAdrenoIBAsync) {
        // IB_START, then IB_POLLs with IODelay from 10 to 200 us until a final reply (FAILURE-MODES A9, A10, A17):
        // returns only once the IB retired or is UNCERTAIN, with ADRENO_GUEST_IB's reply shape, so the caller's
        // classification and the resource list's FROM_DEVICE order are unchanged. fSubmitLock stays held.
#if ADRENO_GPU_RECOVER
        const struct adreno_ib_async_ops ops = {nullptr, ibAsyncHVC, ibAsyncDelay,
                                                adreno_recover_guest_limit_us(&gAdrenoRecover)};
#else
        const struct adreno_ib_async_ops ops = {nullptr, ibAsyncHVC, ibAsyncDelay};
#endif
        uint64_t out[4];
        adreno_ib_async_run(&ops, package, dwords, bytes, out, &gAdrenoIBAsyncStats);
        reply->status = (int64_t)out[0];
        reply->marker = out[1];
        reply->ticks = out[2];
        reply->wptr = out[3];
        return 0;
    }
#endif
    register uint64_t x0 __asm__("x0") = ADRENO_GUEST_IB;
    register uint64_t x1 __asm__("x1") = package;
    register uint64_t x2 __asm__("x2") = dwords;
    register uint64_t x3 __asm__("x3") = bytes;
    // The arena stores must be complete before the monitor cleans the package to PoC and rings it.
    __asm__ volatile("dsb ish\n\thvc %4" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                     : "i"(ADRENO_GUEST_HVC) : "memory", "cc");
    reply->status = (int64_t)x0;
    reply->marker = x1;
    reply->ticks = x2;
    reply->wptr = x3;
    return 0;
}
#if ADRENO_PIPELINE
#if ADRENO_PERF_FRAMES
// The pacing and perf gpu counters of one retired IB (submitIB's block below, moved): at the submission's return for a
// synchronous IB, at the retirement for a flight (agents/gpu-pipeline D5).
static void AdrenoNoteIBRetired(uint64_t gpuTicks)
{
    const uint64_t completedAt = mach_absolute_time();
    __atomic_store_n(&gAdrenoIBCompletedAt, completedAt, __ATOMIC_RELAXED);
#if ADRENO_PERF_GPU
    __atomic_fetch_add(&gAdrenoIBGpuTicks, gpuTicks, __ATOMIC_RELAXED);
    uint64_t seen = __atomic_load_n(&gAdrenoIBGpuTicksMax, __ATOMIC_RELAXED);
    while (gpuTicks > seen && !__atomic_compare_exchange_n(&gAdrenoIBGpuTicksMax, &seen, gpuTicks, false,
                                                           __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
#else
    (void)gpuTicks;
#endif
    __atomic_fetch_add(&gAdrenoIBCompleted, 1, __ATOMIC_RELEASE);
}
#endif
// The command buffer's IOGPUEventFence during its submission (queue+0x518), or null.
static OSObject *AdrenoCurrentFence(IOGPUCommandQueue *queue)
{
    if (!queue) return nullptr;
    OSObject *fence = *reinterpret_cast<OSObject **>(reinterpret_cast<uint8_t *>(queue) + kIOGPUQueueCurrentFenceOffset);
    return OSDynamicCast(IOGPUEventFence, fence);
}
// agents/gpu-pipeline: a pipelined client's IB returns once it is in flight (compat/adreno-ib-pipeline.h); any other
// IB, and every IB while the engine is off, runs as before. The present-sync marks of an IB that stays in flight move to
// its record (D1); the finished flights' post work (unmark, IOGPU signal, list release) runs after the unlock (C6).
int AdrenoAcceleratorX2::submitIB(AdrenoSegmentList *resources, const void *command, uint64_t bytes,
                                  IOGPUCommandQueue *queue)
{
    if (!resources || !command) return ADRENO_LIST_E_ARGUMENT;
    if (!fSubmitLock) return ADRENO_SUBMIT_E_INACTIVE;
#if ADRENO_LATCH
    const AdrenoGPUBusy busy;                // G1: counted from before the lock wait to the return
#endif
    AdrenoSurfaceWrites writes(resources);   // K1: its written IOSurfaces, until the return or, in flight, retirement
#if ADRENO_PERF_GPU
    const uint64_t lockStart = mach_absolute_time();
    IOLockLock(fSubmitLock);
    const uint64_t submitStart = mach_absolute_time();
#else
    IOLockLock(fSubmitLock);
#endif
#if ADRENO_GPU_RECOVER
    recoverLatched();
#endif
    int result;
    bool inFlight = false;
    struct adreno_ib_work *w = fIB;
    if (__atomic_load_n(&fPipeOn, __ATOMIC_ACQUIRE) && fIB) {
#if ADRENO_PRESENT_RELATCH
        // EP2: a flight that is already final is finished before this submission's TO_DEVICE (one POLL)
        if (gAdrenoPipeEarly.on && fPipe.flight.active && !fStampCleanupFailed && adreno_pipe_try_finish(&fPipe, &fPipeOps))
            gAdrenoPipeEarly.pre++;
#endif
        if (fStampCleanupFailed) {
            result = ADRENO_SUBMIT_E_INACTIVE;            // the fail-fast gate, as adreno_submit_gated
        } else {
            AdrenoFlightRecord *rec = pipelineEligible(resources) ? recordAlloc() : nullptr;
            if (rec) { rec->list = resources; rec->queue = queue; rec->fence = AdrenoCurrentFence(queue); }
            const struct adreno_resource_list_ops ops = resources->pipelineOps();
            int flying = 0;
#if ADRENO_PRESENT_RELATCH
            result = adreno_pipe_submit(&fPipe, &fPipeOps, resources->pipelineResources(), &ops, command, bytes,
                                        AdrenoSegmentList::pipelineResolve, this, rec, rec != nullptr, &flying,
                                        &gAdrenoPipeEarly);   // EP3: its TO_DEVICE assists a flight it finds
#else
            result = adreno_pipe_submit(&fPipe, &fPipeOps, resources->pipelineResources(), &ops, command, bytes,
                                        AdrenoSegmentList::pipelineResolve, this, rec, rec != nullptr, &flying);
#endif
            w = fPipe.slot[fPipe.last_slot];
            if (result == ADRENO_SUBMIT_E_UNCERTAIN || result == ADRENO_LIST_E_VISIBILITY)
                fStampCleanupFailed = true;               // as adreno_submit_gated latches
            if (flying) {
                resources->retain();                      // R2: until its retirement's post work
                if (rec->fence) rec->fence->retain();
                writes.handTo(rec->ids, &rec->nids, &rec->wild);
                rec->queue = nullptr;
                inFlight = true;
            } else if (rec) {
                rec->list = nullptr; rec->queue = nullptr; rec->fence = nullptr;
                __atomic_store_n(&rec->busy, 0u, __ATOMIC_RELEASE);
            }
        }
    } else {
        AdrenoGatedSubmission submission = {this, resources, nullptr, command, bytes};
        result = fIB ? adreno_submit_gated(&fStampCleanupFailed, ibGated, &submission) : ADRENO_SUBMIT_E_INACTIVE;
    }
#if ADRENO_PERF_FRAMES
    if (!result && !inFlight) AdrenoNoteIBRetired(w->reply.ticks);
#if ADRENO_PERF_GPU
    if (!result) {
        __atomic_fetch_add(&gAdrenoIBSubmitTicks, mach_absolute_time() - submitStart, __ATOMIC_RELAXED);
        __atomic_fetch_add(&gAdrenoIBLockTicks, submitStart - lockStart, __ATOMIC_RELAXED);
    }
#endif
#endif
    if (!result && ++fSubmittedIBs == 1)
        ALOG("GPU IB %s: %u dwords, %u relocations, marker 0x%llx, ticks %llu, wptr %llu", inFlight ? "in flight" : "completed",
             w->command.ib_dwords, w->command.reloc_count, w->reply.marker, w->reply.ticks, w->reply.wptr);
    if (result && w && result != ADRENO_SUBMIT_E_INACTIVE)
        ALOG("GPU IB refused %d at 0x%x (monitor status %lld, ib state 0x%x)", result, w->error_at,
             w->reply.status, w->state);
    AdrenoFlightRecord *post[4];
    unsigned posts = 0;
    pipelineTakePost(post, &posts);
    IOLockUnlock(fSubmitLock);
    pipelinePost(post, posts);
    if (inFlight) pipelineArm();
    return result;
}
#else
int AdrenoAcceleratorX2::submitIB(AdrenoSegmentList *resources, const void *command, uint64_t bytes)
{
    if (!resources || !command) return ADRENO_LIST_E_ARGUMENT;
    if (!fSubmitLock) return ADRENO_SUBMIT_E_INACTIVE;
#if ADRENO_LATCH
    const AdrenoGPUBusy busy;                // G1: counted from before the lock wait to the return
#endif
#if ADRENO_PRESENT_SYNC
    const AdrenoSurfaceWrites writes(resources);   // K1: its written IOSurfaces, until the return (after the unlock)
#endif
#if ADRENO_PERF_GPU
    const uint64_t lockStart = mach_absolute_time();
    IOLockLock(fSubmitLock);
    const uint64_t submitStart = mach_absolute_time();
#else
    IOLockLock(fSubmitLock);
#endif
#if ADRENO_GPU_RECOVER
    recoverLatched();
#endif
    AdrenoGatedSubmission submission = {this, resources, nullptr, command, bytes};
    int result = fIB ? adreno_submit_gated(&fStampCleanupFailed, ibGated, &submission) : ADRENO_SUBMIT_E_INACTIVE;
#if ADRENO_PERF_FRAMES
    if (!result) {
#if ADRENO_PERF_GPU
        const uint64_t completedAt = mach_absolute_time();
        __atomic_store_n(&gAdrenoIBCompletedAt, completedAt, __ATOMIC_RELAXED);
        // P5: the monitor's poll ticks (x2 of the IB HVC): GPU execution from the doorbell to retirement.
        const uint64_t gpuTicks = fIB->reply.ticks;
        __atomic_fetch_add(&gAdrenoIBGpuTicks, gpuTicks, __ATOMIC_RELAXED);
        uint64_t seen = __atomic_load_n(&gAdrenoIBGpuTicksMax, __ATOMIC_RELAXED);
        while (gpuTicks > seen && !__atomic_compare_exchange_n(&gAdrenoIBGpuTicksMax, &seen, gpuTicks, false,
                                                               __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
        __atomic_fetch_add(&gAdrenoIBSubmitTicks, completedAt - submitStart, __ATOMIC_RELAXED);
        __atomic_fetch_add(&gAdrenoIBLockTicks, submitStart - lockStart, __ATOMIC_RELAXED);
#else
        __atomic_store_n(&gAdrenoIBCompletedAt, mach_absolute_time(), __ATOMIC_RELAXED);
#endif
        __atomic_fetch_add(&gAdrenoIBCompleted, 1, __ATOMIC_RELEASE);
    }
#endif
    if (!result && ++fSubmittedIBs == 1)
        ALOG("GPU IB completed: %u dwords, %u relocations, marker 0x%llx, ticks %llu, wptr %llu",
             fIB->command.ib_dwords, fIB->command.reloc_count, fIB->reply.marker, fIB->reply.ticks, fIB->reply.wptr);
    if (result && fIB && result != ADRENO_SUBMIT_E_INACTIVE)
        ALOG("GPU IB refused %d at 0x%x (monitor status %lld, ib state 0x%x)", result, fIB->error_at,
             fIB->reply.status, fIB->state);
    IOLockUnlock(fSubmitLock);
    return result;
}

#endif
