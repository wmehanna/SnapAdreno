// SPDX-License-Identifier: MIT
// SoftGPU contexts, one per IOGPU task (softgpu/SOFTGPU-PROTOCOL.md 1.8): the bookkeeping, as pure C so a host test
// (tests/test-softgpu-ctx.c) runs every rule against libsgpu's own context table. The host's context is the namespace of
// a process's pipelines, samplers, libraries and depth-stencil states, so a context per task keeps one process's
// object ids, and its leftovers after it died, away from every other process.
//   - The id of a task's context is chosen here, from a counter that never repeats within a device epoch. User space
//     never supplies it: CREATE_CONTEXT and DESTROY_CONTEXT are kernel packets the host refuses in a SUBMIT stream,
//     and the id in a SUBMIT descriptor comes from the task that owns the command buffer's device.
//   - At most SGPU_CTX_MAX_LIVE contexts exist at once (the host stops at 64); a task asking for one past that is
//     refused with SGPU_E_QUOTA, and the contexts of dead tasks are destroyed before the next one is made. The bound is
//     shared out so one process cannot starve the others: at most SGPU_CTX_PER_PID contexts per process (a process opens
//     one IOGPU device, hence one task, per connection), and SGPU_CTX_WS_RESERVE of the slots are for WindowServer's
//     tasks only (the entitlement check of AdrenoTask::withTask), so a flood of other clients never keeps it out.
//   - A task's context dies with the task: its owner record is handed to the pending list, and the next stream
//     (under the submit lock) sends DESTROY_CONTEXT, which frees the context's objects and no memory object.
//   - After a device reset the host holds no context: a record from an older epoch is simply replaced by a new context.
// Needs softgpu-guest.h (SGPU_E_*) included first. No allocation, no locking: the caller serializes.
#pragma once
#include <stdint.h>

#define SGPU_CTX_MAX_LIVE 32u      /* contexts at once; the host's own ceiling is 64 (sgpu_exec.cpp) */
#define SGPU_CTX_PER_PID 8u        /* contexts one process (pid) may hold at once */
#define SGPU_CTX_WS_RESERVE 4u     /* of SGPU_CTX_MAX_LIVE, the slots only a WindowServer task may take */
#define SGPU_CTX_PEND_MAX 64u      /* dead tasks whose DESTROY_CONTEXT has not been sent yet */
/* AIR a process may pin in the host's shared library store. The host does not know the process, so the share is made by
 * construction: SGPU_CTX_PER_PID contexts per process (above) times the host's per-context AIR cap (AIR_CTX_CAP in the
 * host's sgpu_exec.cpp, which must equal SGPU_CTX_AIR_CAP). */
#define SGPU_CTX_AIR_PID_CAP (64u << 20)
#define SGPU_CTX_AIR_CAP (SGPU_CTX_AIR_PID_CAP / SGPU_CTX_PER_PID)
_Static_assert(SGPU_CTX_AIR_CAP * SGPU_CTX_PER_PID <= SGPU_CTX_AIR_PID_CAP, "per-process AIR share");

struct sgpu_ctx_owner {            /* a task's context: lives inside AdrenoTask */
    uint64_t epoch;                /* the device epoch `id` belongs to */
    uint32_t id;                   /* 0 none */
    int32_t pid;                   /* the process that owns the task; set once, when the task is made */
    uint32_t ws;                   /* 1: WindowServer's task (may use the reserved slots); set once */
};
struct sgpu_ctx_slot { uint32_t id; int32_t pid; uint32_t ws; };    /* id 0: free */
struct sgpu_ctx_book {             /* the device's: under fSubmitLock */
    uint64_t epoch;
    uint32_t next;                 /* the next id, from 1; never reused within an epoch */
    uint32_t live;                 /* made in this epoch, destroy not yet sent (the used slots) */
    struct sgpu_ctx_slot slot[SGPU_CTX_MAX_LIVE];
};
struct sgpu_ctx_pend {             /* the dead tasks' contexts; the kext guards it with a spin lock */
    uint32_t n;
    struct { uint32_t id; uint64_t epoch; } e[SGPU_CTX_PEND_MAX];
};

/* The record of a task made for process `pid` (-1: unknown) that is WindowServer's when `ws` is non-zero. Set once. */
static inline void sgpu_ctx_owner_init(struct sgpu_ctx_owner *o, int32_t pid, int ws)
{
    o->epoch = 0;
    o->id = 0;
    o->pid = pid;
    o->ws = ws ? 1 : 0;
}
/* The pid of a task's BSD process, -1 when it has none. */
static inline int32_t sgpu_ctx_proc_pid(void *proc, int (*pid_of)(void *))
{
    return proc ? (int32_t)pid_of(proc) : -1;
}

/* A new device epoch (bring-up or reset): the host holds no context any more. */
static inline void sgpu_ctx_book_reset(struct sgpu_ctx_book *b, uint64_t epoch)
{
    b->epoch = epoch;
    b->next = 1;
    b->live = 0;
    for (uint32_t i = 0; i < SGPU_CTX_MAX_LIVE; i++) b->slot[i].id = 0;
}
/* The context id for a descriptor of the task owning `o`: *id, and *create = 1 when CREATE_CONTEXT must be sent first.
 * 0, or SGPU_E_QUOTA at the live limit, past the process's SGPU_CTX_PER_PID, for a task that is not WindowServer's when
 * only the reserved slots are left, or when the counter ran out. Nothing is recorded until sgpu_ctx_made. */
static inline int sgpu_ctx_want(const struct sgpu_ctx_book *b, const struct sgpu_ctx_owner *o, uint32_t *id, int *create)
{
    if (o->id && o->epoch == b->epoch) { *id = o->id; *create = 0; return 0; }
    if (b->live >= SGPU_CTX_MAX_LIVE || !b->next || b->next == 0xffffffffu) return SGPU_E_QUOTA;
    uint32_t mine = 0, others = 0;
    for (uint32_t i = 0; i < SGPU_CTX_MAX_LIVE; i++) {
        if (!b->slot[i].id) continue;
        if (b->slot[i].pid == o->pid) mine++;
        if (!b->slot[i].ws) others++;
    }
    if (mine >= SGPU_CTX_PER_PID) return SGPU_E_QUOTA;
    if (!o->ws && others >= SGPU_CTX_MAX_LIVE - SGPU_CTX_WS_RESERVE) return SGPU_E_QUOTA;
    *id = b->next;
    *create = 1;
    return 0;
}
/* The host accepted CREATE_CONTEXT for `id` (the value sgpu_ctx_want returned): the task owns it. */
static inline void sgpu_ctx_made(struct sgpu_ctx_book *b, struct sgpu_ctx_owner *o, uint32_t id)
{
    b->next = id + 1;
    for (uint32_t i = 0; i < SGPU_CTX_MAX_LIVE; i++)
        if (!b->slot[i].id) {
            b->slot[i].id = id;
            b->slot[i].pid = o->pid;
            b->slot[i].ws = o->ws;
            b->live++;
            break;
        }
    o->id = id;
    o->epoch = b->epoch;
}
/* The task is gone: forget its record and return what to destroy (id 0: nothing). */
static inline uint32_t sgpu_ctx_owner_take(struct sgpu_ctx_owner *o, uint64_t *epoch)
{
    const uint32_t id = o->id;
    *epoch = o->epoch;
    o->id = 0;
    o->epoch = 0;
    return id;
}
/* Queue a dead task's context. SGPU_E_QUOTA when the list is full: the context then lives until the next reset. */
static inline int sgpu_ctx_pend_push(struct sgpu_ctx_pend *p, uint32_t id, uint64_t epoch)
{
    if (!id) return 0;
    if (p->n >= SGPU_CTX_PEND_MAX) return SGPU_E_QUOTA;
    p->e[p->n].id = id;
    p->e[p->n].epoch = epoch;
    p->n++;
    return 0;
}
/* Take up to `cap` pending ids that belong to `epoch` (older ones are dropped, the host forgot them). Returns the count. */
static inline uint32_t sgpu_ctx_pend_take(struct sgpu_ctx_pend *p, uint64_t epoch, uint32_t *ids, uint32_t cap)
{
    uint32_t n = 0;
    while (p->n && n < cap) {
        p->n--;
        if (p->e[p->n].epoch == epoch) ids[n++] = p->e[p->n].id;
    }
    return n;
}
/* DESTROY_CONTEXT for `id` went out: it no longer counts against any limit. An id the book does not hold (a reset
 * already forgot it) changes nothing. */
static inline void sgpu_ctx_destroyed(struct sgpu_ctx_book *b, uint32_t id)
{
    if (!id) return;
    for (uint32_t i = 0; i < SGPU_CTX_MAX_LIVE; i++)
        if (b->slot[i].id == id) { b->slot[i].id = 0; if (b->live) b->live--; return; }
}

/* ---- the wiring: what AdrenoAcceleratorX2::submitStream, sgFlushPending and AdrenoTask::free do with the rules above,
 * as functions the kext calls and tests/test-softgpu-ctx.c drives with a model of the device. ---- */

/* A dead task's context goes to the pending list through `gone` (the epoch-checking, locking hook); nothing if it has none. */
static inline void sgpu_ctx_task_gone(struct sgpu_ctx_owner *o, void (*gone)(uint32_t id, uint64_t epoch))
{
    uint64_t epoch = 0;
    const uint32_t id = sgpu_ctx_owner_take(o, &epoch);
    if (id) gone(id, epoch);
}
/* Send DESTROY_CONTEXT for ids[0..n) in order through `send` (0: the descriptor is queued). Every queued id leaves the book;
 * at the first failure (*err) the rest are not sent and the count returned says where to resume: the caller puts
 * ids[count..n) back on the pending list, so the book and the device never disagree about a context that is still there. */
static inline uint32_t sgpu_ctx_destroy_batch(struct sgpu_ctx_book *b, const uint32_t *ids, uint32_t n,
                                              int (*send)(void *cookie, uint32_t id), void *cookie, int *err)
{
    uint32_t i = 0;
    *err = 0;
    for (; i < n; i++) {
        const int r = send(cookie, ids[i]);
        if (r) { *err = r; break; }
        sgpu_ctx_destroyed(b, ids[i]);
    }
    return i;
}
/* Put ids back on the pending list (epoch `epoch`); returns how many did not fit (they live until the next reset). */
static inline uint32_t sgpu_ctx_requeue(struct sgpu_ctx_pend *p, const uint32_t *ids, uint32_t n, uint64_t epoch)
{
    uint32_t lost = 0;
    for (uint32_t i = 0; i < n; i++)
        if (sgpu_ctx_pend_push(p, ids[i], epoch)) lost++;
    return lost;
}

/* One drain pass over `ids[0..n)`, taken by sgpu_ctx_flush_take: destroy_batch, and what it could not send goes back on the
 * pending list under the caller's lock, in the book's own epoch. Returns 1 when something is left over (the caller stops
 * its pass instead of spinning on a queue that keeps refusing); *err is the send error, *kept what did not fit back. */
struct sgpu_ctx_flush_ops {
    int (*send)(void *cookie, uint32_t id);     /* DESTROY_CONTEXT pushed: 0 */
    void (*lock)(void *cookie);                 /* the lock of the pending list */
    void (*unlock)(void *cookie);
    void *cookie;
};
static inline uint32_t sgpu_ctx_flush_take(struct sgpu_ctx_pend *p, const struct sgpu_ctx_book *b, uint32_t *ids, uint32_t cap)
{
    return sgpu_ctx_pend_take(p, b->epoch, ids, cap);
}
static inline int sgpu_ctx_flush_batch(struct sgpu_ctx_book *b, struct sgpu_ctx_pend *p, const uint32_t *ids, uint32_t n,
                                       const struct sgpu_ctx_flush_ops *ops, int *err, uint32_t *kept)
{
    const uint32_t sent = sgpu_ctx_destroy_batch(b, ids, n, ops->send, ops->cookie, err);
    *kept = 0;
    if (sent >= n) return 0;
    ops->lock(ops->cookie);
    *kept = sgpu_ctx_requeue(p, ids + sent, n - sent, b->epoch);
    ops->unlock(ops->cookie);
    return 1;
}
/* Does the drain loop go round again: more is pending, and the last pass was not cut short. */
static inline int sgpu_ctx_flush_again(int more, int stuck)
{
    return more && !stuck;
}
/* A context the kext made for itself (the self-test's) is done with. `sent_err` is the DESTROY_CONTEXT push, `done_err`
 * its completion (0 when it was not pushed). A lost or timed-out device forgets everything: that error is returned and the
 * caller resets (the book starts over). A destroy that was never queued goes to the pending list through `gone`, so the
 * next stream sends it; otherwise the context has left the host (or the host refused it) and its slot is free. */
static inline int sgpu_ctx_retire(struct sgpu_ctx_book *b, struct sgpu_ctx_owner *o, int sent_err, int done_err,
                                  void (*gone)(uint32_t id, uint64_t epoch))
{
    uint64_t epoch = 0;
    const uint32_t id = sgpu_ctx_owner_take(o, &epoch);
    const int err = sent_err ? sent_err : done_err;
    if (err == SGPU_E_LOST || err == SGPU_E_TIMEOUT) return err;
    if (id && sent_err) gone(id, epoch);
    else sgpu_ctx_destroyed(b, id);
    return 0;
}

struct sgpu_ctx_ops {              /* the device side of a stream, under fSubmitLock */
    int (*flush)(void *cookie);                 /* sgFlushPending: DESTROYs of dead objects and contexts */
    int (*create)(void *cookie, uint32_t id);   /* sgCreateContext */
    void *cookie;
};
struct sgpu_ctx_stream {           /* the context a stream runs in: the only place a descriptor's context id comes from */
    uint32_t ctx;
};
/* Open a stream of the task owning `o`: dead tasks' contexts are destroyed first (so a freed slot serves this very
 * stream), then the task's context is found or made (CREATE_CONTEXT before it is recorded). 0, or the error, and st->ctx
 * stays 0 then. */
static inline int sgpu_ctx_stream_open(const struct sgpu_ctx_ops *ops, struct sgpu_ctx_book *b, struct sgpu_ctx_owner *o,
                                       struct sgpu_ctx_stream *st)
{
    uint32_t id = 0;
    int create = 0;
    st->ctx = 0;
    int r = ops->flush(ops->cookie);
    if (!r) r = sgpu_ctx_want(b, o, &id, &create);
    if (!r && create) {
        r = ops->create(ops->cookie, id);
        if (!r) sgpu_ctx_made(b, o, id);
    }
    if (!r) st->ctx = id;
    return r;
}
/* The SUBMIT descriptor of an opened stream: its context is the stream's, never a constant and never read from user memory. */
static inline void sgpu_ctx_stream_desc(const struct sgpu_ctx_stream *st, struct sgpu_desc *d, uint32_t cb_id,
                                        uint16_t flags, uint64_t stream_offset, uint32_t stream_bytes, uint64_t bt_offset,
                                        uint32_t bt_count, uint32_t stamp_index, uint32_t stamp_value, uint32_t budget_ms,
                                        uint64_t tag)
{
    sgpu_guest_desc_submit(d, st->ctx, cb_id, flags, stream_offset, stream_bytes, bt_offset, bt_count, stamp_index,
                           stamp_value, budget_ms, tag);
}
