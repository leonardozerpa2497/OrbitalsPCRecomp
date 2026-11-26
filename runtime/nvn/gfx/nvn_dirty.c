// from nxnative dm/gfx/nvn/nvn_dirty.c (MIT, (c) seedds), ported to Win32.
// Write-protect dirty-page tracking for guest memory: pool pages of packaged textures (CPU writes, GPU reads) are set read-only after upload, and as soon as the game writes it faults -> mark dirty, unprotect;
// before use, check the write generation of the covered pages and re-upload if newer than at upload time. NVN semantics: a texture descriptor points to pool memory, and the GPU reads the current content every time
// (measured: the game writes streamed assets into texture storage that is still being sampled, without re-registering).
// Signal handler constraint: the page-fault path uses only atomic operations and mprotect, no allocation, no locks.
// Windows: VirtualProtect in place of mprotect (the fault path is a vectored exception handler, crash.c), QueryPerformanceCounter in place of mach time, 4 KB pages.
#include "nvn_dirty.h"
#include "dm.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#define PROT_READ 1
#define PROT_WRITE 2
static uint64_t mach_absolute_time(void) { LARGE_INTEGER c; QueryPerformanceCounter(&c); return (uint64_t)c.QuadPart; }
typedef struct { uint64_t numer, denom; } mach_timebase_info_data_t;   // ticks * numer / denom = ns
static void mach_timebase_info(mach_timebase_info_data_t *tb) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); tb->numer = 1000000000ull; tb->denom = (uint64_t)f.QuadPart; }
static _Atomic int g_vp_red;
static int mprotect_(void *a, size_t len, int prot) {   // 0 = done
    DWORD old; if (VirtualProtect(a, len, prot & PROT_WRITE ? PAGE_READWRITE : PAGE_READONLY, &old)) return 0;
    if (!atomic_exchange(&g_vp_red, 1))
        LOG("[RED] dirty-page tracking: VirtualProtect %p+%#zx failed (error %lu): those pages stay writable and count as always written; later failures are not logged",
            a, len, (unsigned long)GetLastError());
    return -1;
}

// Order of page state and mprotect: the side changing protection first sets the state to BUSY and sets the final state only after mprotect has landed; other threads wait when they see BUSY.
//   set read-only: NONE -> BUSY -> mprotect(read-only) -> PROT; unprotect: PROT -> BUSY -> mprotect(writable) -> NONE.
// Invariant: a page whose state is NONE is always writable, one whose state is PROT is always read-only. Setting read-only used to be "record PROT, then mprotect"; in between, another thread's unprotect-before-write
// (the movie decoder thread writing a frame into texture storage, a disk read into a guest buffer) could set the state back to NONE, then the mprotect landed => the page read-only with state NONE: a game thread writing it got only
// "re-execute" from the fault handler and spun forever; a host system call reading into it saw NONE in prep_write, skipped, and failed with EFAULT (reproduced by t_dirty_race).
// BUSY lasts at most one mprotect (plus a few atomic writes when setting read-only). The wait is capped at BUSY_WAIT_MS and reports red beyond it: in the fault handler it then returns "not our page" so the crash handler reports truthfully
enum { PG_NONE = 0, PG_PROT = 1, PG_BUSY = 2 };
enum { BUSY_WAIT_MS = 2000 };
// Tracked ranges = the memory pools (registered by MemoryPoolInitialize). After a pool is destroyed its live=0 slot is reused; the signal handler scans without locks.
// The table grows in blocks of 1,024 slots, up to 262,144; a block is never moved or freed, so the signal handler reads it without locks. A pool past the end goes untracked: its textures count
// as dirty and are re-uploaded at every use. The table was a fixed 256 slots (Zelda has about 300 pools at the title), then a fixed 4,096, and Breath of the Wild reaches 4,006 pools in its first
// shrine. The scans only cover slots in use, so the capacity itself costs nothing
enum { BLK_SHIFT = 10, BLK = 1 << BLK_SHIFT, NBLK = 256, MAX_RANGE = BLK * NBLK };
// Ranges are rounded to host pages (16 KB) but guest memory pools are only 4 KB aligned: adjacent pools share their first / last pages, so one address can fall in several registered ranges.
// So: "is this texture tracked" looks for a range that holds its whole span; a fault / host pre-write unprotects and stamps every range covering that page; pool destruction identifies its range by the pool's exact start mem.
// (Previously the first range containing the address was always taken: two SMBW ASTC 6x5 textures whose pool's first page is shared with a small preceding pool were judged untracked and re-uploaded on every use, about 6 MB per frame)
typedef struct { uintptr_t base, end, mem; _Atomic uint8_t *state; _Atomic uint64_t *gen; size_t cap; _Atomic int live; } range_t;
static _Atomic(range_t *) g_blk[NBLK];
static inline range_t *rg(int i) { return &atomic_load_explicit(&g_blk[i >> BLK_SHIFT], memory_order_acquire)[i & (BLK - 1)]; }
static _Atomic int g_nr;
static uintptr_t g_pg, g_shift;
static _Alignas(64) _Atomic uint64_t g_now = 1;
static _Alignas(64) _Atomic uint64_t g_faults, g_preps;
static _Atomic uint64_t g_adds_total;
static _Atomic int g_live, g_live_peak;   // number of registered pools and peak (for the memory readout line)
void nvn_dirty_stat(int *live, int *peak, int *cap) { *live = atomic_load(&g_live); *peak = atomic_load(&g_live_peak); *cap = MAX_RANGE; }
static _Atomic int g_overflow_logged;
static pthread_mutex_t g_slot_mu = PTHREAD_MUTEX_INITIALIZER;   // Pool creation, retirement and protection serialize descriptor lifetime; the page-fault path does not touch this mutex
static _Atomic uint64_t g_mp_calls, g_mp_pages, g_mp_ticks;   // This module's mprotect: call count, pages covered, time spent (mach timebase, safe in the page-fault path too)
static int mp(uintptr_t a, size_t len, int prot) {
    uint64_t t = mach_absolute_time(); int r = mprotect_((void *)a, len, prot);
    atomic_fetch_add(&g_mp_ticks, mach_absolute_time() - t); atomic_fetch_add(&g_mp_calls, 1); atomic_fetch_add(&g_mp_pages, len >> g_shift);
    return r;
}

static mach_timebase_info_data_t g_tb;   // obtained when the first pool is registered (the fault path only multiplies and divides)
static _Atomic int g_busy_red;
// Wait for this page to leave BUSY. Returns 1 = it has left; 0 = waited past the limit (reported red once). Usable in a signal handler: only atomic reads, mach_absolute_time and sched_yield
static int busy_wait(range_t *r, uintptr_t p) {
    uint64_t t0 = 0;
    for (unsigned n = 1; atomic_load(&r->state[p]) == PG_BUSY; n++) {
        if (n & 0x3ff) continue;
        uint64_t now = mach_absolute_time(); if (!t0) t0 = now;
        if ((now - t0) * g_tb.numer / g_tb.denom > (uint64_t)BUSY_WAIT_MS * 1000000ull) {
            if (!atomic_exchange(&g_busy_red, 1)) LOG("[RED] dirty-page tracking: page %p stuck 'busy' for over %d ms (the thread changing its protection has not finished), no longer waiting", (void *)(r->base + (p << g_shift)), BUSY_WAIT_MS);
            return 0; }
        sched_yield();
    }
    return 1;
}
static int live_at(int i) { return atomic_load_explicit(&rg(i)->live, memory_order_acquire); }
// The live ranges sorted by base, for the host-side lookups (a host pre-write, a span): those scanned every slot in use, and the write-back landing makes ~6,000
// pre-writes a frame in co-op over ~770 pools (77 % of the completion thread, on the frame's critical path: the game's sync waits end with it). Rebuilt when
// a pool comes or goes (g_rgen), published whole; a reader checks the slot still holds the range it indexed. maxend[k] = the largest end among the first k + 1,
// so the ranges sharing a page with their neighbour (pools are 4 KB aligned, ranges 16 KB) are all found. Old indexes are freed a few rebuilds later (a reader
// holds one for a few lookups at most). The fault handler keeps its lock-free scan
typedef struct { uint64_t gen; int n; uintptr_t *base, *end, *maxend; int *slot; } range_idx;
static _Atomic uint64_t g_rgen = 1;
static _Atomic(range_idx *) g_idx;
static pthread_mutex_t g_idx_mu = PTHREAD_MUTEX_INITIALIZER;
enum { IDX_KEEP = 8 };
static range_idx *g_idx_old[IDX_KEEP]; static int g_idx_oldn;
static int idx_cmp(const void *x, const void *y) { uintptr_t a = rg(*(const int *)x)->base, b = rg(*(const int *)y)->base; return a < b ? -1 : a > b; }
static void idx_free(range_idx *x) { if (x) { free(x->base); free(x->end); free(x->maxend); free(x->slot); free(x); } }
static range_idx *idx_get(void) {
    range_idx *x = atomic_load_explicit(&g_idx, memory_order_acquire);
    uint64_t g = atomic_load_explicit(&g_rgen, memory_order_acquire);
    if (x && x->gen == g) return x;
    pthread_mutex_lock(&g_idx_mu);
    x = atomic_load_explicit(&g_idx, memory_order_acquire); g = atomic_load_explicit(&g_rgen, memory_order_acquire);
    if (!x || x->gen != g) {
        int n = atomic_load(&g_nr), k = 0;
        range_idx *y = calloc(1, sizeof *y);
        int *slots = malloc(sizeof *slots * (size_t)(n ? n : 1));
        if (y && slots) {
            for (int i = 0; i < n; i++) if (live_at(i)) slots[k++] = i;
            qsort(slots, (size_t)k, sizeof *slots, idx_cmp);
            y->gen = g; y->n = k; y->slot = slots;
            y->base = malloc(sizeof *y->base * (size_t)(k ? k : 1)); y->end = malloc(sizeof *y->end * (size_t)(k ? k : 1)); y->maxend = malloc(sizeof *y->maxend * (size_t)(k ? k : 1));
            if (y->base && y->end && y->maxend) {
                uintptr_t m = 0;
                for (int j = 0; j < k; j++) { range_t *r = rg(slots[j]); y->base[j] = r->base; y->end[j] = r->end; if (r->end > m) m = r->end; y->maxend[j] = m; }
                if (g_idx_oldn == IDX_KEEP) { idx_free(g_idx_old[0]); memmove(g_idx_old, g_idx_old + 1, sizeof g_idx_old - sizeof *g_idx_old); g_idx_oldn--; }
                if (x) g_idx_old[g_idx_oldn++] = x;
                atomic_store_explicit(&g_idx, y, memory_order_release); x = y; y = NULL; slots = NULL;
            }
        }
        if (y) { y->slot = NULL; idx_free(y); }
        free(slots);
    }
    pthread_mutex_unlock(&g_idx_mu);
    return x && x->gen == g ? x : NULL;
}
// The indexed ranges that overlap [a, e), last first: fn(range) for each still live and still the one indexed. Returns 0 when there is no index (scan instead)
#define IDX_EACH(a_, e_, r_, ...) do { range_idx *x_ = idx_get(); if (!x_) { idx_ok_ = 0; break; } idx_ok_ = 1;     int lo_ = 0, hi_ = x_->n; while (lo_ < hi_) { int m_ = (lo_ + hi_) / 2; if (x_->base[m_] < (e_)) lo_ = m_ + 1; else hi_ = m_; }     for (int k_ = lo_ - 1; k_ >= 0 && x_->maxend[k_] > (a_); k_--) { if (x_->end[k_] <= (a_)) continue;         int s_ = x_->slot[k_]; range_t *r_ = rg(s_); if (!live_at(s_) || r_->base != x_->base[k_] || r_->end != x_->end[k_]) continue; __VA_ARGS__ } } while (0)
// The registered range that holds all of [a, a+len) (used for a texture's tracking / protection / dirty check)
static range_t *find_span(uintptr_t a, size_t len) {
    int idx_ok_; range_t *found = NULL;
    IDX_EACH(a, a + (len ? len : 1), r, { if (a >= r->base && a + len <= r->end) { found = r; break; } });
    if (idx_ok_) return found;
    int n = atomic_load(&g_nr);
    for (int i = 0; i < n; i++) { range_t *r = rg(i); if (live_at(i) && a >= r->base && a + len <= r->end) return r; }
    return NULL;
}
static void stamp_page(range_t *r, uintptr_t p) {
    atomic_store(&r->gen[p], atomic_fetch_add(&g_now, 1) + 1);
    // Publish completion after the generation: a checker may have cached "clean" using the incremented clock before the generation store. Both fault and host-write paths
    // must invalidate that cached clock, as the page stays writable until the next upload protects it again.
    atomic_fetch_add(&g_now, 1);
}
// Called with pages that are still read-only and about to become writable (a write fault, or a host write about to happen): their content is
// still the one from before the write. Signal-handler rules apply (atomics, memcpy, no lock, no allocation). vk_mem's lazy write-back shadows
static void (*volatile g_unprot_hook)(uintptr_t a, size_t len);
void nvn_dirty_unprotect_hook(void (*fn)(uintptr_t a, size_t len)) { g_unprot_hook = fn; }
static inline void before_unprotect(uintptr_t a, size_t len) { void (*fn)(uintptr_t, size_t) = g_unprot_hook; if (fn) fn(a, len); }
static void unprotect_page(range_t *r, uintptr_t p) {   // The caller has already changed the state from PROT to BUSY
    before_unprotect(r->base + (p << g_shift), g_pg);
    mp(r->base + (p << g_shift), g_pg, PROT_READ | PROT_WRITE);
    stamp_page(r, p);
    atomic_store(&r->state[p], PG_NONE);
}
// Unprotect one page of a range: if protected, unprotect and stamp it; if another thread is changing its protection, wait for it to finish and look again -- if the other side was setting read-only, the page is PROT after the wait and still has to be unprotected
// (an unprotect-before-write never reaches the fault handler, so it must be done here). Returns 1 = this page is managed by this module; 0 = it is not, or the wait on BUSY timed out
static int release_page(range_t *r, uintptr_t p, _Atomic uint64_t *count) {
    for (;;) {
        uint8_t st = PG_PROT;
        if (atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) { unprotect_page(r, p); if (count) atomic_fetch_add(count, 1); return 1; }
        if (st != PG_BUSY) return atomic_load(&r->gen[p]) != 0;   // already NONE: just unprotected by another thread (the fault happened before the unprotect) -> re-execute; pages never protected are not ours
        if (!busy_wait(r, p)) return 0;
    }
}
// Windows: whether a tracked range has this page protected or is changing it, for the heap's fault hook ahead of on_fault (dm_fault_is_wp)
static int page_wp(void *addr) {
    uintptr_t a = (uintptr_t)addr; int n = atomic_load(&g_nr);
    for (int i = 0; i < n; i++) { range_t *r = rg(i); if (live_at(i) && a >= r->base && a < r->end && atomic_load(&r->state[(a - r->base) >> g_shift]) != PG_NONE) return 1; }
    return 0;
}
static int on_fault(void *addr, int is_write, void *uc) {
    (void)uc;
    if (!is_write) return 0;
    uintptr_t a = (uintptr_t)addr; int ours = 0, n = atomic_load(&g_nr);
    for (int i = 0; i < n; i++)   // Every range sharing the page must be released: releasing only one leaves the other range's state at "protected", and later writes no longer fault and are missed
        { range_t *r = rg(i); if (live_at(i) && a >= r->base && a < r->end) ours |= release_page(r, (a - r->base) >> g_shift, &g_faults); }
    return ours;
}
// Unprotect pages p..q of a range with one mprotect per run of protected pages (a host write over a whole texture, e.g. WriteTexels, used to make one system call per 16 KB page:
// Link's Awakening, about 275 a frame). Pages another thread is changing are handled one by one after the run so far is finished, so no BUSY page is held while waiting
static void release_pages(range_t *r, uintptr_t p, uintptr_t q, _Atomic uint64_t *count) {
    uintptr_t run = 0; int open = 0;
    for (uintptr_t i = p; i <= q + 1; i++) {
        uint8_t st = PG_PROT; int took = i <= q && atomic_compare_exchange_strong(&r->state[i], &st, PG_BUSY);
        if (took) { if (!open) { run = i; open = 1; } continue; }
        if (open) {
            before_unprotect(r->base + (run << g_shift), (i - run) << g_shift);
            mp(r->base + (run << g_shift), (i - run) << g_shift, PROT_READ | PROT_WRITE);
            for (uintptr_t k = run; k < i; k++) { stamp_page(r, k); atomic_store(&r->state[k], PG_NONE); }
            if (count) atomic_fetch_add(count, i - run);
            open = 0;
        }
        if (i <= q && st == PG_BUSY) release_page(r, i, count);   // another thread is changing it: wait, then unprotect it if it ended up protected
    }
}
extern volatile int g_swr_trace_on; unsigned long long swr_trace_ts(void); void swr_trace_span(const char *name, unsigned long long t0);
static void prep_write_(void *addr, size_t len);
static void prep_write(void *addr, size_t len) { unsigned long long t0 = g_swr_trace_on ? swr_trace_ts() : 0; prep_write_(addr, len); if (t0) swr_trace_span("dirty tracker: host pre-write (unprotect)", t0); }
static void prep_write_(void *addr, size_t len) {
    uintptr_t a = (uintptr_t)addr, e = a + len; int n = atomic_load(&g_nr);
    if (!len) return;
    { int idx_ok_;
      IDX_EACH(a, e, r, { uintptr_t lo = a > r->base ? a : r->base, hi = e < r->end ? e : r->end;
                          release_pages(r, (lo - r->base) >> g_shift, (hi - 1 - r->base) >> g_shift, &g_preps); });
      if (idx_ok_) return; }
    for (int i = 0; i < n; i++) {
        range_t *r = rg(i);
        if (!live_at(i) || e <= r->base || a >= r->end) continue;
        uintptr_t lo = a > r->base ? a : r->base, hi = e < r->end ? e : r->end;
        release_pages(r, (lo - r->base) >> g_shift, (hi - 1 - r->base) >> g_shift, &g_preps);
    }
}

void nvn_dirty_add_range(const void *mem, size_t size) {
    static int inited;
    if (!inited) { inited = 1; g_pg = (uintptr_t)getpagesize(); g_shift = (uintptr_t)__builtin_ctzll(g_pg); mach_timebase_info(&g_tb); dm_fault_hook_register(on_fault); dm_guest_write_prep_register(prep_write); dm_fault_wp_register(page_wp); }
    atomic_fetch_add(&g_adds_total, 1);
    pthread_mutex_lock(&g_slot_mu);
    int n = atomic_load(&g_nr);
    int slot = -1;
    for (int i = 0; i < n; i++) if (!live_at(i)) { slot = i; break; }
    if (slot < 0) {
        if (n >= MAX_RANGE) {
            int expected = 0;
            if (atomic_compare_exchange_strong(&g_overflow_logged, &expected, 1)) LOG("[RED] dirty-page tracking ranges are full (%d), pool %p not tracked", MAX_RANGE, mem);
            pthread_mutex_unlock(&g_slot_mu); return;
        }
        slot = n;
        if (!atomic_load_explicit(&g_blk[n >> BLK_SHIFT], memory_order_acquire)) {   // published before g_nr counts the slot (the scans read g_nr first)
            range_t *b = calloc(BLK, sizeof *b); if (!b) die("dirty-page tracking range table allocation failed (block %d)", n >> BLK_SHIFT);
            atomic_store_explicit(&g_blk[n >> BLK_SHIFT], b, memory_order_release);
        }
    }
    { int lv = atomic_fetch_add(&g_live, 1) + 1, pk = atomic_load(&g_live_peak); while (lv > pk && !atomic_compare_exchange_weak(&g_live_peak, &pk, lv)) {} }
    range_t *r = rg(slot);
    uintptr_t base = (uintptr_t)mem & ~(g_pg - 1), end = ((uintptr_t)mem + size + g_pg - 1) & ~(g_pg - 1);
    size_t np = (end - base) >> g_shift;
    _Atomic uint8_t *state = r->state;
    _Atomic uint64_t *gen = r->gen;
    if (r->cap < np) {
        state = calloc(np, 1); gen = calloc(np, sizeof *gen);
        if (!state || !gen) die("dirty-page table allocation failed (%zu pages)", np);
        // The old array is not freed: the signal handler may still have a very brief stale reader; leak = 9 bytes per page.
        r->cap = np;
    } else {
        memset((void *)state, 0, np); memset((void *)gen, 0, np * sizeof *gen);
    }
    r->base = base; r->end = end; r->mem = (uintptr_t)mem; r->state = state; r->gen = gen;
    if (slot == n) atomic_store(&g_nr, n + 1);
    atomic_store_explicit(&r->live, 1, memory_order_release);
    atomic_fetch_add_explicit(&g_rgen, 1, memory_order_release);   // the sorted index is rebuilt at its next use
    pthread_mutex_unlock(&g_slot_mu);
}
// Pool destruction: first restore still-protected pages to writable (the pool memory may be reused for something else once returned to the game), then mark invalid
void nvn_dirty_remove_range(const void *mem) {
    pthread_mutex_lock(&g_slot_mu);
    range_t *r = NULL; int n = atomic_load(&g_nr);
    for (int i = 0; i < n && !r; i++) if (live_at(i) && rg(i)->mem == (uintptr_t)mem) r = rg(i);   // Match by the pool's exact start: an adjacent pool sharing a page may have the same rounded base
    if (r) { prep_write((void *)r->base, r->end - r->base); atomic_store_explicit(&r->live, 0, memory_order_release); atomic_fetch_sub(&g_live, 1); atomic_fetch_add_explicit(&g_rgen, 1, memory_order_release); }
    pthread_mutex_unlock(&g_slot_mu);
}
int nvn_dirty_tracked(const void *addr, size_t len) {
    return find_span((uintptr_t)addr, len) != NULL;
}
// The tracked range holding [addr, addr+len), as a handle a caller keeps (packaged textures: the scan over every range ran twice per texture lookup). NULL = untracked
const void *nvn_dirty_span(const void *addr, size_t len) { return len ? find_span((uintptr_t)addr, len) : NULL; }
// Set read-only: mark the pages BUSY first and mark them PROT only after mprotect has landed (for the order and the invariant see the state notes at the top of the file)
static void protect_(const void *span, const void *addr, size_t len);
void nvn_dirty_protect(const void *addr, size_t len) { protect_(NULL, addr, len); }
// A pool's upload protects its pages ~700 times a frame: through its kept range handle, not a scan over ~5,000 ranges each time (checked: still live, still covering)
void nvn_dirty_protect_span(const void *span, const void *addr, size_t len) { protect_(span, addr, len); }
static void protect_(const void *span, const void *addr, size_t len) {
    // Keep this descriptor live through the last mprotect. Retirement restores
    // RW under this same mutex; it must not finish between lookup and PG_BUSY.
    pthread_mutex_lock(&g_slot_mu);
    range_t *r = (range_t *)span;
    if (!r || !len || !atomic_load_explicit(&r->live, memory_order_acquire) || (uintptr_t)addr < r->base || (uintptr_t)addr + len > r->end) r = len ? find_span((uintptr_t)addr, len) : NULL;
    if (!r) { pthread_mutex_unlock(&g_slot_mu); return; }
    uintptr_t a = (uintptr_t)addr, p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift;
    // Consecutive pages are merged into one mprotect (per-page calls reach thousands per frame on the first upload of large textures; character select page startup frame 5: 4888 calls).
    // Pages already PROT / BUSY break the consecutive run: do not protect again, and do not touch pages another thread is changing.
    // A BUSY page is waited out (the run so far is protected first, so none of ours is BUSY meanwhile) and looked at again: an unprotect in progress leaves the page writable
    // with a generation the caller's stamp may cover, so skipped, its later writes never faulted and the texture stayed stale (MacNativeGames a166602)
    uintptr_t run = 0; int open = 0;
    for (uintptr_t i = p; i <= q + 1; i++) {
        uint8_t st = PG_NONE;
        int took = i <= q && atomic_compare_exchange_strong(&r->state[i], &st, PG_BUSY);
        if (took && !open) { run = i; open = 1; }
        if (!took && open) {
            // Windows can refuse (uncommitted pages): those stay writable and untracked, so they count as always written (UINT64_MAX) rather than silently clean;
            // once a later protect takes, such a page counts as written now and is tracked from there
            if (!mp(r->base + (run << g_shift), (i - run) << g_shift, PROT_READ))
                for (uintptr_t k = run; k < i; k++) {
                    if (atomic_load(&r->gen[k]) == UINT64_MAX) atomic_store(&r->gen[k], atomic_fetch_add(&g_now, 1) + 1);
                    atomic_store(&r->state[k], PG_PROT);
                }
            else for (uintptr_t k = run; k < i; k++) { atomic_store(&r->gen[k], UINT64_MAX); atomic_store(&r->state[k], PG_NONE); }
            open = 0;
        }
        if (!took && i <= q && st == PG_BUSY && busy_wait(r, i)) i--;   // this page again
    }
    pthread_mutex_unlock(&g_slot_mu);
}
uint64_t nvn_dirty_now(void) { return atomic_load(&g_now); }
// Whether the host page holding addr is read-only for every tracked range that covers it (so any write to it reaches before_unprotect first).
// A page being changed (BUSY) or writable in one of them is not
int nvn_dirty_page_protected(const void *addr) {
    uintptr_t a = (uintptr_t)addr; int n = atomic_load(&g_nr), seen = 0;
    for (int i = 0; i < n; i++) { range_t *r = rg(i); if (!live_at(i) || a < r->base || a >= r->end) continue;
        if (atomic_load(&r->state[(a - r->base) >> g_shift]) != PG_PROT) return 0; seen = 1; }
    return seen;
}
// The same through a kept range handle: an inner page of a range is covered by that range alone (ranges overlap only on their rounded first / last
// page), so one state read; the edge pages, or a handle that no longer holds the page, take the full scan
int nvn_dirty_page_protected_span(const void *span, const void *addr) {
    const range_t *r = (const range_t *)span; uintptr_t a = (uintptr_t)addr;
    if (!r || !atomic_load_explicit(&((range_t *)r)->live, memory_order_acquire) || a < r->base + g_pg || a >= r->end - g_pg) return nvn_dirty_page_protected(addr);
    return atomic_load(&r->state[(a - r->base) >> g_shift]) == PG_PROT;
}
int nvn_dirty_since_span(const void *span, const void *addr, size_t len, uint64_t stamp) {
    if (!len) return 0;
    range_t *r = (range_t *)span; uintptr_t a0 = (uintptr_t)addr;   // the kept handle still holds the span (a slot reused by another pool over the same memory is as good): no scan
    if (!r || !atomic_load_explicit(&r->live, memory_order_acquire) || a0 < r->base || a0 + len > r->end) r = find_span(a0, len);
    if (!r) return 1;   // When untracked, conservatively treat as dirty: better to upload more than to show stale content
    uintptr_t a = (uintptr_t)addr, p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift;
    for (uintptr_t i = p; i <= q; i++) if (atomic_load(&r->gen[i]) > stamp) return 1;   // The generation of the last write before upload is <= the stamp; any later write must be > the stamp
    return 0;
}
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp) { return nvn_dirty_since_span(NULL, addr, len, stamp); }
void nvn_dirty_slot_stats(int *live, int *used_slots, uint64_t *adds_total) {
    int n = atomic_load(&g_nr), active = 0;
    for (int i = 0; i < n; i++) active += live_at(i) != 0;
    *live = active; *used_slots = n; *adds_total = atomic_load(&g_adds_total);
}
void nvn_dirty_stats(uint64_t *faults, uint64_t *preps) { *faults = atomic_load(&g_faults); *preps = atomic_load(&g_preps); }
void nvn_dirty_mp_stats(uint64_t *calls, uint64_t *pages, uint64_t *ns) {
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    *calls = atomic_load(&g_mp_calls); *pages = atomic_load(&g_mp_pages); *ns = (uint64_t)((double)atomic_load(&g_mp_ticks) * (double)tb.numer / (double)tb.denom);
}
