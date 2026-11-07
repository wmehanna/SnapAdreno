/* SPDX-License-Identifier: MIT */
/* The per-task SoftGPU context rules (src/AdrenoX2/softgpu-ctx.h): two tasks never share a context, a dead task's
 * context is destroyed (and counted free) before the next one is made, the live count is bounded, a reset starts every
 * task over and no id is ever handed out twice in an epoch. A model of the stream path drives them the way
 * AdrenoAcceleratorX2::submitStream does (want, CREATE_CONTEXT, made; free() -> pending; flush -> DESTROY_CONTEXT).
 *   gcc -std=gnu11 -O1 -g -fsanitize=address,undefined -Wall -I$SNAP/compat -o test-softgpu-ctx tests/test-softgpu-ctx.c
 *   ./test-softgpu-ctx        ($SNAP is the Snapintosh-int superproject) */
#include <stdio.h>
#include <string.h>
#include "softgpu-guest.h"
#include "../src/AdrenoX2/softgpu-ctx.h"
static unsigned checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
/* ---- a model of the device's context table, with the host's own ceiling (sgpu_exec.cpp: 64) ---- */
static struct { uint32_t id; int used; } host[64];
static unsigned host_live(void) { unsigned n = 0; for (int i = 0; i < 64; i++) n += host[i].used; return n; }
static int host_has(uint32_t id) { for (int i = 0; i < 64; i++) if (host[i].used && host[i].id == id) return 1; return 0; }
static int host_create(uint32_t id)
{
    if (!id || host_has(id)) return SGPU_E_OBJECT;
    for (int i = 0; i < 64; i++) if (!host[i].used) { host[i].used = 1; host[i].id = id; return 0; }
    return SGPU_E_QUOTA;
}
static int host_destroy(uint32_t id)
{
    for (int i = 0; i < 64; i++) if (host[i].used && host[i].id == id) { host[i].used = 0; return 0; }
    return SGPU_E_OBJECT;
}
static struct sgpu_ctx_book book;
static struct sgpu_ctx_pend pend;
static void device_reset(uint64_t epoch)
{
    memset(host, 0, sizeof host);
    memset(&pend, 0, sizeof pend);
    sgpu_ctx_book_reset(&book, epoch);
}
/* A stream of a task: the context it runs in, or the error. */
static int stream(struct sgpu_ctx_owner *task, uint32_t *ctx)
{
    int create = 0;
    int r = sgpu_ctx_want(&book, task, ctx, &create);
    if (!r && create) {
        r = host_create(*ctx);
        if (!r) sgpu_ctx_made(&book, task, *ctx);
    }
    return r;
}
/* The task is gone (AdrenoTask::free) */
static void task_dies(struct sgpu_ctx_owner *task)
{
    uint64_t e;
    const uint32_t id = sgpu_ctx_owner_take(task, &e);
    if (id) sgpu_ctx_pend_push(&pend, id, e);
}
/* The next stream's sgFlushPending */
static void flush(void)
{
    uint32_t ids[16], n;
    while ((n = sgpu_ctx_pend_take(&pend, book.epoch, ids, 16))) {
        for (uint32_t i = 0; i < n; i++) CHECK(host_destroy(ids[i]) == 0);
        sgpu_ctx_destroyed(&book, n);
    }
}
int main(void)
{
    struct sgpu_ctx_owner a = {0, 0}, b = {0, 0}, t[SGPU_CTX_MAX_LIVE + 2];
    uint32_t ca = 0, cb = 0, c2 = 0;
    device_reset(7);

    /* two tasks, two contexts; the same task keeps its own; nothing is made twice */
    CHECK(stream(&a, &ca) == 0 && ca == 1);
    CHECK(stream(&b, &cb) == 0 && cb == 2 && cb != ca);
    CHECK(stream(&a, &c2) == 0 && c2 == ca);
    CHECK(stream(&b, &c2) == 0 && c2 == cb);
    CHECK(host_live() == 2 && book.live == 2);

    /* a task can only run in the context it owns: forged records (another epoch, another id) are replaced, never trusted */
    { struct sgpu_ctx_owner forged = {7, 0}; CHECK(stream(&forged, &c2) == 0 && c2 != ca && c2 != cb); task_dies(&forged); flush(); }
    { struct sgpu_ctx_owner stale = {3, cb}; CHECK(stream(&stale, &c2) == 0 && c2 != cb && stale.epoch == 7); task_dies(&stale); flush(); }
    CHECK(host_live() == 2 && book.live == 2);

    /* a killed task: its context is destroyed by the next stream, the neighbour's stays, the id is never reused */
    task_dies(&a);
    CHECK(a.id == 0 && host_has(ca));                     /* destroyed by the flush, not by free() */
    flush();
    CHECK(!host_has(ca) && host_has(cb) && book.live == 1);
    { struct sgpu_ctx_owner fresh = {0, 0}; CHECK(stream(&fresh, &c2) == 0 && c2 != ca && c2 != cb); task_dies(&fresh); flush(); }
    CHECK(stream(&a, &ca) == 0 && ca > cb);              /* the dead task's record is empty: a new context, a new id */

    /* the bound: SGPU_CTX_MAX_LIVE at once, one more is refused, a dead task's slot is reusable after the flush */
    device_reset(8);
    memset(t, 0, sizeof t);
    uint32_t seen[SGPU_CTX_MAX_LIVE + 2];
    for (unsigned i = 0; i < SGPU_CTX_MAX_LIVE; i++) {
        CHECK(stream(&t[i], &seen[i]) == 0);
        for (unsigned j = 0; j < i; j++) CHECK(seen[i] != seen[j]);
    }
    CHECK(book.live == SGPU_CTX_MAX_LIVE && host_live() == SGPU_CTX_MAX_LIVE);
    CHECK(stream(&t[SGPU_CTX_MAX_LIVE], &c2) == SGPU_E_QUOTA);
    CHECK(t[SGPU_CTX_MAX_LIVE].id == 0 && host_live() == SGPU_CTX_MAX_LIVE);      /* a refusal makes nothing */
    CHECK(stream(&t[SGPU_CTX_MAX_LIVE], &c2) == SGPU_E_QUOTA);                    /* and keeps refusing */
    CHECK(SGPU_CTX_MAX_LIVE < 64);                                                /* room under the host's own limit */
    task_dies(&t[3]);
    CHECK(stream(&t[SGPU_CTX_MAX_LIVE], &c2) == SGPU_E_QUOTA);                    /* not before the DESTROY went out */
    flush();
    CHECK(stream(&t[SGPU_CTX_MAX_LIVE], &c2) == 0 && host_live() == SGPU_CTX_MAX_LIVE);
    CHECK(c2 > seen[SGPU_CTX_MAX_LIVE - 1]);
    for (unsigned i = 0; i < SGPU_CTX_MAX_LIVE + 1; i++) task_dies(&t[i]);
    flush();
    CHECK(host_live() == 0 && book.live == 0);

    /* close and reopen many times: nothing accumulates */
    for (unsigned round = 0; round < 500; round++) {
        struct sgpu_ctx_owner x = {0, 0};
        CHECK(stream(&x, &c2) == 0);
        task_dies(&x);
        flush();
    }
    CHECK(host_live() == 0 && book.live == 0);

    /* a reset: the host forgot every context, so every task asks again and the dead tasks' pending ids are dropped */
    device_reset(9);
    CHECK(stream(&a, &ca) == 0 && stream(&b, &cb) == 0);
    task_dies(&b);
    device_reset(10);
    CHECK(pend.n == 0 && host_live() == 0 && book.live == 0);
    CHECK(stream(&a, &c2) == 0 && a.epoch == 10 && host_live() == 1 && book.live == 1);
    sgpu_ctx_pend_push(&pend, 99, 9);                                              /* an older epoch's id */
    { uint32_t ids[4]; CHECK(sgpu_ctx_pend_take(&pend, 10, ids, 4) == 0 && pend.n == 0); }

    /* the pending list is bounded: a full list refuses, the context waits for the next reset */
    memset(&pend, 0, sizeof pend);
    for (unsigned i = 0; i < SGPU_CTX_PEND_MAX; i++) CHECK(sgpu_ctx_pend_push(&pend, 1000 + i, 10) == 0);
    CHECK(sgpu_ctx_pend_push(&pend, 5000, 10) == SGPU_E_QUOTA && pend.n == SGPU_CTX_PEND_MAX);
    CHECK(sgpu_ctx_pend_push(&pend, 0, 10) == 0 && pend.n == SGPU_CTX_PEND_MAX);   /* no id: nothing to queue */
    { uint32_t ids[16]; unsigned total = 0, n; while ((n = sgpu_ctx_pend_take(&pend, 10, ids, 16))) total += n; CHECK(total == SGPU_CTX_PEND_MAX); }

    /* destroyed() never underflows */
    book.live = 2; sgpu_ctx_destroyed(&book, 5); CHECK(book.live == 0);
    /* the counter never wraps into ids already used or into 0 */
    book.next = 0xfffffffeu; book.live = 0;
    { struct sgpu_ctx_owner x = {0, 0}; uint32_t id; int create; CHECK(sgpu_ctx_want(&book, &x, &id, &create) == 0 && id == 0xfffffffeu && create); sgpu_ctx_made(&book, &x, id); }
    { struct sgpu_ctx_owner x = {0, 0}; uint32_t id; int create; CHECK(sgpu_ctx_want(&book, &x, &id, &create) == SGPU_E_QUOTA); }
    book.next = 0;
    { struct sgpu_ctx_owner x = {0, 0}; uint32_t id; int create; CHECK(sgpu_ctx_want(&book, &x, &id, &create) == SGPU_E_QUOTA); }

    printf("softgpu-ctx: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
