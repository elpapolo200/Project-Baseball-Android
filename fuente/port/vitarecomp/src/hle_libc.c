/* hle_libc.c — SceLibc, forwarded to the host C library.
 *
 * These are the easy ones, and they are easy for a specific reason: SceLibc is
 * the standard C library, so the semantics are already specified and already
 * implemented on the host. What is NOT free is the boundary — every pointer
 * argument is a guest address into a flat allocation, not a host pointer, and
 * every one has to be bounds-checked before it is touched. A forwarded
 * `memcpy` that skips that step is a guest-controlled write anywhere in the
 * host process.
 *
 * See hle.h for the calling convention and the link-order requirement.
 */

#include "vitarecomp/cpu.h"
#include "vitarecomp/dispatch.h"
#include "vitarecomp/hle.h"
#include "vitarecomp/mem.h"
#include "vitarecomp/recomp_rt.h"

#include <stdio.h>
#include <string.h>

/* --- the guest/host boundary ------------------------------------------------ */

/* Translate a guest pointer, or trap. Returning NULL and continuing is not an
 * option here: the caller asked for a string to be copied, and a copy that did
 * not happen leaves the program running with the work undone — the same reason
 * a dispatch miss stops rather than returns. */
static void *gp(uint32_t addr, uint32_t len, const char *who) {
    if (len == 0) return NULL;              /* a zero-length op touches nothing */
    void *p = vita_mem_ptr(addr, len);
    if (!p) vita_trap_unimpl(addr, len, who);
    return p;
}

/* The length of a guest C string, bounded by what is actually backed.
 *
 * A host strlen on guest memory runs off the end of the allocation the moment
 * the guest passes something that is not a string, and it does so reading host
 * memory it has no business in. Scanning within the region turns that into a
 * named trap. */
static uint32_t gstrlen(uint32_t addr, const char *who) {
    uint32_t avail = vita_mem_avail(addr);
    if (avail == 0) { vita_trap_unimpl(addr, 0, who); return 0; }
    const uint8_t *p = (const uint8_t *)vita_mem_ptr(addr, 1);
    if (!p) { vita_trap_unimpl(addr, 0, who); return 0; }
    const void *nul = memchr(p, 0, avail);
    if (!nul) { vita_trap_unimpl(addr, avail, who); return avail; }
    return (uint32_t)((const uint8_t *)nul - p);
}

/* --- C++ runtime and CRT init ------------------------------------------------
 *
 * The first firmware the module touches, before any of its own code runs. All
 * of these are bookkeeping for a dynamic-linking and multi-threading world
 * that a statically recompiled single module does not have, which is why they
 * are so short — they are correct as no-ops, not stubbed as ones.
 */

/* Records the DSO handle used to key static destructors. There is one module
 * and it never unloads, so there is nothing to key. */
void vita_hle___cxa_set_dso_handle_main(void) { }

/* Register a destructor to run at exit. Nothing here runs exit — the host
 * stops the program — so registration succeeds and the list is never walked.
 * Returning nonzero would make the C++ runtime treat static construction as
 * having failed. */
void vita_hle___cxa_atexit(void)   { r0 = 0; }
void vita_hle___aeabi_atexit(void) { r0 = 0; }

/* Guards for function-local static initialisation. The guard object's first
 * byte is "initialised"; acquire returns nonzero when the caller should run
 * the constructor.
 *
 * ponytail: single-threaded, so no contention state and no waiting. Two
 * recompiled threads reaching the same static would both construct it; revisit
 * when sceKernel threads land. */
void vita_hle___cxa_guard_acquire(void) {
    r0 = vita_read8(r0) ? 0u : 1u;
}
void vita_hle___cxa_guard_release(void) {
    vita_write8(r0, 1);
}

/* Thread-local storage registration for the module. No TLS, one thread. */
void vita_hle__sceLdTlsRegisterModuleInfo(void) { r0 = 0; }

/* --- errno -------------------------------------------------------------------
 *
 * `errno` is a macro for *__errno_loc(), so the guest needs an address it can
 * write through, which means the slot has to live in GUEST memory rather than
 * be a host int. It is carved out of the heap on first use for that reason.
 */

static uint32_t g_errno_slot;

void vita_hle__sceLibcErrnoLoc(void) { r0 = g_errno_slot; }

/* --- the guest heap ----------------------------------------------------------
 *
 * ponytail: a bump allocator. `free` is a no-op and `realloc` always copies to
 * fresh space, so a program that churns allocations will exhaust the region
 * even though its live set is small. That is the known ceiling, and it is the
 * right first move: C++ static initialisation allocates once and holds, which
 * is exactly the phase this needs to get through. Replace with a real free
 * list when a run dies on exhaustion rather than before.
 */

static uint32_t g_heap_base, g_heap_size, g_heap_used;

void vita_heap_init(uint32_t base, uint32_t size) {
    g_heap_base = base;
    g_heap_size = size;
    g_heap_used = 0;
    /* One word up front, so errno has somewhere to live before any malloc. */
    if (size >= 4) { g_errno_slot = base; g_heap_used = 4; }
}

uint32_t vita_heap_used(void) { return g_heap_used; }
uint32_t vita_heap_size(void) { return g_heap_size; }

/* MLB 15 port: guest threads run in parallel, so the cursor is behind a lock. */
#if defined(_MSC_VER)
#include <intrin.h>
static volatile long g_heap_lock;
static void heap_lock(void)   { while (_InterlockedExchange(&g_heap_lock, 1)) _mm_pause(); }
static void heap_unlock(void) { _InterlockedExchange(&g_heap_lock, 0); }
#else
static volatile int g_heap_lock;
static void heap_lock(void)   { while (__sync_lock_test_and_set(&g_heap_lock, 1)) { } }
static void heap_unlock(void) { __sync_lock_release(&g_heap_lock); }
#endif

static uint32_t heap_alloc_locked(uint32_t n);

static uint32_t heap_alloc(uint32_t n) {
    heap_lock();
    uint32_t at = heap_alloc_locked(n);
    heap_unlock();
    return at;
}

static uint32_t heap_alloc_locked(uint32_t n) {
    if (!g_heap_size) { vita_trap_unimpl(0, n, "malloc before vita_heap_init"); return 0; }

    /* Align the OFFSET, not just the size. Rounding sizes up keeps the cursor
     * aligned only while every reservation does the same, and the errno slot
     * does not — it is one word. Aligning here does not care who moved the
     * cursor or by how much. */
    uint32_t at_off = (g_heap_used + 7u) & ~7u;
    uint32_t size   = (n + 7u) & ~7u;
    if (size < n || at_off < g_heap_used) return 0;         /* either add wrapped */
    if (at_off > g_heap_size || size > g_heap_size - at_off) {
        vita_trap_unimpl(g_heap_used, n, "guest heap exhausted");
        return 0;
    }
    g_heap_used = at_off + size;
    return g_heap_base + at_off;
}

/* --- the module's own allocator (SceLibcParam) --------------------------------
 *
 * MLB 15 port. A module can replace libc's malloc and C++'s new/delete by
 * pointing SceLibcParam at tables of its own functions; the real SceLibc then
 * calls malloc_init at load and routes every allocation through them. MLB 15
 * does exactly that, and a static constructor waits for its malloc_init to
 * have run. So: read the tables, run the init functions the way SceLibc would,
 * and from then on forward to the guest's own allocator. The bump allocator
 * below stays as the fallback for modules that do not replace anything.
 *
 * Table layouts (after u32 size, u32 version):
 *   malloc:  init, finalize, malloc, free, calloc, realloc, memalign,
 *            reallocalign, malloc_stats, malloc_stats_fast, malloc_usable_size
 *   new:     new, new(nothrow), new[], new[](nothrow),
 *            delete, delete(nothrow), delete[], delete[](nothrow)
 *   tls:     init, finalize, malloc, free
 */

enum { MR_INIT, MR_FINI, MR_MALLOC, MR_FREE, MR_CALLOC, MR_REALLOC, MR_MEMALIGN,
       MR_REALLOCALIGN, MR_STATS, MR_STATS_FAST, MR_USABLE, MR_COUNT };
enum { NR_NEW, NR_NEW_NT, NR_NEW_ARR, NR_NEW_ARR_NT,
       NR_DEL, NR_DEL_NT, NR_DEL_ARR, NR_DEL_ARR_NT, NR_COUNT };

static uint32_t g_malloc_fn[MR_COUNT];
static uint32_t g_new_fn[NR_COUNT];
static uint32_t g_tls_fn[4];

static void read_fn_table(uint32_t table, uint32_t *out, int max) {
    uint32_t size = vita_read32(table);
    int n = size >= 8 ? (int)((size - 8) / 4) : 0;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = vita_read32(table + 8 + 4u * (uint32_t)i);
}

void vita_libc_init(uint32_t process_param) {
    uint32_t libc = vita_read32(process_param + 0x2C);
    if (!libc) return;
    uint32_t size = vita_read32(libc);
    uint32_t malloc_tbl = vita_read32(libc + 0x20);
    uint32_t new_tbl    = vita_read32(libc + 0x24);
    uint32_t tls_tbl    = size >= 0x38 ? vita_read32(libc + 0x34) : 0;
    if (new_tbl) read_fn_table(new_tbl, g_new_fn, NR_COUNT);
    if (malloc_tbl) {
        read_fn_table(malloc_tbl, g_malloc_fn, MR_COUNT);
        if (g_malloc_fn[MR_INIT]) vita_dispatch(malloc_tbl, g_malloc_fn[MR_INIT]);
    }
    if (tls_tbl) {
        read_fn_table(tls_tbl, g_tls_fn, 4);
        if (g_tls_fn[0]) vita_dispatch(tls_tbl, g_tls_fn[0]);
    }
}

/* Forward to the guest function if the module supplied one. Arguments are
 * already in r0-r3 and the result comes back in r0. */
static int forward(uint32_t fn) {
    if (!fn) return 0;
    vita_dispatch(0, fn);
    return 1;
}

void vita_hle_malloc(void) {
    if (forward(g_malloc_fn[MR_MALLOC])) return;
    uint32_t n = r0;
    r0 = n ? heap_alloc(n) : 0u;
}

void vita_hle_free(void) {
    forward(g_malloc_fn[MR_FREE]); /* bump allocator: nothing to return */
}

void vita_hle_calloc(void) {
    if (forward(g_malloc_fn[MR_CALLOC])) return;
    uint32_t n = r0 * r1;
    uint32_t at = n ? heap_alloc(n) : 0u;
    void *p = at ? gp(at, n, "calloc") : NULL;
    if (p) memset(p, 0, n);
    r0 = at;
}

void vita_hle_memalign(void) {
    if (forward(g_malloc_fn[MR_MEMALIGN])) return;
    uint32_t align = r0 ? r0 : 8u, n = r1;
    uint32_t at = heap_alloc(n + align);
    r0 = at ? (at + align - 1u) & ~(align - 1u) : 0u;
}

void vita_hle__Znwj(void)  { if (!forward(g_new_fn[NR_NEW]))     vita_hle_malloc(); }
void vita_hle__Znaj(void)  { if (!forward(g_new_fn[NR_NEW_ARR])) vita_hle_malloc(); }
void vita_hle__ZdlPv(void) { if (!forward(g_new_fn[NR_DEL]))     vita_hle_free(); }
void vita_hle__ZdaPv(void) { if (!forward(g_new_fn[NR_DEL_ARR])) vita_hle_free(); }

void vita_hle_realloc(void) {
    if (forward(g_malloc_fn[MR_REALLOC])) return;
    uint32_t old = r0, n = r1;
    if (!n)   { r0 = 0; return; }
    if (!old) { r0 = heap_alloc(n); return; }
    /* The old size is unknown to a bump allocator, so copy what is certainly
     * readable and no more. Over-copying here would read past the original
     * allocation; under-copying loses data the caller still owns, so the bound
     * is what the region can back rather than a guess at the old size. */
    uint32_t at = heap_alloc(n);
    if (!at) { r0 = 0; return; }
    uint32_t avail = vita_mem_avail(old);
    uint32_t copy  = n < avail ? n : avail;
    void *d = gp(at, copy, "realloc dst");
    void *s = gp(old, copy, "realloc src");
    if (d && s) memmove(d, s, copy);
    r0 = at;
}

/* --- memory ------------------------------------------------------------------
 *
 * Sizes are known up front, so these are a bounds check and a host call.
 */

/* Destination the guest cannot back: when the embedder sets this (the MLB15
 * port does, for a garbage pointer in the 3D crowd code) the copy is skipped
 * and reported instead of trapping. Off by default: a trap names the bug. */
int vita_libc_skip_bad_dst = 0;
/* Optional: called (with the guest registers live) each time a copy is skipped. */
void (*vita_libc_bad_dst_hook)(uint32_t addr, uint32_t len) = 0;

static void *gp_dst(uint32_t addr, uint32_t len, const char *who) {
    if (len == 0) return NULL;
    void *p = vita_mem_ptr(addr, len);
    if (!p && vita_libc_skip_bad_dst) {
        static int told;
        if (told++ < 20) {
            printf("vitarecomp: %s 0x%08X (%u bytes) not backed: skipped (lr=0x%08X)\n", who, addr, len, lr);
            if (vita_libc_bad_dst_hook) vita_libc_bad_dst_hook(addr, len);
            fflush(stdout);
        }
        return NULL;
    }
    if (!p) vita_trap_unimpl(addr, len, who);
    return p;
}

void vita_hle_memcpy(void) {
    void *d = gp_dst(r0, r2, "memcpy dst"), *s = d ? gp(r1, r2, "memcpy src") : NULL;
    /* memmove, not memcpy: the guest may hand over overlapping regions, and
     * while ARM's memcpy is equally undefined there, the host's would corrupt
     * rather than trap and the bug would surface far away. */
    if (d && s) memmove(d, s, r2);
    /* memcpy returns its destination. */
}

void vita_hle_memmove(void) {
    void *d = gp_dst(r0, r2, "memmove dst"), *s = d ? gp(r1, r2, "memmove src") : NULL;
    if (d && s) memmove(d, s, r2);
}

void vita_hle_memset(void) {
    void *d = gp_dst(r0, r2, "memset");
    if (d) memset(d, (int)(r1 & 0xFF), r2);
}

void vita_hle_memcmp(void) {
    void *a = gp(r0, r2, "memcmp a"), *b = gp(r1, r2, "memcmp b");
    r0 = (a && b) ? (uint32_t)(int32_t)memcmp(a, b, r2) : 0u;
}

void vita_hle_memchr(void) {
    uint8_t *p = (uint8_t *)gp(r0, r2, "memchr");
    if (!p) { r0 = 0; return; }
    uint8_t *hit = (uint8_t *)memchr(p, (int)(r1 & 0xFF), r2);
    /* A guest pointer back, not a host one: the offset is what carries over. */
    r0 = hit ? r0 + (uint32_t)(hit - p) : 0u;
}

/* --- strings -----------------------------------------------------------------
 *
 * Every one of these has to measure before it can act, because the length is
 * in the data. The measurement is the bounds check.
 */

void vita_hle_strlen(void) { r0 = gstrlen(r0, "strlen"); }

void vita_hle_strcmp(void) {
    uint32_t la = gstrlen(r0, "strcmp a"), lb = gstrlen(r1, "strcmp b");
    void *a = gp(r0, la + 1, "strcmp a"), *b = gp(r1, lb + 1, "strcmp b");
    r0 = (a && b) ? (uint32_t)(int32_t)strcmp((char *)a, (char *)b) : 0u;
}

void vita_hle_strncmp(void) {
    uint32_t n = r2;
    uint32_t la = gstrlen(r0, "strncmp a"), lb = gstrlen(r1, "strncmp b");
    uint32_t ca = la + 1 < n ? la + 1 : n, cb = lb + 1 < n ? lb + 1 : n;
    void *a = gp(r0, ca, "strncmp a"), *b = gp(r1, cb, "strncmp b");
    r0 = (a && b) ? (uint32_t)(int32_t)strncmp((char *)a, (char *)b, n) : 0u;
}

void vita_hle_strcpy(void) {
    uint32_t n = gstrlen(r1, "strcpy src") + 1;
    void *d = gp(r0, n, "strcpy dst"), *s = gp(r1, n, "strcpy src");
    if (d && s) memcpy(d, s, n);
}

void vita_hle_strncpy(void) {
    uint32_t n = r2, l = gstrlen(r1, "strncpy src");
    void *d = gp(r0, n, "strncpy dst");
    if (!d) return;
    uint32_t copy = l < n ? l : n;
    void *s = gp(r1, copy, "strncpy src");
    if (s) memcpy(d, s, copy);
    /* strncpy pads the remainder with NULs — not just one, all of them. */
    if (copy < n) memset((uint8_t *)d + copy, 0, n - copy);
}

void vita_hle_strcat(void) {
    uint32_t ld = gstrlen(r0, "strcat dst"), ls = gstrlen(r1, "strcat src");
    void *d = gp(r0 + ld, ls + 1, "strcat dst"), *s = gp(r1, ls + 1, "strcat src");
    if (d && s) memcpy(d, s, ls + 1);
}

void vita_hle_strchr(void) {
    uint32_t base = r0, l = gstrlen(base, "strchr");
    /* The terminator is findable: strchr(s, 0) returns the end of the string. */
    uint8_t *p = (uint8_t *)gp(base, l + 1, "strchr");
    if (!p) { r0 = 0; return; }
    uint8_t *hit = (uint8_t *)memchr(p, (int)(r1 & 0xFF), l + 1);
    r0 = hit ? base + (uint32_t)(hit - p) : 0u;
}

void vita_hle_strrchr(void) {
    uint32_t base = r0, l = gstrlen(base, "strrchr");
    uint8_t *p = (uint8_t *)gp(base, l + 1, "strrchr");
    if (!p) { r0 = 0; return; }
    uint8_t c = (uint8_t)(r1 & 0xFF);
    for (uint32_t i = l + 1; i-- > 0; ) {
        if (p[i] == c) { r0 = base + i; return; }
    }
    r0 = 0;
}

void vita_hle_strstr(void) {
    uint32_t base = r0;
    uint32_t lh = gstrlen(base, "strstr haystack"), ln = gstrlen(r1, "strstr needle");
    char *h = (char *)gp(base, lh + 1, "strstr haystack");
    char *n = (char *)gp(r1, ln + 1, "strstr needle");
    if (!h || !n) { r0 = 0; return; }
    char *hit = strstr(h, n);
    r0 = hit ? base + (uint32_t)(hit - h) : 0u;
}
