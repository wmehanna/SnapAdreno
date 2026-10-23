// SPDX-License-Identifier: MIT
// The IB pipeline's kext side (ADRENO_PIPELINE): the second package slot, flight records, the IOGPU stamp and
// retirement hooks, the completion timer, start and stop.
// Part of AdrenoX2.cpp's translation unit: included from it in order, never compiled on its own.

#if ADRENO_PIPELINE
// ---------------------------------------------------------------------------------------------------
// The IB pipeline's kext side (agents/gpu-pipeline FAILURE-MODES.md; the engine is compat/adreno-ib-pipeline.h).

// Q2: the second package slot, right after the first in the doubled arena, with its own zero page cleaned to PoC.
bool AdrenoAcceleratorX2::pipelineBindSlot1()
{
    if (fIB2 || !fIB || !fIBArena || !fIBTables || fIBGPULength < 2 * ADRENO_PIPE_SLOT_BYTES) return false;
    fIB2 = static_cast<struct adreno_ib_work *>(IOMalloc(sizeof(*fIB2)));
    fIBRelocs2 = static_cast<struct adreno_ib_reloc *>(IOMalloc(ADRENO_IB_MAX_RELOCS * sizeof(*fIBRelocs2)));
    fIBOrder2 = static_cast<uint16_t *>(IOMalloc(ADRENO_IB_MAX_RELOCS * sizeof(*fIBOrder2)));
    fIBUsed2 = static_cast<uint8_t *>(IOMalloc(ADRENO_IB_MAX_RELOCS * sizeof(*fIBUsed2)));
    fIBTargets2 = static_cast<uint64_t *>(IOMalloc(ADRENO_IB_TARGET_WORDS * sizeof(*fIBTargets2)));
    if (!fIB2 || !fIBRelocs2 || !fIBOrder2 || !fIBUsed2 || !fIBTargets2) { pipelineFreeSlot1(); return false; }
    uint8_t *arena = static_cast<uint8_t *>(fIBArena->getBytesNoCopy()) + ADRENO_PIPE_SLOT_BYTES;
    if (adreno_ib_work_init(fIB2, fIBTables, arena, fIBGPUAddress + ADRENO_PIPE_SLOT_BYTES, ADRENO_PIPE_SLOT_BYTES,
                            fIBRelocs2, fIBOrder2, fIBUsed2, ADRENO_IB_MAX_RELOCS, fIBTargets2) ||
        guestCacheRange(fIBGPUAddress + ADRENO_PIPE_SLOT_BYTES, ADRENO_IB_ZERO_BYTES, ADRENO_CACHE_TO_DEVICE)) {
        pipelineFreeSlot1();
        return false;
    }
    return true;
}
void AdrenoAcceleratorX2::pipelineFreeSlot1()
{
    if (fIB2) IOFree(fIB2, sizeof(*fIB2));
    if (fIBRelocs2) IOFree(fIBRelocs2, ADRENO_IB_MAX_RELOCS * sizeof(*fIBRelocs2));
    if (fIBOrder2) IOFree(fIBOrder2, ADRENO_IB_MAX_RELOCS * sizeof(*fIBOrder2));
    if (fIBUsed2) IOFree(fIBUsed2, ADRENO_IB_MAX_RELOCS * sizeof(*fIBUsed2));
    if (fIBTargets2) IOFree(fIBTargets2, ADRENO_IB_TARGET_WORDS * sizeof(*fIBTargets2));
    fIB2 = nullptr; fIBRelocs2 = nullptr; fIBOrder2 = nullptr; fIBUsed2 = nullptr; fIBTargets2 = nullptr;
}

uint64_t AdrenoAcceleratorX2::pipeNow(void *)
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000u;
}
// C1: a flight started: the next stamp value, set into this queue's event before the submission returns, so the
// command buffer's fence (and any later signalSharedEvent) completes only when the retirement writes it.
uint32_t AdrenoAcceleratorX2::pipeStamp(void *c, void *owner)
{
    auto *self = static_cast<AdrenoAcceleratorX2 *>(c);
    auto *rec = static_cast<AdrenoFlightRecord *>(owner);
    const int index = (int)self->fPipeStampIndex;
    AdrenoEMIncrementStamp(self->fPipeEvents, index);
    const uint32_t stamp = AdrenoEMCurrentStamp(self->fPipeEvents, index);
    if (rec->queue) AdrenoEMSetEventStamp(self->fPipeEvents, index, rec->queue->getPreMappingBarrierEvent());
    rec->stamp = stamp;
    return stamp;
}
// C1 C3 C4: a flight finished, FROM_DEVICE done (under fSubmitLock, in retirement order). Its failure status reaches the
// command buffer's fence, the fail-fast latch takes UNCERTAIN, and the stamp word is written last; signalling IOGPU,
// unmarking and releasing happen after the unlock (pipelinePost).
void AdrenoAcceleratorX2::pipeRetired(void *c, void *owner, int result, const struct adreno_ib_reply *final,
                                      uint32_t stamp)
{
    auto *self = static_cast<AdrenoAcceleratorX2 *>(c);
    auto *rec = static_cast<AdrenoFlightRecord *>(owner);
    rec->result = result;
    if (result == ADRENO_SUBMIT_E_UNCERTAIN || result == ADRENO_LIST_E_VISIBILITY) self->fStampCleanupFailed = true;
#if ADRENO_GPU_RECOVER
    const uint32_t status = self->recoverStatus(result);   // R26: 3 / 0xb for a reset, 2 uncertain, 5 victim
#else
    const uint32_t status = kIOGPUSubmissionErrorKext;
#endif
    if (result && rec->fence)
        __atomic_store_n(reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(rec->fence) + kIOGPUFenceStatusOffset),
                         status, __ATOMIC_RELAXED);
#if ADRENO_PERF_FRAMES
    if (!result) AdrenoNoteIBRetired(final->ticks);
#else
    (void)final;
#endif
#if ADRENO_PRESENT_RELATCH
    // EP1: its FROM_DEVICE ran (adreno_ib_submit_finish, before this callback): the display may copy what it wrote now,
    // not after the unlock (and not after a synchronous client's whole IB). fSubmitLock -> gAdrenoSyncLock only.
    if (gAdrenoPipeEarly.on && (rec->nids || rec->wild)) {
        AdrenoSyncUnmarkIDs(rec->ids, rec->nids, rec->wild);
        rec->nids = 0;
        rec->wild = false;
        gAdrenoPipeEarly.unmarks++;
    }
#endif
    uint32_t *word = static_cast<uint32_t *>(self->fStamps->getBytesNoCopy()) + self->fPipeStampIndex;
    __atomic_store_n(word, stamp, __ATOMIC_RELEASE);
    if (result) {
        const uint64_t n = __atomic_add_fetch(&self->fFailedCommands, 1, __ATOMIC_RELAXED);
        if (n <= 16 || !(n & 4095))
            ALOG("pipeline: IB failed after its submission returned: %d (monitor status %lld); command buffer marked 0x%x",
                 result, final->status, status);
    }
    if (self->fPostCount < sizeof self->fPost / sizeof self->fPost[0]) self->fPost[self->fPostCount++] = rec;
}
AdrenoAcceleratorX2::AdrenoFlightRecord *AdrenoAcceleratorX2::recordAlloc()
{
    for (unsigned i = 0; fRecords && i < ADRENO_PIPE_RECORDS; i++) {
        uint32_t idle = 0;
        if (__atomic_compare_exchange_n(&fRecords[i].busy, &idle, 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            AdrenoFlightRecord *r = &fRecords[i];
            r->list = nullptr; r->queue = nullptr; r->fence = nullptr; r->nids = 0; r->wild = false;
            r->stamp = 0; r->result = 0;
            return r;
        }
    }
    return nullptr;   // every record taken: this IB runs synchronously
}
void AdrenoAcceleratorX2::pipelineTakePost(AdrenoFlightRecord **out, unsigned *n)
{
    *n = fPostCount;
    for (unsigned i = 0; i < fPostCount; i++) out[i] = fPost[i];
    fPostCount = 0;
}
// After the unlock: unmark (D1), wake IOGPU's waiters and its completion machinery (C1), release the list and the
// fence (R2), free the record.
void AdrenoAcceleratorX2::pipelinePost(AdrenoFlightRecord *const *records, unsigned n)
{
    if (!n) return;
    for (unsigned i = 0; i < n; i++) {
        AdrenoFlightRecord *r = records[i];
        AdrenoSyncUnmarkIDs(r->ids, r->nids, r->wild);
        AdrenoEMSignalStamp(fPipeEvents, (int)fPipeStampIndex, r->stamp);
    }
    AdrenoGPUSignalStampsUpdated(this);
    for (unsigned i = 0; i < n; i++) {
        AdrenoFlightRecord *r = records[i];
        OSSafeReleaseNULL(r->fence);
        if (r->list) { r->list->release(); r->list = nullptr; }
        __atomic_store_n(&r->busy, 0u, __ATOMIC_RELEASE);
    }
}
void AdrenoAcceleratorX2::pipelineFinishLocked(AdrenoFlightRecord **post, unsigned *n)
{
    *n = 0;
    if (adreno_pipe_complete(&fPipe, &fPipeOps)) fPipe.stats.drains++;
    pipelineTakePost(post, n);
}
// C5: the completion timer, on the pipeline's own workloop.
void AdrenoAcceleratorX2::pipelineArm()
{
    if (!fPipeTimer || __atomic_exchange_n(&fPipeTimerArmed, 1u, __ATOMIC_ACQ_REL)) return;
#if ADRENO_PRESENT_RELATCH
    // UR1: while the display relatches, the urgent period (the flag is only read here; a stuck flag costs polls)
    const uint64_t urgent = __atomic_load_n(&gAdrenoPresentUrgent, __ATOMIC_RELAXED) ? gAdrenoPipeUrgentTicks : 0;
    fPipeTimer->setTimeout(kIOTimeOptionsWithLeeway, urgent ? urgent : fPipePollTicks, fPipeLeewayTicks);
#else
    fPipeTimer->setTimeout(kIOTimeOptionsWithLeeway, fPipePollTicks, fPipeLeewayTicks);
#endif
}
void AdrenoAcceleratorX2::pipelineTimerFired()
{
    __atomic_store_n(&fPipeTimerArmed, 0u, __ATOMIC_RELEASE);
    AdrenoFlightRecord *post[4];
    unsigned posts = 0;
    IOLockLock(fSubmitLock);
    const int active = fPipeOn ? adreno_pipe_poll(&fPipe, &fPipeOps) : 0;
    pipelineTakePost(post, &posts);
    IOLockUnlock(fSubmitLock);
    pipelinePost(post, posts);
    if (active) pipelineArm();
}
bool AdrenoAcceleratorX2::pipelineEligible(AdrenoSegmentList *list)
{
    if (fPipeMode >= 2) return true;
    IOGPUDevice *device = list ? list->pipelineDevice() : nullptr;
    AdrenoTask *task = OSDynamicCast(AdrenoTask, device ? AdrenoDeviceGPUTask(device) : nullptr);
    return task && task->pipelineClient();
}
// After IOGPU::start: the policy, the monitor, both slots, the IOGPU self-test (C2), records, the timer. Any failure
// leaves the engine off: every IB stays synchronous.
bool AdrenoAcceleratorX2::pipelineStart(IOGPUEventMachine *events)
{
    uint32_t mode = 1, pollUs = 500;
    PE_parse_boot_argn("adreno_pipeline", &mode, sizeof(mode));
    PE_parse_boot_argn("adreno_pipeline_poll_us", &pollUs, sizeof(pollUs));
    fPipeMode = mode > 2 ? 2 : mode;
    if (pollUs < 50) pollUs = 50;
    if (pollUs > 5000) pollUs = 5000;
    if (!fPipeMode) { ALOG("pipeline off (adreno_pipeline=0)"); return false; }
    if (!gAdrenoIBAsync || !fIB || !(fIB->state & ADRENO_IB_STATE_READY) || !fIB2 ||
        !(fIB2->state & ADRENO_IB_STATE_READY) || !events || !fStamps || fStampCleanupFailed) {
        ALOG("pipeline off: needs the monitor's IB_START/IB_POLL (%u) and both package slots (%d %d)", gAdrenoIBAsync,
             fIB ? (int)fIB->state : -1, fIB2 ? (int)fIB2->state : -1);
        return false;
    }
    // C2: the IOGPU event machine as the disassembly says (AdrenoIOGPUStamps.h; host-tested on the KDK's code): the
    // index exists (getNumStamps, read only), a local event at the synced stamp completes, then not after the increment,
    // then again once the stamp word holds the new value.
    fPipeStampIndex = ADRENO_NUM_STAMPS - 1;
    volatile uint32_t *word = static_cast<volatile uint32_t *>(fStamps->getBytesNoCopy()) + fPipeStampIndex;
    struct adreno_iogpu_selftest st;
    if (adreno_iogpu_selftest(events, word, (int)fPipeStampIndex, &st)) {
        ALOG("pipeline off: IOGPU stamp self-test failed (stamps %d, index %u, complete %d, pending %d, complete %d, "
             "stamp 0x%x -> 0x%x)", st.num_stamps, fPipeStampIndex, st.synced, st.pending, st.after, st.start, st.stamp);
        return false;
    }
    fRecords = static_cast<AdrenoFlightRecord *>(IOMalloc(ADRENO_PIPE_RECORDS * sizeof(*fRecords)));
    fPipeLoop = IOWorkLoop::workLoop();
    fPipeTimer = IOTimerEventSource::timerEventSource(kIOTimerEventSourceOptionsPriorityKernelHigh, this,
                                                      ^(IOTimerEventSource *) { pipelineTimerFired(); });
    if (!fRecords || !fPipeLoop || !fPipeTimer || fPipeLoop->addEventSource(fPipeTimer) != kIOReturnSuccess) {
        ALOG("pipeline off: no records, workloop or timer");
        if (fRecords) IOFree(fRecords, ADRENO_PIPE_RECORDS * sizeof(*fRecords));
        fRecords = nullptr;
        OSSafeReleaseNULL(fPipeTimer);
        OSSafeReleaseNULL(fPipeLoop);
        return false;
    }
    bzero(fRecords, ADRENO_PIPE_RECORDS * sizeof(*fRecords));
    nanoseconds_to_absolutetime((uint64_t)pollUs * 1000u, &fPipePollTicks);
    nanoseconds_to_absolutetime(50000u, &fPipeLeewayTicks);
#if ADRENO_PRESENT_RELATCH
    {   // UR1 EP1-EP3 OP3: adreno_pipeline_urgent_us (0 = off, else 50..poll), adreno_pipe_early, adreno_pipe_assist_us
        uint32_t urgentUs = 100, early = 1, assistUs = 250;
        PE_parse_boot_argn("adreno_pipeline_urgent_us", &urgentUs, sizeof(urgentUs));
        PE_parse_boot_argn("adreno_pipe_early", &early, sizeof(early));
        PE_parse_boot_argn("adreno_pipe_assist_us", &assistUs, sizeof(assistUs));
        if (urgentUs && urgentUs < 50) urgentUs = 50;
        if (urgentUs > pollUs) urgentUs = pollUs;
        if (assistUs > 5000) assistUs = 5000;
        uint64_t urgent = 0;
        if (urgentUs) nanoseconds_to_absolutetime((uint64_t)urgentUs * 1000u, &urgent);
        __atomic_store_n(&gAdrenoPipeUrgentTicks, urgent, __ATOMIC_RELAXED);
        gAdrenoPipeEarly.on = early != 0;
        gAdrenoPipeEarly.assist_us = assistUs;
        ALOG("pipeline: urgent completion poll %u us, early release %s, TO_DEVICE assist %u us",
             urgentUs, early ? "on" : "off", early ? assistUs : 0);
    }
#endif
    adreno_pipe_init(&fPipe, fIB, fIB2);
#if ADRENO_GPU_RECOVER
    const struct adreno_pipe_ops ops = {this, ibAsyncHVC, ibAsyncDelay, pipeNow, pipeStamp, pipeRetired,
                                        adreno_recover_guest_limit_us(&gAdrenoRecover)};
#else
    const struct adreno_pipe_ops ops = {this, ibAsyncHVC, ibAsyncDelay, pipeNow, pipeStamp, pipeRetired};
#endif
    fPipeOps = ops;
    fPipeEvents = events;
    __atomic_store_n(&gAdrenoPipeStats, &fPipe.stats, __ATOMIC_RELEASE);
    __atomic_store_n(&gAdrenoPipeState, 1u | fPipeMode << 8, __ATOMIC_RELAXED);
    __atomic_store_n(&fPipeOn, 1u, __ATOMIC_RELEASE);
    ALOG("pipeline on: %s IB submissions return once in flight; stamp %u (0x%x, %d stamps), completion poll %u us",
         fPipeMode >= 2 ? "every client's" : "WindowServer's", fPipeStampIndex, st.stamp, st.num_stamps, pollUs);
    return true;
}
// O2: finish the flight before the arena goes; the timer is cancelled and removed before its workloop is released.
void AdrenoAcceleratorX2::pipelineStop()
{
    if (fSubmitLock && fPipeOn) {
        AdrenoFlightRecord *post[4];
        unsigned posts = 0;
        IOLockLock(fSubmitLock);
        pipelineFinishLocked(post, &posts);
        __atomic_store_n(&fPipeOn, 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&gAdrenoPipeState, 0u, __ATOMIC_RELAXED);
        IOLockUnlock(fSubmitLock);
        pipelinePost(post, posts);
    }
    if (fPipeTimer) {
        fPipeTimer->cancelTimeout();
        if (fPipeLoop) fPipeLoop->removeEventSource(fPipeTimer);
    }
    OSSafeReleaseNULL(fPipeTimer);
    OSSafeReleaseNULL(fPipeLoop);
    if (fRecords && !fPipe.flight.active) {
        IOFree(fRecords, ADRENO_PIPE_RECORDS * sizeof(*fRecords));
        fRecords = nullptr;
    }
}
#endif
