/* SPDX-License-Identifier: MIT */
/* AdrenoDisplayEarlyDone.h - completing a layer-0 swap right after its copy (-DADRENO_PRESENT_EARLY_DONE=1, boot-arg
 * adreno_present_early_done), and the arrival measurements that decide when to. Header-only C shared by
 * AdrenoDisplay.cpp and tools/replay-adreno-latency.c. No clock and no lock here: times are the caller's (mach ticks in
 * the kext, nanoseconds in the replay); the display's work loop serializes every call.
 *
 * Why. AdrenoDisplay copies a layer-0 swap into the scanout at a frame tick and, until now, completes it only at the
 * next tick, a whole refresh after its pixels went out. WindowServer keeps at most two swaps outstanding (QuartzCore
 * 26A428 IOMFBDisplay::can_update_ waits on min(ring, 2) entries; agents/input-latency LATENCY-BUDGET.md, a code reading,
 * not a trace) and starts its next frame when one completes, so the swap on screen holds one of its two entries for
 * that refresh: each frame then has one refresh of budget from the tick that completes its predecessor. A frame that
 * needs longer (the 2026-10-24 desktop runs: 12-18% of the busy windows' ticks found no new swap, at 50-53 copies/s)
 * misses its latch and the panel shows the previous frame twice. Once the copy has returned, nothing reads the swap's
 * surfaces again (the cursor layer was captured into the display's own image at the swap, and the row signatures are
 * the display's), so the swap can be completed there, as a display engine could once it latched the frame. That frees
 * the entry a refresh earlier and a frame that runs long still makes the next latch; the cost is that WindowServer may
 * run up to one frame further ahead (one refresh more latency).
 *
 * Modes (adreno_present_early_done): 0 completes at the tick as before and only measures; 1 always completes at the
 * copy; 2 completes at the copy while WindowServer is behind: from a late frame (a layer-0 swap that arrives within one
 * refresh after a tick that had a frame on screen and neither copied nor kept a new one) for `hold`, renewed by every
 * late frame.
 *
 * The family's queue is unchanged: the display keeps treating the copied swap as on screen until the next proceeding
 * tick (AdrenoDisplay's fScreenHeld stands in for fCurrentSwap there), so arrivals between ticks queue exactly as before
 * and only the client's completion moves. */
#pragma once
#include <stdint.h>
#include <string.h>
#ifdef KERNEL
#include <libkern/libkern.h>     /* snprintf */
#else
#include <stdio.h>
#endif

enum { ADRENO_EARLY_OFF = 0, ADRENO_EARLY_ALWAYS = 1, ADRENO_EARLY_ADAPTIVE = 2 };
/* A layer-0 arrival's offset after the last proceeding tick, 2 ms bins: [0,2) ... [14,16), and 16 ms or more. */
#define ADRENO_EARLY_ARRIVAL_BINS 9u
/* A late frame's arrival after the tick it missed: < 2, < 4, < 8 ms, and 8 ms or more. */
#define ADRENO_EARLY_LATE_BINS 4u
#ifndef ADRENO_PERF_LINE_MAX
#define ADRENO_PERF_LINE_MAX 250
#endif
#ifndef ADRENO_PERF_CAP
#define ADRENO_PERF_CAP 999999999ull
#endif

struct adreno_early_done {
    uint32_t mode;                        /* ADRENO_EARLY_*; anything else is treated as OFF */
    uint64_t period, hold, ms;            /* the refresh, the adaptive hold and one millisecond, caller's units */
    uint64_t last_tick;                   /* the last proceeding tick; 0 before the first */
    uint32_t tick_missed;                 /* that tick had a frame on screen and neither copied nor kept a new one */
    uint64_t until;                       /* adaptive: engaged while now < until */
    /* this window */
    uint64_t early, late, engages, arrivals, ahead;
    uint64_t arr[ADRENO_EARLY_ARRIVAL_BINS], lat[ADRENO_EARLY_LATE_BINS];
};

static inline void adreno_early_init(struct adreno_early_done *e, uint32_t mode, uint64_t period, uint64_t hold,
                                     uint64_t ms)
{
    memset(e, 0, sizeof *e);
    e->mode = mode <= ADRENO_EARLY_ADAPTIVE ? mode : ADRENO_EARLY_OFF;
    e->period = period;
    e->hold = hold;
    e->ms = ms ? ms : 1;
}

/* Stop, restart or a mode set: no tick to measure from, nothing engaged. The mode and the counters stay. */
static inline void adreno_early_reset(struct adreno_early_done *e)
{
    e->last_tick = 0;
    e->tick_missed = 0;
    e->until = 0;
}

/* Whether a layer-0 swap copied at `now` is completed right after its copy. */
static inline int adreno_early_active(const struct adreno_early_done *e, uint64_t now)
{
    if (e->mode == ADRENO_EARLY_ALWAYS) return 1;
    return e->mode == ADRENO_EARLY_ADAPTIVE && e->until && now < e->until;
}

/* A proceeding tick at `now` (not an EARLY/LATE re-arm): had_frame, a WindowServer frame was on screen for the refresh
 * that ends here (completed at this tick, or held since an early completion); served, a layer-0 copy started in it or a
 * layer-0 head is still queued (deferred by present sync or the copy gate: the frame arrived, the GPU was late). */
static inline void adreno_early_tick(struct adreno_early_done *e, uint64_t now, int had_frame, int served)
{
    e->last_tick = now;
    e->tick_missed = had_frame && !served;
}

/* A layer-0 swap first seen pending at `now`, outside a tick. alone: it is the only pending swap (with another one
 * pending its first sight is not its arrival, and WindowServer is ahead). Returns 1 when it was a late frame. */
static inline int adreno_early_arrival(struct adreno_early_done *e, uint64_t now, int alone)
{
    if (!alone) { e->ahead++; return 0; }
    if (!e->last_tick || now < e->last_tick) return 0;
    const uint64_t d = now - e->last_tick, two = 2 * e->ms;
    uint64_t bin = d / two;
    if (bin >= ADRENO_EARLY_ARRIVAL_BINS) bin = ADRENO_EARLY_ARRIVAL_BINS - 1;
    e->arrivals++;
    e->arr[bin]++;
    const int late = e->tick_missed && d < e->period;
    e->tick_missed = 0;                   /* one late frame per missed tick */
    if (!late) return 0;
    e->late++;
    e->lat[d < 2 * e->ms ? 0 : d < 4 * e->ms ? 1 : d < 8 * e->ms ? 2 : 3]++;
    if (e->mode == ADRENO_EARLY_ADAPTIVE) {
        if (!adreno_early_active(e, now)) e->engages++;
        e->until = now + e->hold;
    }
    return 1;
}

/* The caller completed a swap at its copy. */
static inline void adreno_early_completed(struct adreno_early_done *e) { e->early++; }

static inline void adreno_early_next_window(struct adreno_early_done *e)
{
    e->early = e->late = e->engages = e->arrivals = e->ahead = 0;
    memset(e->arr, 0, sizeof e->arr);
    memset(e->lat, 0, sizeof e->lat);
}

/* The perf done line, at most ADRENO_PERF_LINE_MAX bytes at its caps (the replay checks it):
 *   AdrenoDisplay: perf done s=<seq> mode=<mode> on=<engaged now> done=<completed at the copy> late=<late frames>
 *     eng=<adaptive engagements> ahd=<arrivals behind another pending swap> arr=<9 bins> lat=<4 bins> */
static inline unsigned long long adreno_early_cap(uint64_t v)
{
    return (unsigned long long)(v < ADRENO_PERF_CAP ? v : ADRENO_PERF_CAP);
}
static inline int adreno_perf_line_done(char *b, size_t n, uint64_t seq, const struct adreno_early_done *e, int on)
{
    const uint64_t *a = e->arr, *l = e->lat;
    return snprintf(b, n, "AdrenoDisplay: perf done s=%llu mode=%u on=%d done=%llu late=%llu eng=%llu ahd=%llu "
                    "arr=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu lat=%llu/%llu/%llu/%llu",
                    adreno_early_cap(seq), e->mode <= 9 ? e->mode : 9, on ? 1 : 0, adreno_early_cap(e->early),
                    adreno_early_cap(e->late), adreno_early_cap(e->engages), adreno_early_cap(e->ahead),
                    adreno_early_cap(a[0]), adreno_early_cap(a[1]), adreno_early_cap(a[2]), adreno_early_cap(a[3]),
                    adreno_early_cap(a[4]), adreno_early_cap(a[5]), adreno_early_cap(a[6]), adreno_early_cap(a[7]),
                    adreno_early_cap(a[8]), adreno_early_cap(l[0]), adreno_early_cap(l[1]),
                    adreno_early_cap(l[2]), adreno_early_cap(l[3]));
}
