// Host tests of the write-back bookkeeping (runtime/nvn/vk/vk_wb_core.h): the ring's reclamation with plans held between a dispatch's snapshot and
// its landing, transactional plans (device -> host, forced fallback, no memory), and the post room reserved before a dispatch.
//   cc -O1 -g -fsanitize=address,undefined -Iruntime/nvn/vk tests/test_wb_core.c -o /tmp/wbcore && /tmp/wbcore
// The landing model below mirrors vk_mem.c: a record is posted with ring_end = wbp_release (planned) or wring_end (whole range through the ring),
// records land in post order (vk_submit.c's completion thread), and a landing calls wring_released(ring_end) after reading its room.
#include <stdio.h>
#include <stdlib.h>
static int g_fail_realloc;   // the next realloc calls fail
static void *test_realloc(void *p, size_t n) { if (g_fail_realloc) return NULL; return realloc(p, n); }
#define realloc test_realloc
#include "vk_wb_core.h"
#undef realloc

static int g_failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_failures++; } } while (0)

static void ring_init(wring_t *r, uint64_t size) { memset(r, 0, sizeof *r); r->size = size; }
static void ring_copy(wring_t *d, const wring_t *s) {
    d->size = s->size; d->head = s->head; atomic_store(&d->tail, atomic_load(&s->tail)); d->nhold = s->nhold; memcpy(d->hold, s->hold, sizeof d->hold);
}

// ---------- The landing model ----------
typedef struct { uint64_t at, n; } piece;
typedef struct { piece p[4]; int np; uint64_t ring_end; int id; } rec;
enum { MAXREC = 4096 };
static rec g_q[MAXREC]; static int g_qh, g_qt;      // posted, not landed (FIFO)
static rec g_open[MAXREC]; static int g_nopen;      // planned, not posted
static int g_overlaps;

static int phys_overlap(piece a, piece b) { return a.at < b.at + b.n && b.at < a.at + a.n; }
// A piece the producer just took must not overlap any room still owed to a record not landed yet (posted or only planned)
static void check_take(piece t) {
    for (int i = g_qh; i < g_qt; i++) for (int k = 0; k < g_q[i].np; k++) if (phys_overlap(t, g_q[i].p[k])) g_overlaps++;
    for (int i = 0; i < g_nopen; i++) for (int k = 0; k < g_open[i].np; k++) if (phys_overlap(t, g_open[i].p[k])) g_overlaps++;
}
static piece pc(uint64_t at, uint64_t n) { return (piece){at, (n + 15) & ~15ull}; }
static rec plan_rec(const wbp_t *p, uint64_t len) {
    rec r = {0};
    r.p[r.np++] = pc(p->at, len);
    if (p->mode == WBP_CPU) r.p[r.np++] = pc(p->after, len);
    else { r.p[r.np++] = pc(p->out, wbp_osz(len)); if (p->at2 != UINT64_MAX) r.p[r.np++] = pc(p->at2, len); }
    return r;
}
static void model_reset(void) { g_qh = g_qt = g_nopen = 0; g_overlaps = 0; }
static void land_one(wring_t *r) { if (g_qh < g_qt) wring_released(r, g_q[g_qh++].ring_end); }
static void land_all(wring_t *r) { while (g_qh < g_qt) land_one(r); }
// A copy's whole-range write-back through the ring (gpu_wrote kind 4): taken and posted at once
static int post_whole(wring_t *r, uint64_t len, int old_rule) {
    uint64_t at; if (!wring_take(r, (len + 15) & ~15ull, 16, &at)) return 0;
    check_take(pc(at, len));
    rec x = {0}; x.p[x.np++] = pc(at, len); x.ring_end = old_rule ? r->head : wring_end(r);
    g_q[g_qt++] = x; return 1;
}
static int plan_open(wring_t *r, const wbp_in *in, wbp_t *p) {
    int why, m = wbp_plan(r, in, p, &why);
    if (m == WBP_NONE) return 0;
    rec x = plan_rec(p, in->len); for (int k = 0; k < x.np; k++) check_take(x.p[k]);
    x.id = (int)p->mark; g_open[g_nopen++] = x; return 1;
}
static void open_remove(uint64_t mark) { for (int i = 0; i < g_nopen; i++) if (g_open[i].id == (int)mark) { g_open[i] = g_open[--g_nopen]; return; } }
// old_rule: the end the pre-fix code gave a landing (the head when it was posted, plans held or not)
static void post_planned(wring_t *r, const wbp_t *p, uint64_t len, int old_rule) {
    open_remove(p->mark);
    rec x = plan_rec(p, len);
    uint64_t e = wbp_release(r, p); x.ring_end = old_rule ? r->head : e;
    g_q[g_qt++] = x;
}
static wbp_in in_dev(uint64_t len, int gm) { return (wbp_in){len, 0, 16, UINT64_MAX >> 1, gm, 1, 1, 1, 0, NULL}; }

// P1 1: two 256-byte ranges planned before one dispatch, posted in order. The first landing must not release the second's room
static void test_two_ranges(int old_rule) {
    wring_t r; ring_init(&r, 4096); model_reset();
    wbp_in in = in_dev(256, 1); wbp_t a, b;
    CHECK(plan_open(&r, &in, &a) && a.mode == WBP_DEV);
    CHECK(plan_open(&r, &in, &b) && b.mode == WBP_DEV);
    CHECK(a.at == 0 && a.out == 256 && b.at == 544 && b.out == 800 && r.head == 1088);   // the review's numbers
    post_planned(&r, &a, 256, old_rule);
    post_planned(&r, &b, 256, old_rule);
    if (!old_rule) CHECK(g_q[0].ring_end == 544);   // stops at B's room
    land_one(&r);   // A done; B not yet (it lands after the producer goes on)
    int taken = 0; while (post_whole(&r, 1024, old_rule)) taken++;   // the producer goes on and wraps
    if (old_rule) CHECK(g_overlaps > 0);   // the pre-fix rule hands B's records out again before B lands
    else CHECK(g_overlaps == 0);
    land_all(&r);
    if (!old_rule) { CHECK(r.nhold == 0); CHECK(atomic_load(&r.tail) == r.head); }
}

// Mixed host and device plans, whole ranges through the ring between them, plans given up and planned again, landings at random points
static uint64_t g_rng = 88172645463325252ull;
static uint64_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return g_rng; }
static void test_random(int old_rule) {
    wring_t r; ring_init(&r, 1 << 18); model_reset(); g_rng = 88172645463325252ull;
    uint64_t seen = 0; int modes[4] = {0};
    for (int iter = 0; iter < 200000; iter++) {
        // one dispatch: up to 6 ranges planned, then posted in order with whole ranges among them
        wbp_t p[6]; uint64_t len[6]; int np = 0, n = 1 + (int)(rnd() % 6);
        for (int i = 0; i < n; i++) {
            uint64_t l = 4 + (rnd() % 12000); if (rnd() % 4) l &= ~3ull;   // some not word sized: the CPU plan
            wbp_in in = {l, (rnd() % 8) * 4, 16, UINT64_MAX >> 1, (int)(rnd() % 3), (int)(rnd() % 2), (int)(rnd() % 8 != 0), (int)(rnd() % 8 != 0), 3, &seen};
            if (plan_open(&r, &in, &p[np])) { modes[p[np].mode]++; len[np++] = l; } else modes[WBP_NONE]++;
            if (rnd() % 16 == 0) land_one(&r);   // the completion thread runs while the dispatch is recorded
        }
        if (np && rnd() % 32 == 0) { open_remove(p[0].mark); wbp_release(&r, &p[0]); memmove(p, p + 1, --np * sizeof *p); memmove(len, len + 1, np * sizeof *len); }   // given up (planned again later)
        for (int i = 0; i < np; i++) {
            if (rnd() % 4 == 0) post_whole(&r, 16 + rnd() % 2048, old_rule);
            post_planned(&r, &p[i], len[i], old_rule);
            if (rnd() % 3 == 0) land_one(&r);
        }
        if (g_qt - g_qh > 64 || g_qt > MAXREC - 64) land_all(&r);
        if (g_qt > MAXREC - 64) g_qh = g_qt = 0;
    }
    land_all(&r);
    printf("  %s: modes none %d cpu %d gpu %d dev %d, overlaps %d, holds left %d\n", old_rule ? "old rule" : "held plans", modes[0], modes[1], modes[2], modes[3], g_overlaps, r.nhold);
    if (old_rule) { CHECK(g_overlaps > 0); return; }
    CHECK(g_overlaps == 0); CHECK(r.nhold == 0); CHECK(atomic_load(&r.tail) == r.head);
    CHECK(modes[WBP_NONE] && modes[WBP_CPU] && modes[WBP_GPU] && modes[WBP_DEV]);
}

// P1 3: a device plan that cannot be completed, or is forced to the host, takes no room: the host plan gets the room it would have had alone
static void test_transactional(void) {
    uint64_t len = 32u << 20, osz = (wbp_osz(len) + 15) & ~15ull;
    // a 256 MiB ring with room for the snapshot and the records only (filled in pieces: at most a quarter at once)
    wring_t r; ring_init(&r, 256u << 20); uint64_t at, fill = (256u << 20) - len - osz;
    while (fill) { uint64_t n = fill > (64u << 20) ? (64u << 20) : fill; CHECK(wring_take(&r, n, 16, &at)); fill -= n; }
    uint64_t h0 = r.head; wbp_t p; int why;
    wbp_in in = {len, 0, 16, UINT64_MAX >> 1, 2, 1, 1, 1, 0, NULL};
    // wb_gpu 2: device and host GPU plans need len + osz + len and do not fit; the CPU's comparison (2 len) does, from the same start: the device
    // plan's partial room was given back (before the fix it kept the snapshot and the records, and the range went back whole)
    CHECK(wbp_plan(&r, &in, &p, &why) == WBP_CPU && why == WBF_FULL && p.at == h0 % r.size && p.after == p.at + len && r.nhold == 1);
    wbp_unrecorded(&r, &p); CHECK(r.head == h0 && r.nhold == 0);
    in.len = len + (2u << 20);   // a little more: nothing fits, nothing is kept
    CHECK(wbp_plan(&r, &in, &p, &why) == WBP_NONE && why == WBF_FULL && r.head == h0 && r.nhold == 0);
    in.len = len;
    // wb_gpu 1: the device plan fits exactly
    in.gm = 1; CHECK(wbp_plan(&r, &in, &p, &why) == WBP_DEV && why == WBF_OK && p.at == h0 % r.size && r.nhold == 1);
    wbp_unrecorded(&r, &p); CHECK(r.head == h0 && r.nhold == 0);
    // forced to the host before any room is taken: the host GPU plan gets the very same room
    uint64_t seen = 0; in.every = 1; in.seen = &seen;
    CHECK(wbp_plan(&r, &in, &p, &why) == WBP_GPU && why == WBF_FORCED && p.at == h0 % r.size && seen == 1);
    wbp_unrecorded(&r, &p);
    // the device ring missing: the host plan, same room
    in.every = 0; in.dev_ok = 0;
    CHECK(wbp_plan(&r, &in, &p, &why) == WBP_GPU && why == WBF_INIT && p.at == h0 % r.size);
    wbp_unrecorded(&r, &p);
    // property: whatever stops the device plan, the plan is never worse than the host plan alone
    for (int k = 0; k < 20000; k++) {
        wring_t a, b; ring_init(&a, 1 << 16);
        uint64_t pre = rnd() % 60000; while (pre) { uint64_t n = pre > 16000 ? 16000 : pre; wring_take(&a, n, 16, &at); pre -= n; }
        atomic_store(&a.tail, a.head > (1 << 16) ? a.head - (rnd() % (1 << 16)) : rnd() % (a.head + 1));
        ring_copy(&b, &a);
        uint64_t l = 4 + rnd() % 16000; if (rnd() % 4) l &= ~3ull; uint64_t s1 = 0, s2 = 0;
        wbp_in host = {l, (rnd() % 4) * 16, 16 << (rnd() % 3), UINT64_MAX >> 1, (int)(rnd() % 3), 0, (int)(rnd() % 4 != 0), (int)(rnd() % 2), 0, &s1};
        wbp_in dev = host; dev.dev = 1; dev.every = 1 + (int)(rnd() % 3); dev.seen = &s2;
        wbp_t ph, pd; int w1, w2;
        int mh = wbp_plan(&a, &host, &ph, &w1), md = wbp_plan(&b, &dev, &pd, &w2);
        if (mh != WBP_NONE) CHECK(md != WBP_NONE);
        if (md != WBP_DEV) { CHECK(md == mh); if (md != WBP_NONE) CHECK(pd.at == ph.at && pd.end == ph.end); }
        if (md == WBP_NONE) CHECK(b.head == a.head && b.nhold == 0);
    }
}

// P1 2: the record and post room come before the snapshot; no memory for a device plan's record gives the host plan; for the host plan nothing
typedef struct { int fail_left, made; } mk;
static int make_fn(void *ctx, const wbp_t *p) { mk *m = ctx; (void)p; if (m->fail_left > 0) { m->fail_left--; return 0; } m->made++; return 1; }
static int g_notes[8]; static void note_fn(int why) { if (why >= 0 && why < 8) g_notes[why]++; }
static void test_prepare(void) {
    wring_t r; ring_init(&r, 1 << 16); wbp_t p;
    wbp_in in = in_dev(4096, 1); mk m = {1, 0}; memset(g_notes, 0, sizeof g_notes);
    uint64_t h0 = r.head;
    CHECK(wbp_prepare(&r, in, &p, make_fn, &m, note_fn) == WBP_GPU && p.at == h0 && m.made == 1 && g_notes[WBF_NOMEM] == 1 && r.nhold == 1);
    wbp_unrecorded(&r, &p);
    in.dev = 0; m = (mk){1, 0};
    CHECK(wbp_prepare(&r, in, &p, make_fn, &m, note_fn) == WBP_NONE && r.head == h0 && r.nhold == 0 && m.made == 0);
    in.dev = 1; m = (mk){2, 0};   // no memory for either: nothing kept
    CHECK(wbp_prepare(&r, in, &p, make_fn, &m, note_fn) == WBP_NONE && r.head == h0 && r.nhold == 0);
}

// Post room: reserved entries survive any number of ordinary posts, and posting into them needs no memory
static void test_postq(void) {
    postq_t q = {0}; q.esz = 24;
    CHECK(postq_reserve(&q) && postq_reserve(&q) && postq_reserve(&q) && q.reserved == 3);
    for (int i = 0; i < 1000; i++) { CHECK(postq_push(&q, 0) != NULL); CHECK(q.n + q.reserved <= q.cap); }
    g_fail_realloc = 1;
    CHECK(postq_push(&q, 0) == NULL || q.n + q.reserved <= q.cap);   // an ordinary post may fail with no memory...
    size_t n0 = q.n;
    for (int i = 0; i < 3; i++) CHECK(postq_push(&q, 1) != NULL);    // ...the reserved ones never do
    CHECK(q.n == n0 + 3 && q.reserved == 0 && q.n <= q.cap);
    CHECK(!postq_reserve(&q) || q.n + q.reserved <= q.cap);
    g_fail_realloc = 0;
    postq_reset(&q); CHECK(q.n == 0 && q.reserved == 0);
    CHECK(postq_reserve(&q)); postq_unreserve(&q); CHECK(q.reserved == 0);
    free(q.v);
}

// A full queue with no memory (the review's case: 16 effects in 16 entries, then a landing and an event): postq_push_must calls its failure before
// adding anything, for an ordinary post and for a reserved token of another submission (posted as ordinary). vk_submit.c's failure ends the run,
// so a submission missing an effect never completes; here the hook returns and the queue must still hold exactly the 16 earlier effects
typedef struct { int kind; uint32_t v; } t_post;
static int g_fatal; static size_t g_fatal_n;
static void t_fatal(const postq_t *q) { g_fatal++; g_fatal_n = q->n; }
static void test_postq_full_nomem(void) {
    postq_t q = {0}; q.esz = sizeof(t_post);
    for (uint32_t i = 0; i < 16; i++) { t_post *d = postq_push_must(&q, 0, t_fatal); CHECK(d != NULL); if (d) *d = (t_post){1, 1}; }
    CHECK(g_fatal == 0 && q.n == 16 && q.cap == 16 && q.reserved == 0);   // capacity filled exactly: the next ordinary post must grow
    g_fail_realloc = 1;
    t_post *land = postq_push_must(&q, 0, t_fatal);                      // the landing (vks_post_fn)
    CHECK(land == NULL && g_fatal == 1 && g_fatal_n == 16 && q.n == 16);
    t_post *mism = postq_push_must(&q, 0, t_fatal);                      // a reserved token of another submission posts as ordinary: same
    CHECK(mism == NULL && g_fatal == 2 && q.n == 16);
    for (size_t i = 0; i < q.n; i++) CHECK(((t_post *)q.v)[i].kind == 1 && ((t_post *)q.v)[i].v == 1);   // nothing partial, nothing overwritten
    g_fail_realloc = 0;
    // with room reserved before the queue filled, the reserved post needs no memory and never fails
    postq_reset(&q); CHECK(postq_reserve(&q));
    for (int i = 0; i < 15; i++) CHECK(postq_push_must(&q, 0, t_fatal) != NULL);
    CHECK(q.n + q.reserved == q.cap);
    g_fail_realloc = 1; g_fatal = 0;
    CHECK(postq_push_must(&q, 1, t_fatal) != NULL && g_fatal == 0 && q.n == 16 && q.reserved == 0);
    CHECK(postq_push_must(&q, 0, t_fatal) == NULL && g_fatal == 1 && q.n == 16);
    g_fail_realloc = 0;
    free(q.v);
}

static void test_take(void) {
    wring_t r; ring_init(&r, 4096); uint64_t at;
    CHECK(!wring_take(&r, 1025, 16, &at));                       // more than a quarter
    CHECK(wring_take(&r, 16, 16, &at) && at == 0);
    CHECK(wring_take(&r, 32, 256, &at) && at == 256 && r.head == 288);
    r.head = 4000; atomic_store(&r.tail, 4000);
    CHECK(wring_take(&r, 256, 64, &at) && at == 0 && r.head == 4096 + 256);   // does not wrap: the piece starts again at 0
    CHECK(!wring_take(&r, 1024, 16, &at) || r.head - atomic_load(&r.tail) <= r.size);
    // holds: wring_end stops at the oldest held plan, in any release order
    ring_init(&r, 4096); CHECK(wring_hold(&r, 100) && wring_hold(&r, 50) && wring_hold(&r, 300)); r.head = 400;
    CHECK(wring_end(&r) == 50); wring_unhold(&r, 50); CHECK(wring_end(&r) == 100); wring_unhold(&r, 300); CHECK(wring_end(&r) == 100);
    wring_unhold(&r, 100); CHECK(wring_end(&r) == 400 && r.nhold == 0);
    wring_released(&r, 200); wring_released(&r, 100); CHECK(atomic_load(&r.tail) == 200);   // never moves back
}

int main(void) {
    printf("old rule (pre-fix, expected to overlap):\n"); { int f = g_failures; test_two_ranges(1); CHECK(g_failures == f); printf("  overlaps %d\n", g_overlaps); }
    printf("two ranges, held plans:\n"); test_two_ranges(0); printf("  overlaps %d\n", g_overlaps);
    printf("take and holds\n"); test_take();
    printf("transactional plans\n"); test_transactional();
    printf("prepare with no memory\n"); test_prepare();
    printf("post room\n"); test_postq();
    printf("full post queue with no memory\n"); test_postq_full_nomem();
    printf("random mixed batches\n"); test_random(1); test_random(0);
    printf(g_failures ? "FAILED: %d\n" : "OK\n", g_failures);
    return g_failures != 0;
}
