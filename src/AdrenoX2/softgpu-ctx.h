// SPDX-License-Identifier: MIT
// SoftGPU contexts, one per IOGPU task (softgpu/SOFTGPU-PROTOCOL.md 1.8): the bookkeeping, as pure C so a host test
// (tests/test-softgpu-ctx.c) runs every rule against libsgpu's own context table. The host's context is the namespace of
// a process's pipelines, samplers, libraries and depth-stencil states, so a context per task keeps one process's
// object ids, and its leftovers after it died, away from every other process.
//   - The id of a task's context is chosen here, from a counter that never repeats within a device epoch. User space
//     never supplies it: CREATE_CONTEXT and DESTROY_CONTEXT are kernel packets the host refuses in a SUBMIT stream,
//     and the id in a SUBMIT descriptor comes from the task that owns the command buffer's device.
//   - At most SGPU_CTX_MAX_LIVE contexts exist at once (the host stops at 64); a task asking for one past that is
//     refused with SGPU_E_QUOTA, and the contexts of dead tasks are destroyed before the next one is made.
//   - A task's context dies with the task: its owner record is handed to the pending list, and the next stream
//     (under the submit lock) sends DESTROY_CONTEXT, which frees the context's objects and no memory object.
//   - After a device reset the host holds no context: a record from an older epoch is simply replaced by a new context.
// Needs softgpu-guest.h (SGPU_E_*) included first. No allocation, no locking: the caller serializes.
#pragma once
#include <stdint.h>

#define SGPU_CTX_MAX_LIVE 32u      /* contexts at once; the host's own ceiling is 64 (sgpu_exec.cpp) */
#define SGPU_CTX_PEND_MAX 64u      /* dead tasks whose DESTROY_CONTEXT has not been sent yet */

struct sgpu_ctx_owner {            /* a task's context: lives inside AdrenoTask */
    uint64_t epoch;                /* the device epoch `id` belongs to */
    uint32_t id;                   /* 0 none */
};
struct sgpu_ctx_book {             /* the device's: under fSubmitLock */
    uint64_t epoch;
    uint32_t next;                 /* the next id, from 1; never reused within an epoch */
    uint32_t live;                 /* made in this epoch, destroy not yet sent */
};
struct sgpu_ctx_pend {             /* the dead tasks' contexts; the kext guards it with a spin lock */
    uint32_t n;
    struct { uint32_t id; uint64_t epoch; } e[SGPU_CTX_PEND_MAX];
};

/* A new device epoch (bring-up or reset): the host holds no context any more. */
static inline void sgpu_ctx_book_reset(struct sgpu_ctx_book *b, uint64_t epoch)
{
    b->epoch = epoch;
    b->next = 1;
    b->live = 0;
}
/* The context id for a descriptor of the task owning `o`: *id, and *create = 1 when CREATE_CONTEXT must be sent first.
 * 0, or SGPU_E_QUOTA at the live limit or when the counter ran out. Nothing is recorded until sgpu_ctx_made. */
static inline int sgpu_ctx_want(const struct sgpu_ctx_book *b, const struct sgpu_ctx_owner *o, uint32_t *id, int *create)
{
    if (o->id && o->epoch == b->epoch) { *id = o->id; *create = 0; return 0; }
    if (b->live >= SGPU_CTX_MAX_LIVE || !b->next || b->next == 0xffffffffu) return SGPU_E_QUOTA;
    *id = b->next;
    *create = 1;
    return 0;
}
/* The host accepted CREATE_CONTEXT for `id` (the value sgpu_ctx_want returned): the task owns it. */
static inline void sgpu_ctx_made(struct sgpu_ctx_book *b, struct sgpu_ctx_owner *o, uint32_t id)
{
    b->next = id + 1;
    b->live++;
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
/* `n` DESTROY_CONTEXT descriptors went out: they no longer count against the limit. */
static inline void sgpu_ctx_destroyed(struct sgpu_ctx_book *b, uint32_t n)
{
    b->live = n < b->live ? b->live - n : 0;
}
