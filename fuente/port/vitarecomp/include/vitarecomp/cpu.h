/* cpu.h — the recompiled program's machine state.
 *
 * Registers are globals rather than a struct passed around, for the same
 * reason every recompiler does it: the generated C is millions of lines and
 * `r0` reads better than `cpu->r[0]` a few hundred thousand times over. There
 * is one thread of recompiled control at a time.
 */

#ifndef VITARECOMP_CPU_H
#define VITARECOMP_CPU_H

#include <stdint.h>

/* MLB 15 port: the game runs many guest threads (render, kinematics, tasks,
 * audio, streaming), so the machine state is per host thread. Every guest
 * thread runs on its own host thread with its own registers. */
#if defined(_MSC_VER)
#define VITA_TLS __declspec(thread)
#else
#define VITA_TLS _Thread_local
#endif

/* MLB 15 port (2026-11-06): the whole machine state lives in one per-thread
 * struct so the recompiled game can be a separate DLL built on the player's PC
 * (nothing of the game is distributed). The state belongs to whichever image
 * runs the generated code:
 *   VITA_CPU_PROPIA  the game DLL: direct TLS access, as fast as before;
 *   VITA_CPU_REMOTA  the host exe: reaches the DLL's TLS through a pointer
 *                    cached per thread (vita_cpu_alta asks the DLL once);
 *   neither          classic static link (cpu.c defines it).
 * r0-r12, then the three with architectural meaning. r15 (PC) is deliberately
 * absent: a recompiled program has no program counter, and any instruction
 * that genuinely reads PC has its value resolved at translation time.
 * NZCV are held as separate ints rather than packed into a CPSR word. */
typedef union {
    uint32_t u;
    float    f;
} vfp_reg;

typedef struct {
    uint32_t r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12, sp, lr;
    int flag_n, flag_z, flag_c, flag_v;
    int fpscr_n, fpscr_z, fpscr_c, fpscr_v;
    vfp_reg vfp_s[64];
} vita_cpu_state;

#if defined(VITA_CPU_REMOTA)
extern VITA_TLS vita_cpu_state *vita_cpu_p;
vita_cpu_state *vita_cpu_alta(void);
/* The DLL's function returning its per-thread state (set after LoadLibrary). */
extern vita_cpu_state *(*vita_cpu_fuente)(void);
static inline vita_cpu_state *vita_cpu_actual(void) {
    vita_cpu_state *p = vita_cpu_p;
    return p ? p : vita_cpu_alta();
}
#define VITA_CPU (*vita_cpu_actual())
#else
extern VITA_TLS vita_cpu_state vita_cpu;
#define VITA_CPU vita_cpu
#endif

#define r0  (VITA_CPU.r0)
#define r1  (VITA_CPU.r1)
#define r2  (VITA_CPU.r2)
#define r3  (VITA_CPU.r3)
#define r4  (VITA_CPU.r4)
#define r5  (VITA_CPU.r5)
#define r6  (VITA_CPU.r6)
#define r7  (VITA_CPU.r7)
#define r8  (VITA_CPU.r8)
#define r9  (VITA_CPU.r9)
#define r10 (VITA_CPU.r10)
#define r11 (VITA_CPU.r11)
#define r12 (VITA_CPU.r12)
#define sp  (VITA_CPU.sp)
#define lr  (VITA_CPU.lr)
#define flag_n (VITA_CPU.flag_n)
#define flag_z (VITA_CPU.flag_z)
#define flag_c (VITA_CPU.flag_c)
#define flag_v (VITA_CPU.flag_v)
#define fpscr_n (VITA_CPU.fpscr_n)
#define fpscr_z (VITA_CPU.fpscr_z)
#define fpscr_c (VITA_CPU.fpscr_c)
#define fpscr_v (VITA_CPU.fpscr_v)
#define vfp_s (VITA_CPU.vfp_s)

#endif /* VITARECOMP_CPU_H */
