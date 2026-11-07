/* SPDX-License-Identifier: MIT */
/* The per-task SoftGPU context rules (src/AdrenoX2/softgpu-ctx.h): two tasks never share a context, a dead task's
 * context is destroyed (and counted free) before the next one is made, the live count is bounded (per process, with
 * slots kept for WindowServer), a reset starts every task over and no id is ever handed out twice in an epoch. The
 * same functions the kext calls (sgpu_ctx_stream_open / _desc, _task_gone, _destroy_batch) run against a model of the
 * device, and the source of softgpu.cpp and objects.cpp is checked for the calls that wire them in (a literal context
 * id in a descriptor, a free() without the hook, a quota check ahead of the flush all fail here).
 *   gcc -std=gnu11 -O1 -g -fsanitize=address,undefined -Wall -I$SNAP/compat -o test-softgpu-ctx tests/test-softgpu-ctx.c \
 *       $SNAP/compat/softgpu-guest.c
 *   ./test-softgpu-ctx [kext-dir]        ($SNAP is the Snapintosh-int superproject; kext-dir defaults to .) */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
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
static int send_fail_after = -1;        /* DESTROY_CONTEXT pushes before the queue refuses (-1: never) */
static unsigned flushes, creates;
static void device_reset(uint64_t epoch)
{
    memset(host, 0, sizeof host);
    memset(&pend, 0, sizeof pend);
    sgpu_ctx_book_reset(&book, epoch);
}
/* the epoch-checking hook of AdrenoSoftGPUCtxGone */
static void ctx_gone(uint32_t id, uint64_t epoch)
{
    if (!id || epoch != book.epoch) return;
    sgpu_ctx_pend_push(&pend, id, epoch);
}
static int send_destroy(void *cookie, uint32_t id)
{
    (void)cookie;
    if (send_fail_after == 0) return SGPU_E_QUOTA;
    if (send_fail_after > 0) send_fail_after--;
    CHECK(host_destroy(id) == 0);      /* queued descriptors reach the host */
    return 0;
}
/* sgFlushPending: the dead tasks' contexts */
static int flush_dev(void *cookie)
{
    uint32_t ids[16], n;
    (void)cookie;
    flushes++;
    for (;;) {
        n = sgpu_ctx_pend_take(&pend, book.epoch, ids, 16);
        if (!n) return 0;
        int err = 0;
        const uint32_t sent = sgpu_ctx_destroy_batch(&book, ids, n, send_destroy, NULL, &err);
        if (sent < n) { sgpu_ctx_requeue(&pend, ids + sent, n - sent, book.epoch); return 0; }
    }
}
static int create_dev(void *cookie, uint32_t id) { (void)cookie; creates++; return host_create(id); }
static const struct sgpu_ctx_ops ops = {flush_dev, create_dev, NULL};
/* The stream path of a task (what submitStream calls): the context it runs in, or the error. */
static int stream(struct sgpu_ctx_owner *task, uint32_t *ctx)
{
    struct sgpu_ctx_stream st;
    const int r = sgpu_ctx_stream_open(&ops, &book, task, &st);
    *ctx = st.ctx;
    if (!r) {                                  /* and the descriptor it builds carries exactly that context */
        struct sgpu_desc d;
        sgpu_ctx_stream_desc(&st, &d, 1, SGPU_DESC_LAST, 0, 0, 0, 0, SGPU_GUEST_NO_STAMP, 0, 0, 0);
        CHECK(d.ctx_id == st.ctx && d.ctx_id == task->id && d.type == SGPU_DESC_SUBMIT);
        CHECK(host_has(d.ctx_id));
    }
    return r;
}
static void task_dies(struct sgpu_ctx_owner *task) { sgpu_ctx_task_gone(task, ctx_gone); }
static void flush(void) { CHECK(flush_dev(NULL) == 0); }
static struct sgpu_ctx_owner mk(int32_t pid, uint32_t ws) { struct sgpu_ctx_owner o = {0, 0, pid, ws}; return o; }

/* ---- the source wiring ---- */
static char *slurp(const char *dir, const char *rel)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, rel);
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAIL cannot read %s\n", path); failures++; return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *s = calloc(1, (size_t)n + 1);
    if (fread(s, 1, (size_t)n, f) != (size_t)n) s[0] = 0;
    fclose(f);
    for (char *p = s; *p; p++) {          /* blank the comments, keep the offsets */
        if (p[0] == '/' && p[1] == '/') { while (*p && *p != '\n') *p++ = ' '; if (!*p) break; }
        else if (p[0] == '/' && p[1] == '*') {
            while (*p && !(p[0] == '*' && p[1] == '/')) { if (*p != '\n') *p = ' '; p++; }
            if (*p) { *p++ = ' '; *p = ' '; }
            if (!*p) break;
        }
    }
    return s;
}
/* the text from the first `from` up to the next `to` (or the end) */
static char *region(const char *s, const char *from, const char *to)
{
    const char *a = strstr(s, from);
    if (!a) { printf("FAIL no %s\n", from); failures++; return calloc(1, 1); }
    const char *b = to ? strstr(a + strlen(from), to) : NULL;
    size_t n = b ? (size_t)(b - a) : strlen(a);
    char *r = calloc(1, n + 1);
    memcpy(r, a, n);
    return r;
}
static unsigned count(const char *s, const char *needle)
{
    unsigned n = 0;
    for (const char *p = s; (p = strstr(p, needle)); p += strlen(needle)) n++;
    return n;
}
static void wiring(const char *dir)
{
    char *sg = slurp(dir, "src/AdrenoX2/softgpu.cpp"), *ob = slurp(dir, "src/AdrenoX2/objects.cpp");
    if (!sg || !ob) return;
    /* submitStream: the descriptor's context comes from the opened stream, never from a literal or a bare id */
    char *ss = region(sg, "int AdrenoAcceleratorX2::submitStream(", NULL);
    CHECK(count(ss, "sgpu_ctx_stream_open(") == 1);
    CHECK(count(ss, "sgpu_ctx_stream_desc(&st, &d,") == 1);
    CHECK(count(ss, "sgpu_guest_desc_submit(") == 0);
    CHECK(count(ss, "sgpu_ctx_want(") == 0);
    CHECK(strstr(ss, "sgpu_ctx_stream_open(") < strstr(ss, "sgpu_ctx_stream_desc("));
    CHECK(count(ss, "task->softgpuContext()") == 1);                     /* the task's own record, no other */
    CHECK(strstr(ss, "AdrenoDeviceGPUTask(dev)") != NULL);               /* the task of the command buffer's device */
    CHECK(strstr(ss, "sgFlushPending(static_cast<AdrenoSoftGPU *>(c))") != NULL);   /* the flush the open runs first */
    /* the free hook, with the epoch check on the other side of it */
    char *fr = region(ob, "void AdrenoTask::free()", "\n}\n");
    CHECK(count(fr, "sgpu_ctx_task_gone(&fSoftCtx, AdrenoSoftGPUCtxGone)") == 1);
    char *gone = region(sg, "extern \"C\" void AdrenoSoftGPUCtxGone(", "\n}\n");
    CHECK(strstr(gone, "epoch != __atomic_load_n(&gSoftEpoch") != NULL && strstr(gone, "sgpu_ctx_pend_push(&gSoftCtxPend") != NULL);
    /* the quota identity is recorded when the task is made */
    char *wt = region(ob, "AdrenoTask *AdrenoTask::withTask(", "\nuint64_t AdrenoTask::allocate");
    CHECK(strstr(wt, "fSoftCtx.pid = ") != NULL && strstr(wt, "fSoftCtx.ws = isWindowServer(owner)") != NULL);
    /* the batch destroy: only what was queued leaves the book, the rest goes back */
    char *fp = region(sg, "static int sgFlushPending(", "\n}\n");
    CHECK(count(fp, "sgpu_ctx_destroy_batch(&s->ctxs, ctxIds, nctx") == 1 && count(fp, "sgpu_ctx_requeue(&gSoftCtxPend, ctxIds + sent") == 1);
    CHECK(count(fp, "sgpu_ctx_destroyed(") == 0);
    free(ss); free(fr); free(gone); free(wt); free(fp); free(sg); free(ob);
}

int main(int argc, char **argv)
{
    struct sgpu_ctx_owner a = mk(100, 0), b = mk(101, 0), t[SGPU_CTX_MAX_LIVE + 2];
    uint32_t ca = 0, cb = 0, c2 = 0;
    wiring(argc > 1 ? argv[1] : ".");
    device_reset(7);
    /* two tasks, two contexts; the same task keeps its own; nothing is made twice */
    CHECK(stream(&a, &ca) == 0 && ca == 1);
    CHECK(stream(&b, &cb) == 0 && cb == 2 && cb != ca);
    CHECK(stream(&a, &c2) == 0 && c2 == ca);
    CHECK(stream(&b, &c2) == 0 && c2 == cb);
    CHECK(host_live() == 2 && book.live == 2 && creates == 2);
    /* a task can only run in the context it owns: forged records (another epoch, another id) are replaced, never trusted */
    { struct sgpu_ctx_owner forged = {7, 0, 102, 0}; CHECK(stream(&forged, &c2) == 0 && c2 != ca && c2 != cb); task_dies(&forged); flush(); }
    { struct sgpu_ctx_owner stale = {3, cb, 103, 0}; CHECK(stream(&stale, &c2) == 0 && c2 != cb && stale.epoch == 7); task_dies(&stale); flush(); }
    CHECK(host_live() == 2 && book.live == 2);
    /* a killed task: its context is destroyed by the next stream, the neighbour's stays, the id is never reused */
    task_dies(&a);
    CHECK(a.id == 0 && host_has(ca));                     /* destroyed by the flush, not by free() */
    flush();
    CHECK(!host_has(ca) && host_has(cb) && book.live == 1);
    { struct sgpu_ctx_owner fresh = mk(104, 0); CHECK(stream(&fresh, &c2) == 0 && c2 != ca && c2 != cb); task_dies(&fresh); flush(); }
    CHECK(stream(&a, &ca) == 0 && ca > cb);              /* the dead task's record is empty: a new context, a new id */
    /* the flush runs before the quota is looked at: a stream whose only free slot is a dead task's gets it at once */
    device_reset(20);
    for (unsigned i = 0; i < SGPU_CTX_MAX_LIVE - SGPU_CTX_WS_RESERVE; i++) { t[i] = mk(1000 + (int)(i / SGPU_CTX_PER_PID), 0); CHECK(stream(&t[i], &c2) == 0); }
    { struct sgpu_ctx_owner n = mk(2000, 0); CHECK(stream(&n, &c2) == SGPU_E_QUOTA); task_dies(&t[5]); CHECK(stream(&n, &c2) == 0); task_dies(&n); }
    flush();
    for (unsigned i = 0; i < SGPU_CTX_MAX_LIVE - SGPU_CTX_WS_RESERVE; i++) task_dies(&t[i]);
    flush();
    CHECK(host_live() == 0 && book.live == 0);

    /* H1: one process holds at most SGPU_CTX_PER_PID contexts, whatever it opens */
    device_reset(30);
    for (unsigned i = 0; i < SGPU_CTX_PER_PID; i++) { t[i] = mk(777, 0); CHECK(stream(&t[i], &c2) == 0); }
    t[SGPU_CTX_PER_PID] = mk(777, 0);
    CHECK(stream(&t[SGPU_CTX_PER_PID], &c2) == SGPU_E_QUOTA && t[SGPU_CTX_PER_PID].id == 0);
    CHECK(host_live() == SGPU_CTX_PER_PID && book.live == SGPU_CTX_PER_PID);
    { struct sgpu_ctx_owner other = mk(778, 0); CHECK(stream(&other, &c2) == 0); task_dies(&other); flush(); }   /* another pid is unaffected */
    CHECK(stream(&t[0], &c2) == 0);                                             /* a task that has one keeps it */
    task_dies(&t[0]);
    CHECK(stream(&t[SGPU_CTX_PER_PID], &c2) == 0);                              /* a freed slot of the same pid is usable */
    { struct sgpu_ctx_owner ws = mk(777, 1); CHECK(stream(&ws, &c2) == SGPU_E_QUOTA); }                       /* WindowServer too: the cap is per pid */
    for (unsigned i = 1; i <= SGPU_CTX_PER_PID; i++) task_dies(&t[i]);
    flush();
    CHECK(host_live() == 0 && book.live == 0);
    /* H1: an attacker (many pids, 8 each) cannot take the reserved slots; WindowServer still gets its context */
    device_reset(31);
    unsigned taken = 0;
    for (int pid = 0; pid < 8; pid++)
        for (unsigned k = 0; k < SGPU_CTX_PER_PID; k++) {
            struct sgpu_ctx_owner x = mk(500 + pid, 0);
            if (stream(&x, &c2) == 0) taken++; else CHECK(x.id == 0);
        }
    CHECK(taken == SGPU_CTX_MAX_LIVE - SGPU_CTX_WS_RESERVE && book.live == taken);
    { struct sgpu_ctx_owner x = mk(900, 0); CHECK(stream(&x, &c2) == SGPU_E_QUOTA && x.id == 0 && host_live() == taken); }
    for (unsigned i = 0; i < SGPU_CTX_WS_RESERVE; i++) {
        struct sgpu_ctx_owner ws = mk(300 + (int)i, 1);
        CHECK(stream(&ws, &c2) == 0 && ws.id == c2);
    }
    CHECK(book.live == SGPU_CTX_MAX_LIVE && host_live() == SGPU_CTX_MAX_LIVE);
    { struct sgpu_ctx_owner ws = mk(399, 1); CHECK(stream(&ws, &c2) == SGPU_E_QUOTA); }                       /* the total still holds */
    /* WindowServer's own pid cap is the same 8 */
    device_reset(32);
    for (unsigned i = 0; i < SGPU_CTX_PER_PID; i++) { struct sgpu_ctx_owner ws = mk(301, 1); CHECK(stream(&ws, &c2) == 0); }
    { struct sgpu_ctx_owner ws = mk(301, 1); CHECK(stream(&ws, &c2) == SGPU_E_QUOTA); }

    /* the bound: SGPU_CTX_MAX_LIVE at once, one more is refused, a dead task's slot is reusable after the flush */
    device_reset(8);
    memset(t, 0, sizeof t);
    uint32_t seen[SGPU_CTX_MAX_LIVE + 2];
    for (unsigned i = 0; i < SGPU_CTX_MAX_LIVE; i++) {
        const int reserved = i >= SGPU_CTX_MAX_LIVE - SGPU_CTX_WS_RESERVE;
        t[i] = mk(reserved ? 60 + (int)i : 40 + (int)(i / SGPU_CTX_PER_PID), reserved);
        CHECK(stream(&t[i], &seen[i]) == 0);
        for (unsigned j = 0; j < i; j++) CHECK(seen[i] != seen[j]);
    }
    CHECK(book.live == SGPU_CTX_MAX_LIVE && host_live() == SGPU_CTX_MAX_LIVE);
    t[SGPU_CTX_MAX_LIVE] = mk(70, 1);
    CHECK(stream(&t[SGPU_CTX_MAX_LIVE], &c2) == SGPU_E_QUOTA);
    CHECK(t[SGPU_CTX_MAX_LIVE].id == 0 && host_live() == SGPU_CTX_MAX_LIVE);      /* a refusal makes nothing */
    CHECK(stream(&t[SGPU_CTX_MAX_LIVE], &c2) == SGPU_E_QUOTA);                    /* and keeps refusing */
    CHECK(SGPU_CTX_MAX_LIVE < 64);                                                /* room under the host's own limit */
    CHECK(SGPU_CTX_WS_RESERVE > 0 && SGPU_CTX_WS_RESERVE < SGPU_CTX_MAX_LIVE);
    task_dies(&t[3]);
    flushes = 0;
    CHECK(stream(&t[SGPU_CTX_MAX_LIVE], &c2) == 0 && flushes == 1);               /* the stream's own flush freed the slot first */
    CHECK(host_live() == SGPU_CTX_MAX_LIVE && c2 > seen[SGPU_CTX_MAX_LIVE - 1]);
    for (unsigned i = 0; i < SGPU_CTX_MAX_LIVE + 1; i++) task_dies(&t[i]);
    flush();
    CHECK(host_live() == 0 && book.live == 0);
    /* close and reopen many times: nothing accumulates */
    for (unsigned round = 0; round < 500; round++) {
        struct sgpu_ctx_owner x = mk(9, 0);
        CHECK(stream(&x, &c2) == 0);
        task_dies(&x);
        flush();
    }
    CHECK(host_live() == 0 && book.live == 0);

    /* L1: a DESTROY batch the queue stops accepting halfway: the unsent contexts stay counted, pending and on the host */
    device_reset(40);
    { struct sgpu_ctx_owner x[5];
      for (int i = 0; i < 5; i++) { x[i] = mk(60 + i, 0); CHECK(stream(&x[i], &c2) == 0); }
      for (int i = 0; i < 5; i++) task_dies(&x[i]);
      send_fail_after = 2;
      flush();
      send_fail_after = -1;
      CHECK(host_live() == 3 && book.live == 3 && pend.n == 3);       /* 2 sent; 3 still there, counted, queued again */
      flush();
      CHECK(host_live() == 0 && book.live == 0 && pend.n == 0); }
    { uint32_t ids[3] = {5, 6, 7}; int err = 0; struct sgpu_ctx_pend p; memset(&p, 0, sizeof p);
      send_fail_after = 0;
      CHECK(sgpu_ctx_destroy_batch(&book, ids, 3, send_destroy, NULL, &err) == 0 && err == SGPU_E_QUOTA);
      send_fail_after = -1;
      for (unsigned i = 0; i < SGPU_CTX_PEND_MAX; i++) sgpu_ctx_pend_push(&p, 1000 + i, 1);
      CHECK(sgpu_ctx_requeue(&p, ids, 3, 1) == 3); }                  /* a full list reports what did not fit */

    /* a reset: the host forgot every context, so every task asks again and the dead tasks' pending ids are dropped */
    device_reset(9);
    CHECK(stream(&a, &ca) == 0 && stream(&b, &cb) == 0);
    task_dies(&b);
    device_reset(10);
    CHECK(pend.n == 0 && host_live() == 0 && book.live == 0);
    CHECK(stream(&a, &c2) == 0 && a.epoch == 10 && host_live() == 1 && book.live == 1);
    ctx_gone(99, 9);                                                               /* an older epoch's id is not queued */
    CHECK(pend.n == 0);
    sgpu_ctx_pend_push(&pend, 99, 9);
    { uint32_t ids[4]; CHECK(sgpu_ctx_pend_take(&pend, 10, ids, 4) == 0 && pend.n == 0); }
    /* the pending list is bounded: a full list refuses, the context waits for the next reset */
    memset(&pend, 0, sizeof pend);
    for (unsigned i = 0; i < SGPU_CTX_PEND_MAX; i++) CHECK(sgpu_ctx_pend_push(&pend, 1000 + i, 10) == 0);
    CHECK(sgpu_ctx_pend_push(&pend, 5000, 10) == SGPU_E_QUOTA && pend.n == SGPU_CTX_PEND_MAX);
    CHECK(sgpu_ctx_pend_push(&pend, 0, 10) == 0 && pend.n == SGPU_CTX_PEND_MAX);   /* no id: nothing to queue */
    { uint32_t ids[16]; unsigned total = 0, n; while ((n = sgpu_ctx_pend_take(&pend, 10, ids, 16))) total += n; CHECK(total == SGPU_CTX_PEND_MAX); }
    /* destroyed() of an id the book does not hold changes nothing and never underflows */
    device_reset(11);
    sgpu_ctx_destroyed(&book, 12345); CHECK(book.live == 0);
    { struct sgpu_ctx_owner x = mk(1, 0); uint32_t id; CHECK(stream(&x, &id) == 0 && book.live == 1); sgpu_ctx_destroyed(&book, id + 5); CHECK(book.live == 1);
      sgpu_ctx_destroyed(&book, id); sgpu_ctx_destroyed(&book, id); CHECK(book.live == 0); }
    /* the counter never wraps into ids already used or into 0 */
    device_reset(12);
    book.next = 0xfffffffeu;
    { struct sgpu_ctx_owner x = mk(1, 0); uint32_t id; int create; CHECK(sgpu_ctx_want(&book, &x, &id, &create) == 0 && id == 0xfffffffeu && create); }
    book.next = 0xffffffffu;
    { struct sgpu_ctx_owner x = mk(1, 0); uint32_t id; int create; CHECK(sgpu_ctx_want(&book, &x, &id, &create) == SGPU_E_QUOTA); }
    book.next = 0;
    { struct sgpu_ctx_owner x = mk(1, 0); uint32_t id; int create; CHECK(sgpu_ctx_want(&book, &x, &id, &create) == SGPU_E_QUOTA); }
    printf("softgpu-ctx: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
