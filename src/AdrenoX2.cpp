/* SPDX-License-Identifier: MIT */
// AdrenoX2.cpp - Adreno X2-90 IOGPU driver. See AdrenoX2.h.
#include "AdrenoX2.h"
#include "AdrenoEarlyStop.h"
#include "../../../../compat/adreno-guest.h"
#include "../../../../compat/early-stop.h"
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <kern/task.h>

extern "C" task_t kernel_task;   // com.apple.kpi.mach

#define ALOG(fmt, ...) IOLog("AdrenoX2: " fmt "\n", ##__VA_ARGS__)

#if ADRENO_PERF_FRAMES
#include <kern/clock.h>
// Frame pacing (AdrenoDisplayPacing.h, FAILURES-AND-DESIGN.md 2026-10-20 F8): draw submissions complete inside
// submitIB, so its completion time is what the display can compare a copy with. The time is stored before the
// count is published (release); the display reads the count first (acquire). One clock read per IB.
extern "C" uint64_t gAdrenoIBCompleted, gAdrenoIBCompletedAt;
uint64_t gAdrenoIBCompleted, gAdrenoIBCompletedAt;
#endif
#if ADRENO_PERF_GPU
#if !ADRENO_PERF_FRAMES
#error "ADRENO_PERF_GPU needs ADRENO_PERF_FRAMES (the clock and the IB counters)"
#endif
// The perf gpu line (AdrenoX2.h): the monitor's CP poll ticks of every completed IB (its GPU execution, host counter
// units) and their maximum since the display last took it (exchange), and the mach time spent inside submitIB with
// fSubmitLock held and waiting for it. Relaxed totals: the display takes window deltas.
uint64_t gAdrenoIBGpuTicks, gAdrenoIBGpuTicksMax, gAdrenoIBSubmitTicks, gAdrenoIBLockTicks;
#endif
#ifndef ADRENO_LATCH
#define ADRENO_LATCH 0
#endif
#if ADRENO_LATCH
// --present-latch's copy gate (agents/input-latency FAILURE-MODES.md G1-G3): GPU submissions (IB, FILL32) that entered
// the kext and have not returned, waiting for fSubmitLock or running. AdrenoDisplay defers a fresh layer-0 copy while it
// is not 0. b12v11: WindowServer's swap can arrive before its frame's IB completes (ibaft 17-87 a window).
extern "C" uint64_t gAdrenoGPUBusy;
uint64_t gAdrenoGPUBusy;
struct AdrenoGPUBusy {
    AdrenoGPUBusy() { __atomic_fetch_add(&gAdrenoGPUBusy, 1, __ATOMIC_ACQ_REL); }
    ~AdrenoGPUBusy() { __atomic_fetch_sub(&gAdrenoGPUBusy, 1, __ATOMIC_ACQ_REL); }
};
#endif
#if ADRENO_PRESENT_SYNC
#include "AdrenoDisplaySync.h"
#include <kern/clock.h>
// --present-sync (agents/present-sync FAILURE-MODES.md K1, K5, K7): the IOSurfaces a GPU submission (IB, FILL32) writes,
// marked from before the fSubmitLock wait to the return, when the IB has retired and its FROM_DEVICE maintenance is
// done. AdrenoDisplay asks AdrenoPresentSyncQuery before it copies a layer-0 surface. The table's spin lock is held for
// a slot scan only and never together with fSubmitLock. Zero-initialized (__bss); the lock is made by configureDevice
// before any queue exists.
extern "C" void *AdrenoResourceIOSurface(const void *) __asm__("__ZNK13IOGPUResource12getIOSurfaceEv");
extern "C" uint32_t AdrenoIOSurfaceID(const void *) __asm__("__ZNK9IOSurface12getSurfaceIDEv");
static struct adreno_sync_table gAdrenoSyncTable;
static IOSimpleLock *gAdrenoSyncLock;
extern "C" uint32_t AdrenoPresentSyncQuery(uint32_t surfaceID, uint64_t *oldest)
{
    IOSimpleLock *lock = __atomic_load_n(&gAdrenoSyncLock, __ATOMIC_ACQUIRE);
    if (oldest) *oldest = 0;
    if (!lock) return 0;
    IOSimpleLockLock(lock);
    const uint32_t writers = adreno_sync_query(&gAdrenoSyncTable, surfaceID, oldest);
    IOSimpleLockUnlock(lock);
    return writers;
}
extern "C" void AdrenoPresentSyncCounters(uint64_t *marks, uint64_t *overflows, uint64_t *unmatched)
{
    IOSimpleLock *lock = __atomic_load_n(&gAdrenoSyncLock, __ATOMIC_ACQUIRE);
    *marks = *overflows = *unmatched = 0;
    if (!lock) return;
    IOSimpleLockLock(lock);
    *marks = gAdrenoSyncTable.marks; *overflows = gAdrenoSyncTable.overflows; *unmatched = gAdrenoSyncTable.unmatched;
    IOSimpleLockUnlock(lock);
}
// RAII: the constructor marks the distinct IOSurfaces of the list's Write entries; the destructor unmarks exactly them on
// every return path (K2). One clock read, and only when something is marked.
struct AdrenoSurfaceWrites {
    uint32_t ids[ADRENO_SYNC_IDS_PER_SUBMIT];
    unsigned n;
    bool wild;
    explicit AdrenoSurfaceWrites(AdrenoSegmentList *list) : n(0), wild(false)
    {
        IOSimpleLock *lock = __atomic_load_n(&gAdrenoSyncLock, __ATOMIC_ACQUIRE);
        if (!lock || !list) return;
        const struct adreno_resource_list *r = list->presentSyncResources();
        for (uint64_t i = 0; r->entries && i < r->count; i++) {
            const struct adreno_resource_entry *e = &r->entries[i];
            if (!(e->usage & 2u) || !e->resource) continue;               // Write usage (adreno-resource-list.c)
            const void *surface = AdrenoResourceIOSurface(e->resource);
            const uint32_t id = surface ? AdrenoIOSurfaceID(surface) : 0;
            if (!id) continue;
            unsigned k = 0;
            while (k < n && ids[k] != id) k++;
            if (k < n) continue;
            if (n < ADRENO_SYNC_IDS_PER_SUBMIT) ids[n++] = id; else wild = true;
        }
        if (!n && !wild) return;
        const uint64_t now = mach_absolute_time();
        IOSimpleLockLock(lock);
        for (unsigned k = 0; k < n; k++) adreno_sync_mark(&gAdrenoSyncTable, ids[k], now);
        if (wild) adreno_sync_mark(&gAdrenoSyncTable, ADRENO_SYNC_WILDCARD, now);
        IOSimpleLockUnlock(lock);
    }
    ~AdrenoSurfaceWrites()
    {
        if (!n && !wild) return;
        IOSimpleLock *lock = __atomic_load_n(&gAdrenoSyncLock, __ATOMIC_ACQUIRE);
        IOSimpleLockLock(lock);
        for (unsigned k = 0; k < n; k++) adreno_sync_unmark(&gAdrenoSyncTable, ids[k]);
        if (wild) adreno_sync_unmark(&gAdrenoSyncTable, ADRENO_SYNC_WILDCARD);
        IOSimpleLockUnlock(lock);
    }
#if ADRENO_PIPELINE
    // agents/gpu-pipeline D1: the IB stays in flight after the submission returns; its marks go to the flight's record
    // and are released at its retirement (AdrenoSyncUnmarkIDs), not here.
    void handTo(uint32_t *out, unsigned *count, bool *outWild)
    {
        for (unsigned k = 0; k < n; k++) out[k] = ids[k];
        *count = n; *outWild = wild;
        n = 0; wild = false;
    }
#endif
};
#if ADRENO_PIPELINE
static void AdrenoSyncUnmarkIDs(const uint32_t *ids, unsigned n, bool wild)
{
    if (!n && !wild) return;
    IOSimpleLock *lock = __atomic_load_n(&gAdrenoSyncLock, __ATOMIC_ACQUIRE);
    if (!lock) return;
    IOSimpleLockLock(lock);
    for (unsigned k = 0; k < n; k++) adreno_sync_unmark(&gAdrenoSyncTable, ids[k]);
    if (wild) adreno_sync_unmark(&gAdrenoSyncTable, ADRENO_SYNC_WILDCARD);
    IOSimpleLockUnlock(lock);
}
#endif
#endif

#ifndef ADRENO_IB_ASYNC
#define ADRENO_IB_ASYNC 0
#endif
#if ADRENO_IB_ASYNC
#include "../../../../compat/adreno-ib-async.h"
// Asynchronous IB completion (compat/adreno-ib-async.h, agents/async-ib/FAILURE-MODES.md): a monitor built with
// SNAP_ADRENO_IB_ASYNC answers IB_POLL(0) with IDLE; then ibRing is IB_START and IB_POLLs with IODelay backoff, and the
// GPU's execution no longer holds the monitor's service token. Any other monitor keeps the synchronous ADRENO_GUEST_IB.
// Set once by configureGuestIB before the first IB; the counters change under fSubmitLock. Zero-initialized (__bss):
// no initialized data ahead of kmod_info.
static uint32_t gAdrenoIBAsync;
static struct adreno_ib_async_stats gAdrenoIBAsyncStats;
static void ibAsyncHVC(void *, uint64_t regs[4])
{
    register uint64_t x0 __asm__("x0") = regs[0];
    register uint64_t x1 __asm__("x1") = regs[1];
    register uint64_t x2 __asm__("x2") = regs[2];
    register uint64_t x3 __asm__("x3") = regs[3];
    // As ibRing: the arena stores are complete before the monitor cleans the package to PoC and rings it.
    __asm__ volatile("dsb ish\n\thvc %4" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                     : "i"(ADRENO_GUEST_HVC) : "memory", "cc");
    regs[0] = x0; regs[1] = x1; regs[2] = x2; regs[3] = x3;
}
static void ibAsyncDelay(void *, uint32_t us) { IODelay(us); }
#endif

#if ADRENO_GPU_RECOVER
#if !ADRENO_IB_ASYNC
#error "ADRENO_GPU_RECOVER needs ADRENO_IB_ASYNC (the monitor's RESET ends an asynchronous flight)"
#endif
#include <kern/clock.h>
#include "../../../../compat/adreno-recover-guest.h"
// agents/gpu-recovery: QUERY's answer (configureGuestIB, before the probe IB) and the un-latch state; changed under
// fSubmitLock afterwards. Zero-initialized (__bss): no initialized data ahead of kmod_info.
static struct adreno_recover_guest gAdrenoRecover;
static const struct adreno_recover_ops kAdrenoRecoverOps = {nullptr, ibAsyncHVC};
#endif

#if ADRENO_SOFTGPU
// IOGPUDevice::getGPUTask: the task a command buffer's device belongs to, whose SoftGPU context its stream runs in.
extern "C" IOGPUTask *AdrenoDeviceGPUTask(const IOGPUDevice *) __asm__("__ZNK11IOGPUDevice10getGPUTaskEv");
#endif
#if ADRENO_PIPELINE
#if !ADRENO_IB_ASYNC || !ADRENO_PRESENT_SYNC
#error "ADRENO_PIPELINE needs ADRENO_IB_ASYNC (IB_START/IB_POLL) and ADRENO_PRESENT_SYNC (the marks the display waits on)"
#endif
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include "AdrenoDisplayPipe.h"
// --pipeline (agents/gpu-pipeline FAILURE-MODES.md, findings 1-6): IOGPUFamily 26A428's exported event machine and
// completion kick, and two private offsets checked against the KDK's arm64e.kernel slice: the queue's current fence
// (queue+0x518, set at submit_command_buffer 0x4c710) and the fence's completion status (fence+0xc8, written at
// 0x4c834, read by IOGPUBlockFence::notifyClient at 0x2367c).
#include "AdrenoIOGPUStamps.h"   // the event machine's calls, checked by tools/test-adreno-iogpu-stamps.c on the KDK's code
extern "C" void AdrenoGPUSignalStampsUpdated(IOGPU *) __asm__("__ZN5IOGPU19signalStampsUpdatedEv");
extern "C" IOGPUTask *AdrenoDeviceGPUTask(const IOGPUDevice *) __asm__("__ZNK11IOGPUDevice10getGPUTaskEv");
static constexpr size_t kIOGPUQueueCurrentFenceOffset = 0x518;
static constexpr size_t kIOGPUFenceStatusOffset = 0xc8;
#if !ADRENO_GPU_RECOVER
static constexpr uint32_t kIOGPUSubmissionErrorKext = 0xa;   // what processSegmentKernelCommand sets today
#endif
#define ADRENO_PIPE_RECORDS 8u
// One pipelined IB's kernel side: its list (retained while in flight), the command buffer's fence (retained, for
// an asynchronous failure's status), the present-sync marks handed over at the submission's return, its stamp.
struct AdrenoAcceleratorX2::AdrenoFlightRecord {
    AdrenoSegmentList *list;
    IOGPUCommandQueue *queue;     // during the submission only: its event takes the stamp
    OSObject *fence;
    uint32_t ids[ADRENO_SYNC_IDS_PER_SUBMIT];
    unsigned nids;
    bool wild;
    uint32_t stamp, busy;
    int result;
};
// The display's perf pipe line reads the engine's counters (AdrenoDisplayPipe.h order; relaxed, window deltas there).
static const struct adreno_pipe_stats *gAdrenoPipeStats;
static uint32_t gAdrenoPipeState;               // bit 0 on, bits 8-15 adreno_pipeline
extern "C" uint32_t AdrenoPipelineCounters(uint64_t *v)
{
    const struct adreno_pipe_stats *s = __atomic_load_n(&gAdrenoPipeStats, __ATOMIC_ACQUIRE);
    for (unsigned i = 0; i < ADRENO_PIPE_COUNTERS; i++) v[i] = 0;
    if (s) {
        v[ADRENO_PIPE_C_FLIGHTS] = __atomic_load_n(&s->flights, __ATOMIC_RELAXED);
        v[ADRENO_PIPE_C_OVERLAPPED] = __atomic_load_n(&s->overlapped, __ATOMIC_RELAXED);
        v[ADRENO_PIPE_C_IMMEDIATE] = __atomic_load_n(&s->immediate, __ATOMIC_RELAXED);
        v[ADRENO_PIPE_C_SYNC] = __atomic_load_n(&s->sync_ibs, __ATOMIC_RELAXED);
        v[ADRENO_PIPE_C_FOLDED] = __atomic_load_n(&s->folded, __ATOMIC_RELAXED);
        v[ADRENO_PIPE_C_DRAINS] = __atomic_load_n(&s->drains, __ATOMIC_RELAXED);
        v[ADRENO_PIPE_C_TIMER] = __atomic_load_n(&s->timer_finishes, __ATOMIC_RELAXED);
        v[ADRENO_PIPE_C_POLLS] = __atomic_load_n(&s->polls, __ATOMIC_RELAXED);
        v[ADRENO_PIPE_C_SPIN_US] = __atomic_load_n(&s->spin_us, __ATOMIC_RELAXED);
        v[ADRENO_PIPE_C_GAP_US] = __atomic_load_n(&s->gap_us, __ATOMIC_RELAXED);
        v[ADRENO_PIPE_C_GAP_N] = __atomic_load_n(&s->gap_n, __ATOMIC_RELAXED);
        v[ADRENO_PIPE_C_MAX_US] = __atomic_load_n(&s->flight_max_us, __ATOMIC_RELAXED);
        v[ADRENO_PIPE_C_FAILURES] = __atomic_load_n(&s->failures, __ATOMIC_RELAXED);
    }
    return __atomic_load_n(&gAdrenoPipeState, __ATOMIC_RELAXED);
}
#if ADRENO_PRESENT_RELATCH
// agents/present-pacing (FAILURE-MODES.md UR1 UR2, EP1-EP3, OP1). AdrenoDisplay sets gAdrenoPresentUrgent while a
// relatch waits for a mark: the completion timer then polls every adreno_pipeline_urgent_us. gAdrenoPipeEarly holds the
// early-release switches (adreno_pipe_early, adreno_pipe_assist_us) and totals, changed under fSubmitLock and read
// relaxed by the perf relatch line. Zero-initialized (__bss): no initialized data ahead of kmod_info.
extern "C" uint32_t gAdrenoPresentUrgent;
uint32_t gAdrenoPresentUrgent;
static struct adreno_pipe_early gAdrenoPipeEarly;
static uint64_t gAdrenoPipeUrgentTicks;
extern "C" uint32_t AdrenoPipelineEarlyCounters(uint64_t *v)
{
    v[0] = __atomic_load_n(&gAdrenoPipeEarly.pre, __ATOMIC_RELAXED);
    v[1] = __atomic_load_n(&gAdrenoPipeEarly.assisted, __ATOMIC_RELAXED);
    v[2] = __atomic_load_n(&gAdrenoPipeEarly.assist_polls, __ATOMIC_RELAXED);
    v[3] = __atomic_load_n(&gAdrenoPipeEarly.unmarks, __ATOMIC_RELAXED);
    return (__atomic_load_n(&gAdrenoPipeEarly.on, __ATOMIC_RELAXED) ? 1u : 0u) |
           (__atomic_load_n(&gAdrenoPipeUrgentTicks, __ATOMIC_RELAXED) ? 2u : 0u);
}
#endif
#endif

OSDefineMetaClassAndStructors(AdrenoEventMachine, IOGPUEventMachine)
OSDefineMetaClassAndStructors(AdrenoSysMemory, IOGPUSysMemory)
OSDefineMetaClassAndStructors(AdrenoResource, IOGPUResource)
OSDefineMetaClassAndStructors(AdrenoMemoryMap, IOGPUMemoryMap)
OSDefineMetaClassAndStructors(AdrenoTask, IOGPUTask)
OSDefineMetaClassAndStructors(AdrenoSegmentList, IOGPUSegmentResourceList)
OSDefineMetaClassAndStructors(AdrenoCommandQueue, IOGPUCommandQueue)
OSDefineMetaClassAndStructors(AdrenoAcceleratorX2, IOGPU)

// The rest of this translation unit, in its original order: the parts keep each file under 1,000 lines and
// are never compiled on their own.
#include "AdrenoX2/objects.cpp"
#include "AdrenoX2/device.cpp"
#include "AdrenoX2/submission.cpp"
#include "AdrenoX2/pipeline.cpp"
#include "AdrenoX2/lifecycle.cpp"
#include "AdrenoX2/softgpu.cpp"
