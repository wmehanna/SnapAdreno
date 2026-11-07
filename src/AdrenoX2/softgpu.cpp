// SPDX-License-Identifier: MIT
// The SoftGPU transport (plans/p6-softgpu-integration.md W2, softgpu/SOFTGPU-PROTOCOL.md rev 6): bring-up of the QEMU
// device behind the ADT property snapintosh,softgpu-reg, the SOFTGPU_KCMD_STREAM (0x1010) kernel command forwarded to
// its descriptor ring, memory objects over the maps' physical runs, loss recovery, and the F1 panel self-test. Polled
// and synchronous, all under fSubmitLock: a stream completes before its command returns, so the trailing FENCE, the
// IOGPU stamps and the event machine keep today's path. Descriptors, packets and the ring are compat/softgpu-guest.c.
// Part of AdrenoX2.cpp's translation unit: included from it, never compiled on its own.
#if ADRENO_SOFTGPU

struct AdrenoSoftGPU {
    IOMemoryMap *reg;
    volatile uint8_t *base;
    IOBufferMemoryDescriptor *ringMem, *statusMem, *bootMem, *arenaMem;
    struct sgpu_desc *ringVA;
    volatile uint8_t *statusVA;
    uint8_t *bootVA, *arenaVA;
    struct sgpu_guest_ring ring;
    struct sgpu_guest_arena arena;
    uint64_t epoch;          // the device epoch the objects below belong to (atomic: read outside fSubmitLock)
    uint32_t nextMem;        // memory object ids are never reused within an epoch
    uint32_t cbSerial;
    uint32_t features;
    bool on;
    bool resetUnconfirmed;      // the last RESET_DEVICE never raised RESET_DONE: the device may still DMA
    // W5: the DONE interrupt. The handler runs on its own workloop with neither fSubmitLock nor the map locks.
    IOWorkLoop *loop;
    IOInterruptEventSource *irq;
    IOLock *wake;               // guards the sleep/wakeup pair on irqSeq
    volatile uint32_t irqSeq;   // bumped by the handler under `wake`
    bool irqOn;                 // the source is attached and enabled and the device's mask is set (re-set after a reset)
    uint64_t irqCount;          // handler runs that acked a cause (atomic)
    uint64_t irqWakes;          // waits ended by the interrupt
    uint64_t pollWakes;         // waits ended by the slice timeout with the work already done: the poll fallback at work
    uint64_t pollTimeouts;      // slices that ended with nothing done
};
// Memory objects whose backing the maps gave up (AdrenoMemoryMap::releaseFromGPUPageTable, free), under the map's lock:
// they cannot take fSubmitLock there (the IB path takes the map lock under it), so the ids wait here and the next
// stream or the self-test sends the DESTROY_MEMORY descriptors. Epoch-tagged: a reset already dropped older ones.
static IOSimpleLock *gSoftPendLock;
static struct { uint32_t id; uint64_t epoch; } gSoftPend[256];
static uint32_t gSoftPendCount;
static uint64_t gSoftEpoch;     // mirror of AdrenoSoftGPU::epoch for the map-side hook

extern "C" void AdrenoSoftGPUMapGone(uint32_t id, uint64_t epoch)
{
    IOSimpleLock *lock = __atomic_load_n(&gSoftPendLock, __ATOMIC_ACQUIRE);
    if (!lock || !id || epoch != __atomic_load_n(&gSoftEpoch, __ATOMIC_RELAXED)) return;
    IOSimpleLockLock(lock);
    if (gSoftPendCount < sizeof gSoftPend / sizeof *gSoftPend) {
        gSoftPend[gSoftPendCount].id = id;
        gSoftPend[gSoftPendCount].epoch = epoch;
        gSoftPendCount++;
    }   // full: the id leaks in the host until the next reset; the device quota (4096 contexts' worth) bounds it
    IOSimpleLockUnlock(lock);
}

#define SG_IRQ_CAUSES (SGPU_IRQ_DONE | SGPU_IRQ_FAULT | SGPU_IRQ_FATAL)   // RESET_DONE stays masked: sgResetDevice polls RAW
#define SG_IRQ_SLICE_MS 5u        // longest sleep without a re-check of the status page
#define SG_BUDGET_MS 2000u
#define SG_COMPILE_MS 20000u      // COMPILE_BUDGET_MS register default; W4 measures before this moves
static inline uint32_t sgRead(AdrenoSoftGPU *s, uint32_t off) { return *reinterpret_cast<volatile uint32_t *>(s->base + off); }
static inline void sgWrite(AdrenoSoftGPU *s, uint32_t off, uint32_t v) { *reinterpret_cast<volatile uint32_t *>(s->base + off) = v; }
static inline void sgWrite64(AdrenoSoftGPU *s, uint32_t off, uint64_t v)   // _LO then _HI
{
    sgWrite(s, off, (uint32_t)v);
    sgWrite(s, off + 4, (uint32_t)(v >> 32));
}
static inline uint64_t sgDone(AdrenoSoftGPU *s) { return __atomic_load_n(reinterpret_cast<volatile uint64_t *>(s->statusVA), __ATOMIC_ACQUIRE); }
static uint64_t sgNow()
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns;
}
// Sleep up to SG_IRQ_SLICE_MS on the device interrupt. `done` is re-checked under the lock the handler takes before it wakes
// us, so an interrupt that arrives between the check and the sleep is not lost.
template <typename F> static void sgIrqSleep(AdrenoSoftGPU *s, F done)
{
    uint64_t deadline = 0;
    IOLockLock(s->wake);
    if (!done()) {
        uint64_t slice = 0;
        nanoseconds_to_absolutetime((uint64_t)SG_IRQ_SLICE_MS * 1000000ull, &slice);
        deadline = mach_absolute_time() + slice;
        const wait_result_t w = IOLockSleepDeadline(s->wake, (void *)&s->irqSeq, deadline, THREAD_UNINT);
        if (w == THREAD_AWAKENED) s->irqWakes++;
        else if (done()) s->pollWakes++;                   // the interrupt never came but the work is done: the poll caught it
        else s->pollTimeouts++;
    }
    IOLockUnlock(s->wake);
}
// Poll `done()` until it holds or `ms` pass: a short busy wait first (a fill is microseconds), then 1 ms sleeps, or, with
// `s` given and its interrupt armed, sleeps on the interrupt in slices.
template <typename F> static bool sgPoll(uint32_t ms, F done, AdrenoSoftGPU *s = nullptr)
{
    const uint64_t limit = sgNow() + (uint64_t)ms * 1000000ull;
    for (unsigned spin = 0; !done(); spin++) {
        if (sgNow() > limit) return false;
        if (spin < 200) IODelay(10);
        else if (s && s->irqOn) sgIrqSleep(s, done);
        else IOSleep(1);
    }
    return true;
}
static bool sgContiguous(IOBufferMemoryDescriptor *md, uint64_t *pa)
{
    IOByteCount seg = 0;
    *pa = md->getPhysicalSegment(0, &seg, kIOMemoryMapperNone);
    return *pa && !(*pa & 0x3fff) && seg >= md->getLength();
}

// RESET_DEVICE and its RESET_DONE. False when the device never confirmed: it may still be reading its ring and writing
// guest pages, so nothing it was given may be freed or reused.
static bool sgResetDevice(AdrenoSoftGPU *s)
{
    sgWrite(s, SGPU_REG_IRQ_STATUS, 0xffffffffu);
    sgWrite(s, SGPU_REG_CONTROL, SGPU_CTL_RESET_DEVICE);
    if (!sgPoll(SG_BUDGET_MS, [&] { return (sgRead(s, SGPU_REG_IRQ_RAW) & SGPU_IRQ_RESET_DONE) != 0; })) return false;
    sgWrite(s, SGPU_REG_IRQ_STATUS, SGPU_IRQ_RESET_DONE);
    return true;
}

// Push one descriptor and ring RING_TAIL. 0, or SGPU_GUEST_BUSY. Caller holds fSubmitLock.
static int sgPush(AdrenoSoftGPU *s, const struct sgpu_desc *d, uint64_t *seqno)
{
    struct sgpu_desc withIrq = *d;
    if (s->irqOn) withIrq.flags |= SGPU_DESC_IRQ;          // the DONE cause is what the wait sleeps on
    const uint64_t n = sgpu_guest_ring_push(&s->ring, &withIrq);
    if (!n) return SGPU_GUEST_BUSY;
    __asm__ volatile("dsb st" ::: "memory");
    sgWrite(s, SGPU_REG_RING_TAIL, s->ring.produced);
    *seqno = n;
    return 0;
}
// Consume every completion up to done_seqno and free their arena blocks. The first failing status, else 0.
static int sgRetire(AdrenoSoftGPU *s, uint32_t *packetOffset)
{
    int first = 0;
    struct sgpu_completion c;
    const struct sgpu_completion *rec = reinterpret_cast<const struct sgpu_completion *>(const_cast<const uint8_t *>(s->statusVA + 0x1000));
    for (;;) {
        const int r = sgpu_guest_ring_retire_one(&s->ring, rec, sgDone(s), &c);
        if (r < 0) return SGPU_E_LOST;                         // a record that is not the expected one: reset (1.5)
        if (!r) break;
        const int32_t status = sgpu_guest_device_status(c.status);       // the device is not trusted to stay in range
        if (status && !first) { first = status; if (packetOffset) *packetOffset = c.packet_offset; }
    }
    sgpu_guest_arena_release(&s->arena, s->ring.done_seqno);
    return first;
}
// Wait until `seqno` is done and retire. SGPU_E_TIMEOUT after `ms`, SGPU_E_LOST when the device went FATAL.
static int sgWaitDone(AdrenoSoftGPU *s, uint64_t seqno, uint32_t ms, uint32_t *packetOffset)
{
    bool fatal = false;
    const bool ok = sgPoll(ms, [&] {
        if (sgDone(s) >= seqno) return true;
        if (sgRead(s, SGPU_REG_STATUS) & SGPU_ST_FATAL) { fatal = true; return true; }
        return false;
    }, s);
    if (fatal) return SGPU_E_LOST;
    if (!ok) return SGPU_E_TIMEOUT;
    return sgRetire(s, packetOffset);
}
// Emit one KERNEL descriptor whose stream `fill` writes into the arena; waits only when asked.
template <typename F> static int sgKernel(AdrenoSoftGPU *s, uint32_t bytes, uint16_t flags, uint32_t stampIndex,
                                          uint32_t stampValue, F fill)
{
    for (unsigned attempt = 0;; attempt++) {
        uint64_t off = 0, seq = 0;
        int r = sgpu_guest_arena_alloc(&s->arena, bytes, s->ring.next_seqno, &off);
        if (r == 0) {
            if (fill(s->arenaVA + off) < 0) { sgpu_guest_arena_release(&s->arena, s->ring.done_seqno); return SGPU_E_FORMAT; }
            struct sgpu_desc d;
            sgpu_guest_desc_kernel(&d, flags, off, bytes, stampIndex, stampValue);
            r = sgPush(s, &d, &seq);
            if (r == 0) return 0;
            // ring full: the block belongs to a seqno that will never exist; it is freed by the drain below
        }
        if (r == SGPU_E_QUOTA || attempt) return r == SGPU_GUEST_BUSY ? SGPU_E_QUOTA : r;
        const uint64_t last = s->ring.next_seqno - 1;      // drain what is queued, then retry once
        if (last) { r = sgWaitDone(s, last, SG_BUDGET_MS, nullptr); if (r) return r; }
    }
}
// Everything queued is done and retired: 0, a failing status, SGPU_E_TIMEOUT or SGPU_E_LOST.
static int sgDrain(AdrenoSoftGPU *s, uint32_t ms)
{
    const uint64_t last = s->ring.next_seqno - 1;
    return last > s->ring.done_seqno ? sgWaitDone(s, last, ms, nullptr) : sgRetire(s, nullptr);
}

// Send DESTROY_MEMORY for the maps that gave up their backing and wait for them. Under fSubmitLock. These descriptors
// belong to no stream: a refusal is logged and dropped, only a lost device (SGPU_E_LOST) is the caller's.
static int sgFlushPending(AdrenoSoftGPU *s)
{
    uint32_t ids[64];
    uint32_t n = 0;
    IOSimpleLock *lock = __atomic_load_n(&gSoftPendLock, __ATOMIC_ACQUIRE);
    if (!lock) return 0;
    for (;;) {
        n = 0;
        IOSimpleLockLock(lock);
        while (gSoftPendCount && n < 64) {
            gSoftPendCount--;
            if (gSoftPend[gSoftPendCount].epoch == s->epoch) ids[n++] = gSoftPend[gSoftPendCount].id;
        }
        const bool more = gSoftPendCount != 0;
        IOSimpleLockUnlock(lock);
        if (n) {
            int r = 0;
            for (uint32_t i = 0; i < n && !r; i++)
                r = sgKernel(s, sizeof(struct sgpu_destroy_memory), 0, SGPU_GUEST_NO_STAMP, 0,
                             [&](uint8_t *p) { return sgpu_guest_pkt_destroy_memory(p, sizeof(struct sgpu_destroy_memory), ids[i]); });
            if (!r) r = sgDrain(s, SG_BUDGET_MS);
            if (r) ALOG("SoftGPU: DESTROY_MEMORY of %u objects failed %d", n, r);
            if ((r = sgpu_guest_housekeeping_result(r))) return r;
        }
        if (!more) return 0;
    }
}

// CREATE_MEMORY for a map's runs, completed before it returns: the caller records the id only on 0. The queue holds
// nothing else here (sgFlushPending drained it), so a failing status is this object's. Under fSubmitLock.
static int sgCreateMemory(AdrenoSoftGPU *s, uint32_t id, uint32_t flags, uint64_t size, uint32_t head,
                          const struct sgpu_mem_run *runs, uint32_t nruns)
{
    const uint32_t bytes = (uint32_t)(sizeof(struct sgpu_create_memory) + (uint64_t)nruns * sizeof *runs);
    int r = sgKernel(s, bytes, 0, SGPU_GUEST_NO_STAMP, 0, [&](uint8_t *p) {
        return sgpu_guest_pkt_create_memory(p, bytes, id, flags, size, 0, head, runs, nruns); });
    return r ? r : sgDrain(s, SG_BUDGET_MS);
}

// Spec 1.9 steps 1 to 6 on a device in any state. Allocations are made once (first); a reset reruns the rest.
bool AdrenoAcceleratorX2::softgpuBringUp(bool first)
{
    AdrenoSoftGPU *s = fSoft;
    s->on = false;
    s->irqOn = false;           // until RING_ENABLE: bring-up waits poll
    // 1: MAGIC, VERSION major 1, RESET_DEVICE and its RESET_DONE
    if (sgRead(s, SGPU_REG_MAGIC) != SGPU_MAGIC || (sgRead(s, SGPU_REG_VERSION) >> 16) != 1) {
        ALOG("SoftGPU: bad magic 0x%x or version 0x%x", sgRead(s, SGPU_REG_MAGIC), sgRead(s, SGPU_REG_VERSION));
        return false;
    }
    s->resetUnconfirmed = !sgResetDevice(s);
    if (s->resetUnconfirmed) {
        ALOG("SoftGPU: RESET_DEVICE did not finish");
        return false;
    }
    // 2: negotiate (the core feature only in this wave), the protocol version
    s->features = sgRead(s, SGPU_REG_FEATURES0) & SGPU_F_CORE;
    sgWrite(s, SGPU_REG_DRV_FEATURES0, s->features);
    sgWrite(s, SGPU_REG_DRV_FEATURES1, 0);
    sgWrite(s, SGPU_REG_DRV_VERSION, SGPU_VERSION);
    // 3: wired contiguous 16 KiB pages, the registers
    uint64_t ringPA = 0, statusPA = 0, bootPA = 0, stampPA = 0;
    if (first) {
        const IOOptionBits opts = kIODirectionInOut | kIOMemoryPhysicallyContiguous;
        s->ringMem = IOBufferMemoryDescriptor::withOptions(opts, 0x4000, 0x4000);
        s->statusMem = IOBufferMemoryDescriptor::withOptions(opts, 0x4000, 0x4000);
        s->bootMem = IOBufferMemoryDescriptor::withOptions(opts, 0x4000, 0x4000);
        s->arenaMem = IOBufferMemoryDescriptor::withOptions(kIODirectionInOut, SGPU_GUEST_ARENA_BYTES, 0x4000);
        IOBufferMemoryDescriptor *all[4] = {s->ringMem, s->statusMem, s->bootMem, s->arenaMem};
        for (auto *md : all)
            if (!md || md->prepare(kIODirectionInOut) != kIOReturnSuccess) { ALOG("SoftGPU: page allocation failed"); return false; }
        s->ringVA = static_cast<struct sgpu_desc *>(s->ringMem->getBytesNoCopy());
        s->statusVA = static_cast<volatile uint8_t *>(s->statusMem->getBytesNoCopy());
        s->bootVA = static_cast<uint8_t *>(s->bootMem->getBytesNoCopy());
        s->arenaVA = static_cast<uint8_t *>(s->arenaMem->getBytesNoCopy());
        bzero(s->arenaVA, SGPU_GUEST_ARENA_BYTES);
    }
    if (!sgContiguous(s->ringMem, &ringPA) || !sgContiguous(s->statusMem, &statusPA) || !sgContiguous(s->bootMem, &bootPA)) {
        ALOG("SoftGPU: ring, status or bootstrap page is not contiguous and 16 KiB aligned");
        return false;
    }
    IOByteCount seg = 0;
    stampPA = fStamps->getPhysicalSegment(0, &seg, kIOMemoryMapperNone);
    if (!stampPA || seg < (ADRENO_NUM_STAMPS + 1u) * ADRENO_STAMP_BYTES) { ALOG("SoftGPU: stamp page not physical"); return false; }
    bzero((void *)s->statusVA, 0x4000);
    bzero(s->bootVA, 0x4000);
    sgpu_guest_ring_init(&s->ring, s->ringVA, SGPU_GUEST_RING_ENTRIES);
    sgpu_guest_arena_init(&s->arena, SGPU_GUEST_ARENA_BYTES);
    sgWrite64(s, SGPU_REG_RING_BASE_LO, ringPA);
    sgWrite(s, SGPU_REG_RING_ENTRIES, SGPU_GUEST_RING_ENTRIES);
    sgWrite64(s, SGPU_REG_STATUS_PAGE_LO, statusPA);
    sgWrite64(s, SGPU_REG_STAMP_BASE_LO, stampPA);
    sgWrite(s, SGPU_REG_STAMP_COUNT, ADRENO_NUM_STAMPS + 1u);
    // 4: RING_ENABLE (refused when the monitor lock is not what the device wants)
    sgWrite(s, SGPU_REG_CONTROL, SGPU_CTL_RING_ENABLE);
    if (!(sgRead(s, SGPU_REG_STATUS) & SGPU_ST_RING_ENABLED)) {
        ALOG("SoftGPU: RING_ENABLE refused (status 0x%x); is the device locked or monitorless?", sgRead(s, SGPU_REG_STATUS));
        return false;
    }
    // The device reset cleared the interrupt mask: DONE, FAULT and FATAL on again when the source is attached.
    if (s->irq) {
        sgWrite(s, SGPU_REG_IRQ_STATUS, SG_IRQ_CAUSES);
        sgWrite(s, SGPU_REG_IRQ_MASK_SET, SG_IRQ_CAUSES);
        s->irqOn = true;
    }
    // 5: the bootstrap descriptor, its stream in the bootstrap page read by GPA: CREATE_MEMORY id 1, the arena
    struct sgpu_mem_run runs[(SGPU_GUEST_ARENA_BYTES >> 14) + 1];
    uint32_t nruns = 0;
    for (uint64_t off = 0; off < SGPU_GUEST_ARENA_BYTES;) {
        IOByteCount span = 0;
        const uint64_t pa = s->arenaMem->getPhysicalSegment(off, &span, kIOMemoryMapperNone);
        if (!pa || !span || sgpu_guest_runs_add(runs, &nruns, sizeof runs / sizeof *runs, pa, (span + 0xfff) & ~0xfffull)) {
            ALOG("SoftGPU: arena runs failed at 0x%llx", off);
            return false;
        }
        off += span;
    }
    const int len = sgpu_guest_pkt_create_memory(s->bootVA, 0x4000, 1, SGPU_MEM_ARENA, SGPU_GUEST_ARENA_BYTES, 0, 0, runs, nruns);
    struct sgpu_desc d;
    uint64_t seq = 0;
    if (len < 0) return false;
    sgpu_guest_desc_kernel(&d, SGPU_DESC_BOOTSTRAP, bootPA, (uint32_t)len, SGPU_GUEST_NO_STAMP, 0);
    int r = sgPush(s, &d, &seq);
    if (!r) r = sgWaitDone(s, seq, SG_BUDGET_MS, nullptr);
    if (r) { ALOG("SoftGPU: arena registration failed %d", r); return false; }
    // CREATE_CONTEXT 1: one context for every task in this wave
    s->nextMem = 2;
    r = sgKernel(s, sizeof(struct sgpu_create_context), 0, SGPU_GUEST_NO_STAMP, 0,
                 [&](uint8_t *p) { return sgpu_guest_pkt_create_context(p, sizeof(struct sgpu_create_context), 1); });
    // 6: probe STAMP into slot 128 and read it back from the stamp page
    const uint32_t probe = 0x50f70000u | (uint32_t)(first ? 1 : 2);
    if (!r) r = sgKernel(s, sizeof(struct sgpu_stamp), SGPU_DESC_IRQ, SGPU_GUEST_NO_STAMP, 0, [&](uint8_t *p) {
        return sgpu_guest_pkt_stamp(p, sizeof(struct sgpu_stamp), SGPU_GUEST_PROBE_STAMP, probe); });
    if (!r) r = sgDrain(s, SG_BUDGET_MS);
    const volatile uint32_t *stamps = static_cast<const volatile uint32_t *>(fStamps->getBytesNoCopy());
    if (r || stamps[SGPU_GUEST_PROBE_STAMP] != probe) {
        ALOG("SoftGPU: probe stamp failed (status %d, stamp 0x%x, want 0x%x)", r, stamps[SGPU_GUEST_PROBE_STAMP], probe);
        return false;
    }
    const uint64_t epoch = *reinterpret_cast<volatile uint64_t *>(s->statusVA + 32);
    __atomic_store_n(&s->epoch, epoch, __ATOMIC_RELEASE);
    __atomic_store_n(&gSoftEpoch, epoch, __ATOMIC_RELEASE);
    setProperty("SoftGPUEpoch", epoch, 64);               // the plug-in reads it after a failure (1.8, 4.6)
    setProperty("SoftGPUProtocol", (uint64_t)SGPU_VERSION, 32);
    s->on = true;
    ALOG("SoftGPU: ring on, epoch %llu, probe stamp ok, completion by %s", epoch, s->irqOn ? "interrupt (poll as fallback)" : "poll");
    return true;
}

// The device lost every object (FATAL, a stale record, a stuck descriptor): reset, rebuild arena and context, publish
// the new epoch. Memory ids are forgotten by their epoch tag; the plug-in replays its objects (4.6). Under fSubmitLock.
void AdrenoAcceleratorX2::softgpuLost(int why)
{
    ALOG("SoftGPU: device lost (%d), resetting", why);
    IOSimpleLock *lock = __atomic_load_n(&gSoftPendLock, __ATOMIC_ACQUIRE);
    if (lock) { IOSimpleLockLock(lock); gSoftPendCount = 0; IOSimpleLockUnlock(lock); }
    if (!softgpuBringUp(false)) ALOG("SoftGPU: reset failed; streams are refused until the next boot");
}

// The DONE interrupt (INTID 144 behind the monitor's forwarder, provider interrupt 0). Runs on the SoftGPU workloop, never
// under fSubmitLock: it acks the causes it enabled (W1C) and wakes the waiter. RESET_DONE is not enabled; sgResetDevice polls it.
void AdrenoAcceleratorX2::softgpuIrq(OSObject *owner, IOInterruptEventSource *, int)
{
    AdrenoAcceleratorX2 *self = OSDynamicCast(AdrenoAcceleratorX2, owner);
    AdrenoSoftGPU *s = self ? __atomic_load_n(&self->fSoft, __ATOMIC_ACQUIRE) : nullptr;
    if (!s || !s->base || !s->wake) return;
    const uint32_t causes = sgRead(s, SGPU_REG_IRQ_STATUS) & SG_IRQ_CAUSES;
    if (!causes) return;
    sgWrite(s, SGPU_REG_IRQ_STATUS, causes);
    __atomic_add_fetch(&s->irqCount, 1, __ATOMIC_RELAXED);
    IOLockLock(s->wake);
    s->irqSeq++;
    IOLockUnlock(s->wake);
    IOLockWakeup(s->wake, (void *)&s->irqSeq, false);
}

void AdrenoAcceleratorX2::softgpuStart(IOService *provider)
{
    OSData *index = OSDynamicCast(OSData, provider->getProperty("snapintosh,softgpu-reg"));
    if (!index || index->getLength() != sizeof(uint32_t)) { ALOG("SoftGPU: no device in the ADT, PM4 path only"); return; }
    uint32_t reg = 0;
    memcpy(&reg, index->getBytesNoCopy(), sizeof reg);
    fSoft = static_cast<AdrenoSoftGPU *>(IOMalloc(sizeof(AdrenoSoftGPU)));
    if (!fSoft) return;
    bzero(fSoft, sizeof(AdrenoSoftGPU));
    IOMemoryMap *map = provider->mapDeviceMemoryWithIndex(reg, kIOMapInhibitCache);
    if (!map || map->getLength() < 0x1000) {
        ALOG("SoftGPU: cannot map device memory %u", reg);
        OSSafeReleaseNULL(map);
        softgpuStop();
        return;
    }
    fSoft->reg = map;
    fSoft->base = reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress());
    // The completion interrupt. Anything missing leaves the transport polled: nothing below needs it.
    uint32_t noIrq = 0;         // boot-arg softgpu_nirq=1 forces the poll, for A/B runs
    if (!(PE_parse_boot_argn("softgpu_nirq", &noIrq, sizeof noIrq) && noIrq)) {
        fSoft->loop = IOWorkLoop::workLoop();
        fSoft->wake = IOLockAlloc();
        if (fSoft->loop && fSoft->wake) {
            fSoft->irq = IOInterruptEventSource::interruptEventSource(this, &AdrenoAcceleratorX2::softgpuIrq, provider, 0);
            if (fSoft->irq && fSoft->loop->addEventSource(fSoft->irq) == kIOReturnSuccess) {
                fSoft->irq->enable();
            } else {
                ALOG("SoftGPU: no interrupt source on the provider, completion by poll");
                if (fSoft->irq) { fSoft->loop->removeEventSource(fSoft->irq); }
                OSSafeReleaseNULL(fSoft->irq);
            }
        }
    }
    if (!__atomic_load_n(&gSoftPendLock, __ATOMIC_ACQUIRE))
        __atomic_store_n(&gSoftPendLock, IOSimpleLockAlloc(), __ATOMIC_RELEASE);
    IOLockLock(fSubmitLock);
    const bool ok = softgpuBringUp(true);
    IOLockUnlock(fSubmitLock);
    if (!ok) { softgpuStop(); return; }
#if ADRENO_SOFTGPU_SELFTEST
    uint32_t w5 = 0;
    if (PE_parse_boot_argn("softgpu_w5check", &w5, sizeof w5) && w5) {
        IOLockLock(fSubmitLock);
        softgpuW5Check();
        IOLockUnlock(fSubmitLock);
    }
    uint32_t selftest = 0;
    if (PE_parse_boot_argn("softgpu_selftest", &selftest, sizeof selftest) && selftest) {
        IOLockLock(fSubmitLock);
        softgpuSelfTest();
        IOLockUnlock(fSubmitLock);
    }
#endif
}

void AdrenoAcceleratorX2::softgpuStop()
{
    AdrenoSoftGPU *s = fSoft;
    if (!s) return;
    if (s->irq) {                                          // quiet the line, then take the source off before anything is freed
        s->irqOn = false;
        if (s->base) sgWrite(s, SGPU_REG_IRQ_MASK_CLR, 0xffffffffu);
        s->irq->disable();
        s->loop->removeEventSource(s->irq);                // returns once a handler in flight has finished
        OSSafeReleaseNULL(s->irq);
    }
    fSoft = nullptr;                                       // streams see INACTIVE from here on
    const bool quiet = !s->base || sgResetDevice(s);       // stop(): no host state left behind, once the device says so
    OSSafeReleaseNULL(s->reg);
    if (quiet) {
        OSSafeReleaseNULL(s->ringMem);
        OSSafeReleaseNULL(s->statusMem);
        OSSafeReleaseNULL(s->bootMem);
        OSSafeReleaseNULL(s->arenaMem);
    } else {
        // A device that never confirmed the reset might still read the ring and write the status page and the arena,
        // and the guest would reuse those pages: they are leaked on purpose (4 x 16 KiB plus the arena) for this boot.
        ALOG("SoftGPU: reset unconfirmed at stop; the ring, status, bootstrap and arena pages are kept");
    }
    OSSafeReleaseNULL(s->loop);
    if (s->wake) IOLockFree(s->wake);
    IOFree(s, sizeof(AdrenoSoftGPU));
}

#if ADRENO_SOFTGPU_SELFTEST
#include <IOKit/IOPlatformExpert.h>
// F1: FILL_BUFFER over a memory object made from the boot console's first 64 rows (the panel). A fill writes one byte
// everywhere (sgpu_fill_buffer.value is the fill byte), so the rows end up as 0xc0 bytes (softgpu_selftest_byte), not the
// plan's 0xff3080c0 pixel. Debug builds only (ADRENO_SOFTGPU_SELFTEST): the device writes through guest physical memory.
void AdrenoAcceleratorX2::softgpuSelfTest()
{
    AdrenoSoftGPU *s = fSoft;
    // The panel is the platform expert's boot console, the framebuffer AdrenoDisplay maps (mapScanout). The target must
    // lie inside it: softgpu_selftest_pa=<hex> moves the start within the panel, it never leaves it.
    PE_Video video = {};
    IOPlatformExpert *pe = getPlatform();
    if (!pe || pe->getConsoleInfo(&video) != kIOReturnSuccess || !video.v_baseAddr || !video.v_rowBytes || !video.v_height) {
        ALOG("SoftGPU: self-test: no boot framebuffer in PE_Video");
        return;
    }
    const uint64_t panel = video.v_baseAddr + video.v_offset;
    if ((panel >> 48) == 0xffff) { ALOG("SoftGPU: self-test: the boot framebuffer is a virtual address"); return; }
    const uint64_t panelBytes = (uint64_t)video.v_rowBytes * video.v_height;
    uint64_t pa = panel;
    uint32_t row32 = (uint32_t)video.v_rowBytes;
    uint32_t fillByte = 0xc0;
    PE_parse_boot_argn("softgpu_selftest_byte", &fillByte, sizeof fillByte);
    PE_parse_boot_argn("softgpu_selftest_row", &row32, sizeof row32);
    PE_parse_boot_argn("softgpu_selftest_pa", &pa, sizeof pa);
    const uint64_t rowBytes = row32;
    const uint64_t bytes = rowBytes * 64;
    if (!rowBytes || !sgpu_guest_range_inside(pa, bytes, panel, panelBytes)) {
        ALOG("SoftGPU: self-test: 0x%llx + 0x%llx is outside the panel 0x%llx + 0x%llx", pa, bytes, panel, panelBytes);
        return;
    }
    struct sgpu_mem_run run = {pa & ~0xfffull, ((pa & 0xfff) + bytes + 0xfff) & ~0xfffull};
    const uint32_t id = s->nextMem++;
    int r = sgFlushPending(s);
    if (!r) r = sgCreateMemory(s, id, 0, bytes, (uint32_t)(pa & 0xfff), &run, 1);
    const bool created = !r;
    struct sgpu_guest_bt_in in = {id, ADRENO_USAGE_WRITE, 0, 0, bytes, 0};
    uint64_t off = 0, seq = 0;
    if (!r) r = sgpu_guest_arena_alloc(&s->arena, sizeof(struct sgpu_fill_buffer) + sizeof(struct sgpu_bt_entry), s->ring.next_seqno, &off);
    if (!r) {
        sgpu_guest_pkt_fill_buffer(s->arenaVA + off, sizeof(struct sgpu_fill_buffer), 0, 0, bytes, fillByte);
        sgpu_guest_bt_build(reinterpret_cast<struct sgpu_bt_entry *>(s->arenaVA + off + sizeof(struct sgpu_fill_buffer)), &in, 1);
        struct sgpu_desc d;
        sgpu_guest_desc_submit(&d, 1, ++s->cbSerial, SGPU_DESC_LAST, off, sizeof(struct sgpu_fill_buffer),
                               off + sizeof(struct sgpu_fill_buffer), 1, SGPU_GUEST_NO_STAMP, 0, 0, 0);
        r = sgPush(s, &d, &seq);
    }
    if (!r) r = sgDrain(s, SG_BUDGET_MS + SG_COMPILE_MS + 3000);
    ALOG("SoftGPU: self-test %s: %llu bytes at 0x%llx filled with 0x%02x (status %d)", r ? "FAILED" : "ok", bytes, pa,
         fillByte & 0xff, r);
    // The object aliases the panel's pages: destroy it whatever happened (a device that is gone forgot it already).
    if (created && r != SGPU_E_LOST && r != SGPU_E_TIMEOUT) {
        const int d = sgKernel(s, sizeof(struct sgpu_destroy_memory), 0, SGPU_GUEST_NO_STAMP, 0,
                               [&](uint8_t *p) { return sgpu_guest_pkt_destroy_memory(p, sizeof(struct sgpu_destroy_memory), id); });
        if (d || (r = sgDrain(s, SG_BUDGET_MS))) ALOG("SoftGPU: self-test: DESTROY_MEMORY %u failed %d", id, d ? d : r);
    }
}
#endif

#if ADRENO_SOFTGPU_SELFTEST
// W5 pass checks (plans/p6-softgpu-integration.md W5, spec 1.3): the device must refuse memory the monitor withholds.
// softgpu_w5check=1 with softgpu_w5_monitor_pa=<hex> (a page of the monitor's work area) and softgpu_w5_trust_pa=<hex> (the
// static trust cache). Debug builds only. The one write attempted is a single byte over the trust cache holding the value it
// already has, so a monitorless boot (where the write would succeed) changes nothing.
void AdrenoAcceleratorX2::softgpuW5Check()
{
    AdrenoSoftGPU *s = fSoft;
    uint64_t monitor = 0, trust = 0;
    PE_parse_boot_argn("softgpu_w5_monitor_pa", &monitor, sizeof monitor);
    PE_parse_boot_argn("softgpu_w5_trust_pa", &trust, sizeof trust);
    bool pass = true;
    auto destroy = [&](uint32_t id) {
        const int d = sgKernel(s, sizeof(struct sgpu_destroy_memory), 0, SGPU_GUEST_NO_STAMP, 0,
                               [&](uint8_t *p) { return sgpu_guest_pkt_destroy_memory(p, sizeof(struct sgpu_destroy_memory), id); });
        if (d || sgDrain(s, SG_BUDGET_MS)) ALOG("SoftGPU: W5 check: DESTROY_MEMORY %u failed", id);
    };
    // FILL_BUFFER of `bytes` with `value` over object `id`, bound WRITE. The stream's status.
    auto fill = [&](uint32_t id, uint64_t bytes, uint32_t value, uint64_t at = 0) {
        struct sgpu_guest_bt_in in = {id, ADRENO_USAGE_WRITE, 0, at, bytes, 0};
        uint64_t off = 0, seq = 0;
        int r = sgpu_guest_arena_alloc(&s->arena, sizeof(struct sgpu_fill_buffer) + sizeof(struct sgpu_bt_entry), s->ring.next_seqno, &off);
        if (r) return r;
        sgpu_guest_pkt_fill_buffer(s->arenaVA + off, sizeof(struct sgpu_fill_buffer), 0, 0, bytes, value);
        sgpu_guest_bt_build(reinterpret_cast<struct sgpu_bt_entry *>(s->arenaVA + off + sizeof(struct sgpu_fill_buffer)), &in, 1);
        struct sgpu_desc d;
        sgpu_guest_desc_submit(&d, 1, ++s->cbSerial, SGPU_DESC_LAST, off, sizeof(struct sgpu_fill_buffer),
                               off + sizeof(struct sgpu_fill_buffer), 1, SGPU_GUEST_NO_STAMP, 0, 0, 0);
        r = sgPush(s, &d, &seq);
        return r ? r : sgDrain(s, SG_BUDGET_MS + SG_COMPILE_MS);
    };
    sgFlushPending(s);
    // 1. a monitor page: no window, so CREATE_MEMORY itself fails with SGPU_E_MEMORY
    if (monitor) {
        const uint32_t id = s->nextMem++;
        struct sgpu_mem_run run = {monitor & ~0x3fffull, 0x4000};
        const int r = sgCreateMemory(s, id, 0, 0x4000, 0, &run, 1);
        const bool ok = r == SGPU_E_MEMORY;
        ALOG("SoftGPU: W5 monitor page 0x%llx: CREATE_MEMORY status %d, want %d (SGPU_E_MEMORY): %s", run.gpa, r, SGPU_E_MEMORY, ok ? "ok" : "FAILED");
        if (!r) destroy(id);
        pass &= ok;
    }
    // 2. the trust cache: readable (a READ window), but a WRITE binding is refused
    if (trust) {
        const uint32_t id = s->nextMem++;
        struct sgpu_mem_run run = {trust & ~0x3fffull, 0x4000};
        // A fill writes ONE byte value everywhere, so the write is aimed at four bytes that already hold the same value.
        uint8_t head[256] = {};
        int at = -1;
        IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(run.gpa, 0x4000, kIODirectionIn);
        if (md && md->prepare() == kIOReturnSuccess) { md->readBytes(0, head, sizeof head); md->complete(); }
        OSSafeReleaseNULL(md);
        for (int i = 0; i + 4 <= (int)sizeof head && at < 0; i += 4)
            if (head[i] == head[i + 1] && head[i] == head[i + 2] && head[i] == head[i + 3]) at = i;
        int r = sgCreateMemory(s, id, 0, 0x4000, 0, &run, 1);
        ALOG("SoftGPU: W5 trust cache 0x%llx: CREATE_MEMORY (read) status %d, want 0: %s", run.gpa, r, r ? "FAILED" : "ok");
        pass &= !r;
        const bool made = !r;
        if (!r && at < 0) { ALOG("SoftGPU: W5 trust cache: no four equal bytes in the first 256 to aim the write at"); pass = false; }
        if (!r && at >= 0) {
            r = fill(id, 4, head[at], (uint64_t)at);
            const bool ok = r == SGPU_E_ACCESS;                       // READ is kept, WRITE refused: exactly ACCESS (spec 1.3)
            ALOG("SoftGPU: W5 trust cache WRITE binding: status %d, want %d (SGPU_E_ACCESS): %s",
                 r, SGPU_E_ACCESS, ok ? "ok" : "FAILED");
            pass &= ok;
        }
        if (made) destroy(id);
    }
    // 3. control: ordinary kernel memory takes the same write, so the refusals above are the windows' doing
    {
        IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::withOptions(kIODirectionInOut | kIOMemoryPhysicallyContiguous, 0x4000, 0x4000);
        uint64_t pa = 0;
        if (md && md->prepare() == kIOReturnSuccess && sgContiguous(md, &pa)) {
            uint8_t *va = static_cast<uint8_t *>(md->getBytesNoCopy());
            bzero(va, 0x4000);
            const uint32_t id = s->nextMem++;
            struct sgpu_mem_run run = {pa, 0x4000};
            int r = sgCreateMemory(s, id, 0, 0x4000, 0, &run, 1);
            if (!r) r = fill(id, 64, 0x5a);
            bool same = true;
            for (unsigned i = 0; i < 64; i++) same &= va[i] == 0x5a;
            const bool ok = !r && same && va[64] == 0;
            ALOG("SoftGPU: W5 control page 0x%llx: write status %d, bytes %s: %s", pa, r, same ? "0x5a" : "unchanged", ok ? "ok" : "FAILED");
            pass &= ok;
            if (!r || r == SGPU_E_ACCESS || r == SGPU_E_MEMORY) destroy(id);
            md->complete();
        } else {
            ALOG("SoftGPU: W5 control page: no contiguous memory");
            pass = false;
        }
        OSSafeReleaseNULL(md);
    }
    ALOG("SoftGPU: completion irqs %llu, waits ended by the interrupt %llu, by the poll %llu, empty slices %llu (interrupt %s)",
         __atomic_load_n(&s->irqCount, __ATOMIC_RELAXED), s->irqWakes, s->pollWakes, s->pollTimeouts, s->irq ? "armed" : "absent");
    ALOG("SoftGPU: W5 check %s (windows, lock and refusals)", pass ? "PASSED" : "FAILED");
    setProperty("SoftGPUW5", pass ? 1ULL : 0ULL, 32);
}
#endif

// SOFTGPU_KCMD_STREAM. Entries of the segment list become binding-table entries in list order; their memory objects
// are created on first use from the map's physical runs. 0, or an SGPU_E_* / ADRENO_* result for submission error 0xa
// (the queue maps SGPU_E_* through sgpu_guest_iogpu_status).
// TODO(spec review item M7): fSubmitLock is still held while this sleeps for the completion (up to SG_BUDGET_MS +
// SG_COMPILE_MS + 3 s, about 25 s, for a stream), now on the interrupt instead of a poll. Dropping it across the wait needs the
// ring and the arena owned by one more lock; W5 did not change who may submit while a slow compile runs.
int AdrenoAcceleratorX2::submitStream(AdrenoSegmentList *list, const void *command, uint64_t bytes)
{
    if (!list || !command) return ADRENO_LIST_E_ARGUMENT;
    AdrenoSoftGPU *s = fSoft;
    if (!s || !fSubmitLock) return ADRENO_SUBMIT_E_INACTIVE;
    // Before the lock (map locks are taken inside the IB path under fSubmitLock, never the other way round): the runs
    // of every entry whose memory object is missing or from an older epoch. They go into one pool that grows with what
    // the maps really have (a map has few runs, not one per page), once per distinct map, up to
    // SGPU_GUEST_POOL_MAX_RUNS for the whole stream; a list needing more is refused.
    const uint64_t count = list->entryCount();
    if (count > SGPU_MAX_BT_COUNT) return SGPU_E_FORMAT;
    struct Ent { AdrenoMemoryMap *map; uint32_t usage, resourceId, runOff, runs, head; uint64_t size; };
    const size_t entBytes = (size_t)count * sizeof(Ent) + 1;
    Ent *ent = static_cast<Ent *>(IOMalloc(entBytes));
    if (!ent) return SGPU_E_HOST;
    bzero(ent, entBytes);
    struct sgpu_mem_run *pool = nullptr;
    uint32_t poolCap = 0, poolUsed = 0;
    uint64_t *distinct = nullptr;
    const uint32_t distinctSlots = sgpu_guest_set_slots((uint32_t)count);
    int result = 0;
    const uint64_t seen = __atomic_load_n(&s->epoch, __ATOMIC_ACQUIRE);
    for (uint64_t i = 0; i < count && !result; i++) {
        if (!list->entryAt(i, &ent[i].map, &ent[i].usage, &ent[i].resourceId) || !ent[i].map) { result = SGPU_E_OBJECT; break; }
        if (ent[i].map->softgpuId(seen)) continue;
        if (!distinct) {
            distinct = static_cast<uint64_t *>(IOMalloc((size_t)distinctSlots * sizeof *distinct));
            if (!distinct) { result = SGPU_E_HOST; break; }
            bzero(distinct, (size_t)distinctSlots * sizeof *distinct);
        }
        const int added = sgpu_guest_set_add(distinct, distinctSlots, (uint64_t)(uintptr_t)ent[i].map);
        if (added <= 0) { result = added; continue; }       // 0: the same map again, its runs are already in the pool
        for (;;) {
            result = ent[i].map->softgpuRuns(pool + poolUsed, poolCap - poolUsed, &ent[i].runs, &ent[i].head, &ent[i].size);
            if (result != SGPU_E_BOUNDS) break;
            const uint32_t grown = sgpu_guest_pool_grow(poolCap);
            if (!grown) { result = SGPU_E_QUOTA; break; }
            struct sgpu_mem_run *bigger = static_cast<struct sgpu_mem_run *>(IOMalloc((size_t)grown * sizeof *bigger));
            if (!bigger) { result = SGPU_E_HOST; break; }
            if (pool) { memcpy(bigger, pool, (size_t)poolUsed * sizeof *bigger); IOFree(pool, (size_t)poolCap * sizeof *pool); }
            pool = bigger;
            poolCap = grown;
        }
        ent[i].runOff = poolUsed;
        if (!result) poolUsed += ent[i].runs;
    }
    if (distinct) IOFree(distinct, (size_t)distinctSlots * sizeof *distinct);
    IOLockLock(fSubmitLock);
    s = fSoft;
    uint32_t packetOffset = 0;
    bool stale = false, wedged = false;
    if (!result && s && s->on) {
        result = sgFlushPending(s);                         // DESTROYs of dead objects: only a lost device is this stream's
        const uint8_t *stream = nullptr;
        uint32_t sbytes = 0;
        if (!result) {
            result = sgpu_guest_kcmd_stream(command, bytes, __atomic_load_n(&s->epoch, __ATOMIC_RELAXED), &stream, &sbytes);
            if (result) stale = true;                       // a stale plug-in epoch is refused (status 5), it never resets
        }
        // memory objects of this epoch for every entry (a stale id with no runs captured means a reset raced us)
        for (uint64_t i = 0; i < count && !result; i++) {
            const uint32_t id = ent[i].map->softgpuId(s->epoch);
            if (id) continue;
            if (!ent[i].runs) { result = SGPU_E_LOST; stale = true; break; }
            const uint32_t nid = s->nextMem++;
            result = sgCreateMemory(s, nid, 0, ent[i].size, ent[i].head, pool + ent[i].runOff, ent[i].runs);
            if (!result) ent[i].map->softgpuSet(nid, s->epoch);   // only an object the device accepted
        }
        if (!result) {
            const uint64_t total = (uint64_t)sbytes + 32ull * count;
            uint64_t off = 0, seq = 0;
            result = sgpu_guest_arena_alloc(&s->arena, total, s->ring.next_seqno, &off);
            if (result == SGPU_GUEST_BUSY && !(result = sgDrain(s, SG_BUDGET_MS)))
                result = sgpu_guest_arena_alloc(&s->arena, total, s->ring.next_seqno, &off);
            if (!result) {
                memcpy(s->arenaVA + off, stream, sbytes);
                struct sgpu_bt_entry *bt = reinterpret_cast<struct sgpu_bt_entry *>(s->arenaVA + off + sbytes);
                for (uint64_t i = 0; i < count; i++) {
                    struct sgpu_guest_bt_in in = {ent[i].map->softgpuId(s->epoch), ent[i].usage, ent[i].map->softgpuReadOnly(), 0,
                                                  ent[i].map->softgpuLogicalBytes(), ent[i].resourceId};
                    sgpu_guest_bt_build(&bt[i], &in, 1);
                }
                struct sgpu_desc d;
                sgpu_guest_desc_submit(&d, 1, ++s->cbSerial, SGPU_DESC_LAST, off, sbytes, off + sbytes, (uint32_t)count,
                                       SGPU_GUEST_NO_STAMP, 0, 0, (uint64_t)(uintptr_t)list);
                result = sgPush(s, &d, &seq);
                if (result == SGPU_GUEST_BUSY && !(result = sgDrain(s, SG_BUDGET_MS))) result = sgPush(s, &d, &seq);
                // renderd's watchdog bound (1.11)
                if (!result) result = sgWaitDone(s, seq, SG_BUDGET_MS + SG_COMPILE_MS + 3000, &packetOffset);
                if (result == SGPU_E_TIMEOUT) {                 // still running: abort it and take the queue back
                    sgWrite(s, SGPU_REG_CONTROL, SGPU_CTL_RESET_QUEUE);
                    if (sgPoll(SG_BUDGET_MS, [&] { return (sgRead(s, SGPU_REG_IRQ_RAW) & SGPU_IRQ_RESET_DONE) != 0; })) {
                        sgWrite(s, SGPU_REG_IRQ_STATUS, SGPU_IRQ_RESET_DONE);
                        sgRetire(s, nullptr);
                    } else {
                        // Unconfirmed: the device may still be running this stream against the list's pages. Not
                        // "lost": IOGPU would unprepare (unpin) them. UNCERTAIN keeps the pins; the device is reset
                        // below, and if that is unconfirmed too the transport stays off.
                        result = ADRENO_SUBMIT_E_UNCERTAIN;
                        wedged = true;
                    }
                }
            }
        }
        if (wedged || (result == SGPU_E_LOST && !stale)) {      // the device said so: FATAL, or a bad record
            softgpuLost(result);
            result = sgpu_guest_reset_result(result, !s->resetUnconfirmed);
        }
    } else if (!result) {
        result = ADRENO_SUBMIT_E_INACTIVE;
    }
    IOLockUnlock(fSubmitLock);
    if (s && s == fSoft && s->irq) {                            // what the board check reads from ioreg
        setProperty("SoftGPUIrqs", __atomic_load_n(&s->irqCount, __ATOMIC_RELAXED), 64);
        setProperty("SoftGPUIrqWakes", s->irqWakes, 64);
        setProperty("SoftGPUPollWakes", s->pollWakes, 64);
    }
    result = sgpu_guest_public_result(result);                  // SGPU_GUEST_BUSY / _E_RECORD are host-only codes
    if (result && result != ADRENO_SUBMIT_E_INACTIVE)
        ALOG("SoftGPU: stream refused %d at packet 0x%x", result, packetOffset);
    if (pool) IOFree(pool, (size_t)poolCap * sizeof *pool);
    IOFree(ent, entBytes);
    return result;
}
#endif
