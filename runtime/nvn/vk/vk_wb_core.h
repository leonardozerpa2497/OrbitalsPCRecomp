// Write-back bookkeeping with no Vulkan in it (vk_mem.c, vk_submit.c; tests/test_wb_core.c runs it on the host):
// the ring a dispatch's write-backs live in, the plan a dispatch range gets before its shader, and the room for completion effects.
#pragma once
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// ---------- The ring ----------
// Monotonic head (the recording thread only) and tail (the completion thread publishes it when a landing is done). A landing posted with end E
// lets the ring reuse everything below E once it has run. Landings run in the order they were posted, so E must not pass any room still owed to a
// landing posted later: a plan's room is taken before its dispatch, but its landing is posted after the dispatch, possibly after other landings
// (the other ranges of the same dispatch, a copy's write-back). Such a plan is held from when its room is taken until its landing is posted
// (or it is given up), and wring_end stops at the oldest held plan.
enum { WRING_MAXHOLD = 1024 };
typedef struct {
    uint64_t size, head;
    _Atomic uint64_t tail;
    int nhold; uint64_t hold[WRING_MAXHOLD];   // the monotonic starts of held plans, ascending
} wring_t;

// Room for n bytes at an offset aligned to align (a power of two dividing size): *at. A piece does not wrap; 0 when there is no room now
static inline int wring_take(wring_t *r, uint64_t n, uint64_t align, uint64_t *at) {
    if (!n || n > r->size / 4 || !align || (align & (align - 1)) || r->size % align) return 0;
    uint64_t h = r->head, o = h % r->size, oa = (o + align - 1) & ~(align - 1);
    h += oa - o; o = oa;
    if (o + n > r->size) { h += r->size - o; o = 0; }
    if (h + n - atomic_load(&r->tail) > r->size) return 0;
    r->head = h + n; *at = o; return 1;
}
// The end a landing posted now may release: the head, or the start of the oldest held plan
static inline uint64_t wring_end(const wring_t *r) { return r->nhold && r->hold[0] < r->head ? r->hold[0] : r->head; }
static inline int wring_hold(wring_t *r, uint64_t start) {
    if (r->nhold >= WRING_MAXHOLD) return 0;
    int i = r->nhold++;
    while (i > 0 && r->hold[i - 1] > start) { r->hold[i] = r->hold[i - 1]; i--; }
    r->hold[i] = start; return 1;
}
static inline void wring_unhold(wring_t *r, uint64_t start) {
    for (int i = 0; i < r->nhold; i++) if (r->hold[i] == start) { memmove(&r->hold[i], &r->hold[i + 1], (size_t)(r->nhold - i - 1) * sizeof r->hold[0]); r->nhold--; return; }
}
// A landing is done (the completion thread): the ring may reuse what is below end. Never moves back
static inline void wring_released(wring_t *r, uint64_t end) { if (end > atomic_load(&r->tail)) atomic_store(&r->tail, end); }

// ---------- A dispatch range's plan, made before its shader ----------
// DEV: snapshot in the device ring at at (host room at at taken, unused unless wb_gpu 2 keeps a host snapshot there too), records at out, wb_gpu 2 the
// whole range at at2. GPU: the same with the snapshot in the host ring. CPU: snapshot at at, the range after the shader at after, compared by the CPU.
// NONE: no snapshot; the whole range goes back (as when the ring is full). Everything a mode needs is taken here or nothing is: a device plan that
// cannot be completed is rewound before the host plan is tried, so it never costs the host plan its room.
enum { WBP_NONE, WBP_CPU, WBP_GPU, WBP_DEV };
enum { WBF_OK, WBF_NOGPU, WBF_SHAPE, WBF_INIT, WBF_FORCED, WBF_FULL, WBF_HOLDS, WBF_NOMEM };   // why a wanted device snapshot was not taken
typedef struct {
    uint64_t len, po, align, max_range;   // range bytes, its offset in its pool buffer, minStorageBufferOffsetAlignment, maxStorageBufferRange
    int gm;                               // wb_gpu
    int dev;                              // device snapshots wanted (NXRT_VK_WB_BASE_DEVICE)
    int gpu_ok, dev_ok;                   // the comparison pipeline; the device ring (device-local)
    int every; uint64_t *seen;            // NXRT_VK_WB_BASE_DEVICE_FAIL: every n-th range that could take the device snapshot gets the host plan
} wbp_in;
typedef struct { int mode; uint64_t mark, end, at, out, at2, after; } wbp_t;   // mark..end: the ring's head before and after; at2, after: UINT64_MAX none

static inline uint64_t wbp_osz(uint64_t len) { return 16 + ((len / 4 + 63) / 64) * 272; }   // wb_diff.comp's records: header, then 68 words a block
static inline uint64_t wbp_nblk(uint64_t len) { return (len / 4 + 63) / 64; }
static inline int wbp_gpu_shape(const wbp_in *in) {
    return in->gm && in->len && !(in->len & 3) && in->align && !(in->po % in->align) && in->len <= in->max_range && wbp_osz(in->len) <= in->max_range;
}
static inline int wbp_take_gpu(wring_t *r, const wbp_in *in, wbp_t *p) {
    uint64_t al = in->align < 16 ? 16 : in->align;   // at and out are bound as storage buffers
    p->at2 = UINT64_MAX;
    return wring_take(r, (in->len + 15) & ~15ull, al, &p->at) && wring_take(r, (wbp_osz(in->len) + 15) & ~15ull, al, &p->out) &&
           (in->gm < 2 || wring_take(r, (in->len + 15) & ~15ull, 16, &p->at2));
}
static inline int wbp_plan(wring_t *r, const wbp_in *in, wbp_t *p, int *why) {
    memset(p, 0, sizeof *p); p->at2 = p->after = UINT64_MAX; *why = WBF_OK;
    if (r->nhold >= WRING_MAXHOLD) { if (in->dev) *why = WBF_HOLDS; return p->mode = WBP_NONE; }
    uint64_t mark = r->head; int shape = wbp_gpu_shape(in);
    if (in->dev) {
        if (!in->gm) *why = WBF_NOGPU;
        else if (!shape) *why = WBF_SHAPE;
        else if (!in->gpu_ok || !in->dev_ok) *why = WBF_INIT;
        else if (in->every > 0 && in->seen && ++*in->seen % (uint64_t)in->every == 0) *why = WBF_FORCED;   // before any room is taken
        else if (wbp_take_gpu(r, in, p)) { p->mode = WBP_DEV; goto ok; }
        else { r->head = mark; *why = WBF_FULL; }
    }
    if (shape && in->gpu_ok) { if (wbp_take_gpu(r, in, p)) { p->mode = WBP_GPU; goto ok; } r->head = mark; }
    p->at2 = UINT64_MAX;
    if (wring_take(r, (in->len + 15) & ~15ull, 16, &p->at) && wring_take(r, (in->len + 15) & ~15ull, 16, &p->after)) { p->mode = WBP_CPU; goto ok; }
    r->head = mark; p->after = UINT64_MAX;
    return p->mode = WBP_NONE;
ok:
    p->mark = mark; p->end = r->head; wring_hold(r, mark);   // (room for the hold checked above)
    return p->mode;
}
// A plan given up before anything was recorded into its room: its room goes back when nothing was taken after it
static inline void wbp_unrecorded(wring_t *r, const wbp_t *p) { wring_unhold(r, p->mark); if (r->head == p->end) r->head = p->mark; }
// A plan whose landing is posted now, or which is given up after its copies were recorded: no longer held. Returns the end its landing may release
static inline uint64_t wbp_release(wring_t *r, const wbp_t *p) { wring_unhold(r, p->mark); return wring_end(r); }
// The plan with what it needs beside the ring: make (the record and its post room) returns 0 when there is no memory for them. Then the plan is rewound
// and, when it was the device plan, the host plan is tried (note: why the device plan was not taken). The mode, WBP_NONE: nothing is kept
typedef int (*wbp_make_fn)(void *ctx, const wbp_t *p);
static inline int wbp_prepare(wring_t *r, wbp_in in, wbp_t *p, wbp_make_fn make, void *ctx, void (*note)(int why)) {
    for (;;) {
        int why, mode = wbp_plan(r, &in, p, &why);
        if (why && note) note(why);
        if (mode == WBP_NONE) return WBP_NONE;
        if (make(ctx, p)) return mode;
        wbp_unrecorded(r, p);
        if (mode != WBP_DEV) return WBP_NONE;
        if (note) note(WBF_NOMEM);
        in.dev = 0;
    }
}

// ---------- Room for completion effects ----------
// A queue of n entries of esz bytes; reserved entries are room set aside before a dispatch, so posting its landings afterwards cannot fail
typedef struct { void *v; size_t n, cap, reserved, esz; } postq_t;
static inline int postq_room(postq_t *q, size_t extra) {
    size_t need = q->n + q->reserved + extra; if (need <= q->cap) return 1;
    size_t c = q->cap ? q->cap : 16; while (c < need) c *= 2;
    void *nv = realloc(q->v, c * q->esz); if (!nv) return 0;
    q->v = nv; q->cap = c; return 1;
}
static inline int postq_reserve(postq_t *q) { if (!postq_room(q, 1)) return 0; q->reserved++; return 1; }
static inline void postq_unreserve(postq_t *q) { if (q->reserved) q->reserved--; }
// The next entry: from the reserved room when reserved (never fails then), else new room (NULL: no memory)
static inline void *postq_push(postq_t *q, int reserved) {
    if (reserved && q->reserved) q->reserved--;
    else if (!postq_room(q, 1)) return NULL;
    return (char *)q->v + q->n++ * q->esz;
}
// The next entry or never: an effect with no room must not let its submission complete without it (its event, counter or landing silently lost while
// the submission's value is published). fail reports and ends the run; it is called before anything is added and must not return (a host test's
// hook may: NULL then, with the queue unchanged)
static inline void *postq_push_must(postq_t *q, int reserved, void (*fail)(const postq_t *q)) {
    void *d = postq_push(q, reserved);
    if (!d) fail(q);
    return d;
}
static inline void postq_reset(postq_t *q) { q->n = 0; q->reserved = 0; }
