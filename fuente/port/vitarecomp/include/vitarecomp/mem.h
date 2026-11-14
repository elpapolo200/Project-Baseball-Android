/* mem.h — the recompiled program's address space.
 *
 * The Vita has an MMU, so unlike the PSP there is no small fixed map to fold
 * onto a backing store with a mask. What there is instead is simpler in
 * practice: a module is loaded at a known base (0x81000000 for the executable
 * segment) and the interesting addresses are all within a bounded window above
 * it. A flat host allocation plus a subtraction covers it.
 *
 * Accesses outside the window are COUNTED rather than silently returning zero.
 * A recompiled program should reach a steady state with that counter flat;
 * when it climbs, discovery or translation missed something.
 */

#ifndef VITARECOMP_MEM_H
#define VITARECOMP_MEM_H

#include <stdint.h>
#include <stddef.h>

extern uint32_t vita_mem_bad_access;

/* Reserve `size` bytes of guest address space starting at `base`. */
int  vita_mem_init(uint32_t base, size_t size);
void vita_mem_free(void);

/* Returns NULL rather than a wild pointer when an access would straddle the
 * end of the region, so a bad address is a diagnosable NULL dereference at the
 * point of failure instead of silent corruption elsewhere. */
void *vita_mem_ptr(uint32_t addr, uint32_t len);

/* Bytes backed from `addr` to the end of the region, or 0 if it is outside.
 *
 * The HLE needs this and `vita_mem_ptr` cannot answer it: a C string has no
 * length until you find its terminator, so forwarding `strlen` to the host
 * means scanning guest memory, and a scan with no bound runs off the
 * allocation when the guest hands over a pointer to something that is not a
 * string. Unlike a bad read this does not count as a bad access — asking how
 * much is there is a legitimate question. */
uint32_t vita_mem_avail(uint32_t addr);

/* The flat window, so a separately built image (the MLB 15 port's game DLL) can
 * reach guest memory without a call per access: host = mem + (addr - base). */
void vita_mem_window(uint8_t **mem, uint32_t *base, size_t *size);

#if defined(VITA_MEM_DIRECTA)
/* MLB 15 port, 2026-11-08 (PLAN_RENDIMIENTO.md, fase 1). In the game DLL every
 * load/store used to be an imported call into the exe, then vita_mem_ptr, then
 * memcpy: three calls per access, millions per second. Here it is one compare
 * and a direct access; anything outside the window (or before the window is
 * set) still goes to the exe's functions, which count it exactly as before. */
extern uintptr_t vita_md_off;   /* (uintptr_t)mem - base */
extern uint32_t  vita_md_base;
extern uint32_t  vita_md_lim;   /* size - 3: a 4-byte access at off < lim fits; 0 = not set */
uint32_t vita_read32_lento(uint32_t addr) __asm__("vita_read32");
uint16_t vita_read16_lento(uint32_t addr) __asm__("vita_read16");
uint8_t  vita_read8_lento (uint32_t addr) __asm__("vita_read8");
void vita_write32_lento(uint32_t addr, uint32_t v) __asm__("vita_write32");
void vita_write16_lento(uint32_t addr, uint16_t v) __asm__("vita_write16");
void vita_write8_lento (uint32_t addr, uint8_t  v) __asm__("vita_write8");
#define VITA_MD_DENTRO(a) ((uint32_t)((a) - vita_md_base) < vita_md_lim)
#define VITA_MD_P(a) (vita_md_off + (uintptr_t)(uint32_t)(a))
/* volatile: every guest access really happens, in order. Without it the compiler,
 * now seeing plain memory instead of a call, hoisted loads out of the game's spin
 * loops (a thread waiting for another to set a flag never saw it change: the
 * frontend hung). Still no call, just a compare and one mov. aligned(1): guest
 * addresses need not be aligned (fine on x86). */
typedef volatile uint32_t __attribute__((aligned(1))) vita_md_u32;
typedef volatile uint16_t __attribute__((aligned(1))) vita_md_u16;
typedef volatile uint8_t vita_md_u8;
static inline uint32_t vita_md_read32(uint32_t a) {
    if (__builtin_expect(VITA_MD_DENTRO(a), 1)) return *(vita_md_u32 *)VITA_MD_P(a);
    return vita_read32_lento(a);
}
static inline uint16_t vita_md_read16(uint32_t a) {
    if (__builtin_expect(VITA_MD_DENTRO(a), 1)) return *(vita_md_u16 *)VITA_MD_P(a);
    return vita_read16_lento(a);
}
static inline uint8_t vita_md_read8(uint32_t a) {
    if (__builtin_expect(VITA_MD_DENTRO(a), 1)) return *(vita_md_u8 *)VITA_MD_P(a);
    return vita_read8_lento(a);
}
static inline void vita_md_write32(uint32_t a, uint32_t v) {
    if (__builtin_expect(VITA_MD_DENTRO(a), 1)) { *(vita_md_u32 *)VITA_MD_P(a) = v; return; }
    vita_write32_lento(a, v);
}
static inline void vita_md_write16(uint32_t a, uint16_t v) {
    if (__builtin_expect(VITA_MD_DENTRO(a), 1)) { *(vita_md_u16 *)VITA_MD_P(a) = v; return; }
    vita_write16_lento(a, v);
}
static inline void vita_md_write8(uint32_t a, uint8_t v) {
    if (__builtin_expect(VITA_MD_DENTRO(a), 1)) { *(vita_md_u8 *)VITA_MD_P(a) = v; return; }
    vita_write8_lento(a, v);
}
/* Distinct names + macros: a static function called vita_read32 would share the
 * symbol the slow path's asm label points at, and an out-of-window access then
 * called itself forever (the frontend hung at 98 % CPU). */
#define vita_read32(a)     vita_md_read32(a)
#define vita_read16(a)     vita_md_read16(a)
#define vita_read8(a)      vita_md_read8(a)
#define vita_write32(a, v) vita_md_write32(a, v)
#define vita_write16(a, v) vita_md_write16(a, v)
#define vita_write8(a, v)  vita_md_write8(a, v)
#else
uint32_t vita_read32(uint32_t addr);
uint16_t vita_read16(uint32_t addr);
uint8_t  vita_read8 (uint32_t addr);

void vita_write32(uint32_t addr, uint32_t v);
void vita_write16(uint32_t addr, uint16_t v);
void vita_write8 (uint32_t addr, uint8_t  v);
#endif

#endif /* VITARECOMP_MEM_H */
