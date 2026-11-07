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
static unsigned flushes, creates, locks, unlocks, sends;
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
    sends++;
    if (send_fail_after == 0) return SGPU_E_QUOTA;
    if (send_fail_after > 0) send_fail_after--;
    CHECK(host_destroy(id) == 0);      /* queued descriptors reach the host */
    return 0;
}
/* sgFlushPending: the dead tasks' contexts, through the same helpers the kext calls */
static int flush_err;                   /* the flush itself fails with this (a lost device) */
static void pend_lock(void *c) { (void)c; locks++; }
static void pend_unlock(void *c) { (void)c; unlocks++; }
static int flush_dev(void *cookie)
{
    uint32_t ids[16];
    int stuck = 0;
    (void)cookie;
    flushes++;
    if (flush_err) return flush_err;
    for (unsigned pass = 0;; pass++) {
        if (pass > 50) { CHECK(!"the flush spins on a queue that keeps refusing"); return -1; }
        const uint32_t n = sgpu_ctx_flush_take(&pend, &book, ids, 16);
        const int more = pend.n != 0;
        if (n) {
            const struct sgpu_ctx_flush_ops fops = {send_destroy, pend_lock, pend_unlock, NULL};
            int err = 0;
            uint32_t kept = 0;
            if (sgpu_ctx_flush_batch(&book, &pend, ids, n, &fops, &err, &kept)) stuck = 1;
        }
        if (!sgpu_ctx_flush_again(more, stuck)) return 0;
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
static struct sgpu_ctx_owner mk(int32_t pid, uint32_t ws) { struct sgpu_ctx_owner o; sgpu_ctx_owner_init(&o, pid, (int)ws); return o; }

/* ---- the source wiring ----
 * What cannot run here is the kext's own glue (it needs XNU). Its logic sits in softgpu-ctx.h and is driven above; what is
 * left in softgpu.cpp and objects.cpp is checked as whole statements (comments dropped, white space collapsed), so an edit
 * that keeps a substring but changes an argument, an operand or a condition no longer matches. */
static char *slurp(const char *dir, const char *rel)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, rel);
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAIL cannot read %s\n", path); failures++; return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *s = calloc(1, (size_t)n + 1), *o = calloc(1, (size_t)n + 1);
    if (fread(s, 1, (size_t)n, f) != (size_t)n) s[0] = 0;
    fclose(f);
    size_t w = 0;
    int space = 0;
    for (const char *p = s; *p; p++) {         /* drop comments, collapse white space */
        if (p[0] == '/' && p[1] == '/') { while (p[1] && p[1] != '\n') p++; space = 1; continue; }
        if (p[0] == '/' && p[1] == '*') { p += 2; while (*p && !(p[0] == '*' && p[1] == '/')) p++; if (*p) p++; space = 1; continue; }
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') { space = 1; continue; }
        if (space && w) o[w++] = ' ';
        space = 0;
        o[w++] = *p;
    }
    free(s);
    return o;
}
/* the braced body that follows the first `from` (balanced), or "" */
static char *body(const char *s, const char *from)
{
    const char *a = strstr(s, from);
    if (!a) { printf("FAIL no %s\n", from); failures++; return calloc(1, 1); }
    const char *b = strchr(a + strlen(from), '{');
    int depth = 0;
    const char *e = b;
    for (; e && *e; e++) {
        if (*e == '{') depth++;
        else if (*e == '}' && --depth == 0) break;
    }
    if (!b || !e || !*e) { printf("FAIL no body for %s\n", from); failures++; return calloc(1, 1); }
    char *r = calloc(1, (size_t)(e - b) + 2);
    memcpy(r, b, (size_t)(e - b) + 1);
    return r;
}
static int idc(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
/* occurrences of `needle` that start and end on identifier boundaries (a needle ending in punctuation needs none there) */
static unsigned count(const char *s, const char *needle)
{
    unsigned n = 0;
    const size_t l = strlen(needle);
    for (const char *p = s; (p = strstr(p, needle)); p += l) {
        if (idc(needle[0]) && p != s && idc(p[-1])) continue;
        if (idc(needle[l - 1]) && idc(p[l])) continue;
        n++;
    }
    return n;
}
static void once(const char *where, const char *text, const char *stmt)
{
    checks++;
    if (count(text, stmt) != 1) { failures++; printf("FAIL %s: expected exactly once: %s\n", where, stmt); }
}
static void never(const char *where, const char *text, const char *needle)
{
    checks++;
    if (count(text, needle)) { failures++; printf("FAIL %s: must not appear: %s\n", where, needle); }
}
static void exactly(const char *where, const char *text, const char *needle, unsigned n)
{
    checks++;
    if (count(text, needle) != n) { failures++; printf("FAIL %s: %s must appear %u times\n", where, needle, n); }
}
static void wiring(const char *dir)
{
    char *sg = slurp(dir, "src/AdrenoX2/softgpu.cpp"), *ob = slurp(dir, "src/AdrenoX2/objects.cpp");
    if (!sg || !ob) return;
    /* submitStream: the descriptor's context comes from the opened stream, never from a literal or a bare id */
    char *ss = body(sg, "int AdrenoAcceleratorX2::submitStream(");
    once("submitStream", ss, "struct sgpu_ctx_stream st = {};");
    once("submitStream", ss, "struct sgpu_ctx_ops ops = { [](void *c) { return sgFlushPending(static_cast<AdrenoSoftGPU *>(c)); }, "
                             "[](void *c, uint32_t id) { return sgCreateContext(static_cast<AdrenoSoftGPU *>(c), id); }, s};");
    once("submitStream", ss, "result = sgpu_ctx_stream_open(&ops, &s->ctxs, task->softgpuContext(), &st);");
    once("submitStream", ss, "sgpu_ctx_stream_desc(&st, &d, ++s->cbSerial, SGPU_DESC_LAST, off, sbytes, off + sbytes, (uint32_t)count, "
                             "SGPU_GUEST_NO_STAMP, 0, 0, (uint64_t)(uintptr_t)list);");
    once("submitStream", ss, "AdrenoDeviceGPUTask(dev)");                    /* the task of the command buffer's device */
    exactly("submitStream", ss, "&st", 2);                                    /* the open and the descriptor, nothing else */
    never("submitStream", ss, "st.");                                         /* the stream's context is never assigned */
    exactly("submitStream", ss, "st =", 1);                                   /* only its declaration */
    never("submitStream", ss, "ctx_id");
    never("submitStream", ss, "sgpu_guest_desc_submit");
    never("submitStream", ss, "sgpu_ctx_want");
    never("submitStream", ss, "sgpu_ctx_made");
    exactly("submitStream", ss, "task->softgpuContext()", 1);                 /* the task's own record, no other */
    CHECK(strstr(ss, "sgpu_ctx_stream_open(") < strstr(ss, "sgpu_ctx_stream_desc("));
    /* the free hook, with the epoch check on the other side of it */
    char *fr = body(ob, "void AdrenoTask::free()");
    once("free", fr, "sgpu_ctx_task_gone(&fSoftCtx, AdrenoSoftGPUCtxGone);");
    char *gone = body(sg, "extern \"C\" void AdrenoSoftGPUCtxGone(");
    once("CtxGone", gone, "if (!lock || !id || epoch != __atomic_load_n(&gSoftEpoch, __ATOMIC_RELAXED)) return;");
    once("CtxGone", gone, "const int r = sgpu_ctx_pend_push(&gSoftCtxPend, id, epoch);");
    /* the quota identity is recorded once, when the task is made, from the opener's pid and entitlements */
    char *wt = body(ob, "AdrenoTask *AdrenoTask::withTask(");
    once("withTask", wt, "sgpu_ctx_owner_init(&t->fSoftCtx, owner == kernel_task ? 0 : taskPid(owner), isWindowServer(owner));");
    char *tp = body(ob, "static int32_t taskPid(task_t owner)");
    CHECK(strcmp(tp, "{ return sgpu_ctx_proc_pid(get_bsdtask_info(owner), proc_pid); }") == 0);
    never("objects.cpp", ob, "fSoftCtx.");                                   /* no other write to the record */
    exactly("objects.cpp", ob, "fSoftCtx", 2);                                /* its init and the free hook */
    /* the drain: every context step goes through the tested helpers, in the book's own epoch */
    char *fp = body(sg, "static int sgFlushPending(");
    once("sgFlushPending", fp, "nctx = sgpu_ctx_flush_take(&gSoftCtxPend, &s->ctxs, ctxIds, 16);");
    once("sgFlushPending", fp, "struct Cookie { AdrenoSoftGPU *s; IOSimpleLock *lock; } cookie = {s, lock};");
    once("sgFlushPending", fp, "[](void *c) { IOSimpleLockLock(static_cast<Cookie *>(c)->lock); }, "
                               "[](void *c) { IOSimpleLockUnlock(static_cast<Cookie *>(c)->lock); }, &cookie};");
    once("sgFlushPending", fp, "if (sgpu_ctx_flush_batch(&s->ctxs, &gSoftCtxPend, ctxIds, nctx, &fops, &r, &kept)) stuck = true;");
    once("sgFlushPending", fp, "if (!sgpu_ctx_flush_again(more, stuck)) return 0;");
    once("sgFlushPending", fp, "bool stuck = false;");
    exactly("sgFlushPending", fp, "stuck", 3);                                /* declared, set, passed on: no more, no fewer */
    exactly("sgFlushPending", fp, "s->epoch", 1);                             /* the memory ids' own filter */
    never("sgFlushPending", fp, "sgpu_ctx_destroy_batch");
    never("sgFlushPending", fp, "sgpu_ctx_requeue");
    never("sgFlushPending", fp, "sgpu_ctx_pend_take");
    never("sgFlushPending", fp, "sgpu_ctx_destroyed");
    /* the self-test's own context leaves through sgpu_ctx_retire, whatever the destroy did */
    once("selftest", sg, "const int lost = sgpu_ctx_retire(&s->ctxs, &owner, sent, done, AdrenoSoftGPUCtxGone);");
    once("selftest", sg, "if (lost) r = lost;");
    never("softgpu.cpp", sg, "sgpu_ctx_destroyed");
    never("softgpu.cpp", sg, "sgpu_ctx_owner_take");
    free(ss); free(fr); free(gone); free(wt); free(tp); free(fp); free(sg); free(ob);
}
static int fake_pid_of(void *p) { return *(int *)p; }
/* ---- behaviour of the new helpers ---- */
static void helpers(void)
{
    /* sgpu_ctx_owner_init overwrites a dirty record: no id, no epoch, the pid, ws as 0 or 1 */
    struct sgpu_ctx_owner o = {99, 5, -7, 3};
    sgpu_ctx_owner_init(&o, 4242, 0);
    CHECK(o.epoch == 0 && o.id == 0 && o.pid == 4242 && o.ws == 0);
    sgpu_ctx_owner_init(&o, 0, 17);
    CHECK(o.pid == 0 && o.ws == 1);
    sgpu_ctx_owner_init(&o, -1, 1);
    CHECK(o.pid == -1 && o.ws == 1);
    { int pid = 321; CHECK(sgpu_ctx_proc_pid(&pid, fake_pid_of) == 321); CHECK(sgpu_ctx_proc_pid(NULL, fake_pid_of) == -1); }
    /* the recorded identity is what the quota uses: the pid cap and the WindowServer reserve */
    device_reset(60);
    { struct sgpu_ctx_owner t2[SGPU_CTX_PER_PID + 1]; uint32_t c;
      for (unsigned i = 0; i <= SGPU_CTX_PER_PID; i++) sgpu_ctx_owner_init(&t2[i], 4242, 0);
      for (unsigned i = 0; i < SGPU_CTX_PER_PID; i++) CHECK(stream(&t2[i], &c) == 0);
      CHECK(stream(&t2[SGPU_CTX_PER_PID], &c) == SGPU_E_QUOTA);                 /* the same pid shares one cap */
      for (unsigned i = 0; i < SGPU_CTX_PER_PID; i++) task_dies(&t2[i]);
      flush(); }
    device_reset(61);
    { struct sgpu_ctx_owner junk[SGPU_CTX_MAX_LIVE]; uint32_t c; unsigned k = 0;
      for (int pid = 0; k < SGPU_CTX_MAX_LIVE - SGPU_CTX_WS_RESERVE; pid++)
          for (unsigned j = 0; j < SGPU_CTX_PER_PID && k < SGPU_CTX_MAX_LIVE - SGPU_CTX_WS_RESERVE; j++, k++) {
              sgpu_ctx_owner_init(&junk[k], 800 + pid, 0);
              CHECK(stream(&junk[k], &c) == 0);
          }
      struct sgpu_ctx_owner plain, ws;
      sgpu_ctx_owner_init(&plain, 900, 0);
      sgpu_ctx_owner_init(&ws, 901, 1);
      CHECK(stream(&plain, &c) == SGPU_E_QUOTA);                                /* the reserve is not for it ... */
      CHECK(stream(&ws, &c) == 0);                                              /* ... but for WindowServer */
      sgpu_ctx_owner_init(&ws, 902, 99);
      CHECK(ws.ws == 1 && stream(&ws, &c) == 0);                                /* any non-zero flag is WindowServer's */
      for (unsigned i = 0; i < k; i++) task_dies(&junk[i]);
      flush(); }
    /* the flush pass: what was not sent goes back in the BOOK's epoch (so the next take finds it), under the lock, and the
     * pass reports it; a clean pass touches no lock */
    device_reset(70);
    { uint32_t ids[16], c, n; struct sgpu_ctx_owner x[5];
      for (int i = 0; i < 5; i++) { sgpu_ctx_owner_init(&x[i], 50 + i, 0); CHECK(stream(&x[i], &c) == 0); }
      for (int i = 0; i < 5; i++) task_dies(&x[i]);
      CHECK(pend.n == 5);
      pend.e[0].epoch = 69;                                                     /* an older epoch's id is not taken */
      locks = unlocks = 0;
      n = sgpu_ctx_flush_take(&pend, &book, ids, 16);
      CHECK(n == 4 && pend.n == 0);
      send_fail_after = 1;
      { const struct sgpu_ctx_flush_ops fops = {send_destroy, pend_lock, pend_unlock, NULL}; int err = 0; uint32_t kept = 9;
        CHECK(sgpu_ctx_flush_batch(&book, &pend, ids, n, &fops, &err, &kept) == 1);
        CHECK(err == SGPU_E_QUOTA && kept == 0 && pend.n == 3 && locks == 1 && unlocks == 1);
        for (uint32_t i = 0; i < pend.n; i++) CHECK(pend.e[i].epoch == book.epoch);
        CHECK(sgpu_ctx_flush_take(&pend, &book, ids, 16) == 3);                 /* still takeable: not dropped as stale */
        locks = unlocks = 0;
        send_fail_after = -1;
        CHECK(sgpu_ctx_flush_batch(&book, &pend, ids, 3, &fops, &err, &kept) == 0 && err == 0 && kept == 0);
        CHECK(locks == 0 && unlocks == 0 && pend.n == 0); }
      device_reset(71); }
    /* a full pending list reports what it could not take back */
    { uint32_t ids[3] = {5, 6, 7}, kept = 0; int err = 0; struct sgpu_ctx_pend p; memset(&p, 0, sizeof p);
      for (unsigned i = 0; i < SGPU_CTX_PEND_MAX; i++) sgpu_ctx_pend_push(&p, 1000 + i, book.epoch);
      const struct sgpu_ctx_flush_ops fops = {send_destroy, pend_lock, pend_unlock, NULL};
      send_fail_after = 0;
      CHECK(sgpu_ctx_flush_batch(&book, &p, ids, 3, &fops, &err, &kept) == 1 && kept == 3);
      send_fail_after = -1; }
    /* the drain loop goes round again only when more is pending and the last pass was not cut short */
    CHECK(sgpu_ctx_flush_again(1, 0) == 1);
    CHECK(sgpu_ctx_flush_again(1, 1) == 0);
    CHECK(sgpu_ctx_flush_again(0, 0) == 0);
    CHECK(sgpu_ctx_flush_again(0, 1) == 0);
    /* more dead contexts than one pass takes, and a queue that refuses: the flush ends, nothing is lost or counted free */
    device_reset(72);
    { struct sgpu_ctx_owner x[20]; uint32_t c; unsigned k = 0;
      for (int pid = 0; k < 20; pid++)
          for (unsigned j = 0; j < 4 && k < 20; j++, k++) { sgpu_ctx_owner_init(&x[k], 3000 + pid, 0); CHECK(stream(&x[k], &c) == 0); }
      for (unsigned i = 0; i < 20; i++) task_dies(&x[i]);
      send_fail_after = 0;
      sends = 0;
      flush();
      send_fail_after = -1;
      CHECK(sends == 1 && host_live() == 20 && book.live == 20 && pend.n == 20);
      flush();                                                                  /* the queue is back: everything goes */
      CHECK(host_live() == 0 && book.live == 0 && pend.n == 0); }
    /* a flush that fails (a lost device) ends the stream: no context is made, nothing is recorded */
    device_reset(73);
    { struct sgpu_ctx_owner x = mk(5, 0); uint32_t c = 77; const unsigned c0 = creates;
      flush_err = SGPU_E_LOST;
      CHECK(stream(&x, &c) == SGPU_E_LOST && c == 0 && x.id == 0 && creates == c0 && host_live() == 0 && book.live == 0);
      flush_err = SGPU_E_TIMEOUT;
      CHECK(stream(&x, &c) == SGPU_E_TIMEOUT && x.id == 0 && creates == c0);
      flush_err = 0;
      CHECK(stream(&x, &c) == 0 && x.id == c && creates == c0 + 1);
      task_dies(&x); flush(); }
    /* N3: the context the kext made for itself (the self-test) never stays counted, whichever way its destroy ends */
    device_reset(80);
    for (int variant = 0; variant < 5; variant++) {
        struct sgpu_ctx_owner st = mk(0, 1);
        uint32_t id = 0;
        int create = 0;
        CHECK(sgpu_ctx_want(&book, &st, &id, &create) == 0 && create && host_create(id) == 0);
        sgpu_ctx_made(&book, &st, id);
        CHECK(book.live == 1 && st.id == id);
        const int sent_err = variant == 1 ? SGPU_E_QUOTA : variant == 4 ? SGPU_E_TIMEOUT : 0;
        const int done_err = variant == 2 ? SGPU_E_OBJECT : variant == 3 ? SGPU_E_LOST : 0;
        if (!sent_err) host_destroy(id);                                          /* the push went out */
        const int lost = sgpu_ctx_retire(&book, &st, sent_err, done_err, ctx_gone);
        CHECK(st.id == 0 && st.epoch == 0);
        if (variant == 3 || variant == 4) {                                       /* lost or timed out: the caller resets */
            CHECK(lost == (variant == 3 ? SGPU_E_LOST : SGPU_E_TIMEOUT) && pend.n == 0);
            device_reset(81 + (uint64_t)variant);
        } else if (variant == 1) {                                                /* never pushed: queued, the next flush sends it */
            CHECK(lost == 0 && pend.n == 1 && book.live == 1 && host_has(id));
            flush();
        } else {                                                                  /* destroyed, or refused by the host */
            CHECK(lost == 0 && pend.n == 0);
        }
        CHECK(book.live == 0 && pend.n == 0);
        host_destroy(id);
    }
    { struct sgpu_ctx_owner none = mk(0, 1); CHECK(sgpu_ctx_retire(&book, &none, 0, 0, ctx_gone) == 0 && book.live == 0); }
    /* the AIR a process can pin in the host's store: its contexts times the per-context cap the host enforces */
    CHECK(SGPU_CTX_AIR_PID_CAP == (64u << 20) && SGPU_CTX_AIR_CAP == (8u << 20));
    CHECK((uint64_t)SGPU_CTX_PER_PID * SGPU_CTX_AIR_CAP <= SGPU_CTX_AIR_PID_CAP);
    CHECK((uint64_t)SGPU_CTX_AIR_PID_CAP < (256u << 20));                         /* a process never takes the whole store */
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
    helpers();
    printf("softgpu-ctx: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
