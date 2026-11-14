/* emit.c â€” ARM to C.
 *
 * The output is meant to be read. Every instruction carries its address and
 * disassembly as a comment above the C it became, because the first thing
 * anyone does with a recompiled function that misbehaves is compare it against
 * the original.
 *
 * Two things this does NOT do, deliberately:
 *
 *   - It does not guess. An instruction whose operands the decoder did not
 *     fill becomes a named trap, never an approximation. A gap that stops the
 *     program is worth far more than one that produces a program which runs and
 *     is wrong.
 *   - It does not reorder. ARM has no delay slots, so the translation is
 *     instruction-for-instruction, and the only control flow that moves is a
 *     branch becoming a goto.
 */

#include "emit.h"
#include "neon_emit.h"
#include "../../include/vitarecomp/neon.h"     /* neon_kind, shared with the runtime */

#include <stdlib.h>
#include <string.h>

extern uint32_t g_dbg_trace_addr;   /* VITA_DBGA (definida en main.c): depuración dirigida */

#define MAX_INSNS_PER_FUNC 65536

static const char *reg_name(int r) {
    static const char *n[16] = {
        "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
        "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc"
    };
    return (r >= 0 && r < 16) ? n[r] : "?";
}

/* --- collecting one function's instructions -------------------------------- */

typedef struct {
    arm_insn *v;
    uint32_t  n, cap;
    uint32_t *labels;
    uint32_t  nlabels, lcap;
} fbody;

static void fb_push(fbody *b, const arm_insn *in) {
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 256;
        b->v = (arm_insn *)realloc(b->v, b->cap * sizeof(arm_insn));
    }
    b->v[b->n++] = *in;
}

static void fb_label(fbody *b, uint32_t addr) {
    for (uint32_t i = 0; i < b->nlabels; i++)
        if (b->labels[i] == addr) return;
    if (b->nlabels == b->lcap) {
        b->lcap = b->lcap ? b->lcap * 2 : 64;
        b->labels = (uint32_t *)realloc(b->labels, b->lcap * sizeof(uint32_t));
    }
    b->labels[b->nlabels++] = addr;
}

static int fb_has_label(const fbody *b, uint32_t addr) {
    for (uint32_t i = 0; i < b->nlabels; i++)
        if (b->labels[i] == addr) return 1;
    return 0;
}

/* Collect a function by FOLLOWING ITS CONTROL FLOW, not by sweeping its extent.
 *
 * The distinction is the difference between a usable output and a 2.4 GB one.
 * `fn->end` is the highest address the discovery walk reached, and because a
 * tail call is a branch, that walk follows branches into other functions â€” so
 * an extent routinely spans code and data belonging to many functions. Sweeping
 * it linearly re-emits all of that under every function whose extent covers it,
 * and the same instruction lands in the output dozens of times.
 *
 * Flow-following emits each instruction once, in exactly the places it is
 * genuinely reachable, and stops at returns. Extent is still useful as a bound;
 * it is not a body.
 */

#define VISIT_SLOTS 65536

typedef struct { uint32_t k[VISIT_SLOTS]; } visited;

static int visit_mark(visited *v, uint32_t addr) {
    uint32_t h = (addr * 2654435761u) & (VISIT_SLOTS - 1);
    for (uint32_t i = 0; i < VISIT_SLOTS; i++) {
        uint32_t s = (h + i) & (VISIT_SLOTS - 1);
        if (v->k[s] == addr) return 0;          /* already seen */
        if (v->k[s] == 0)    { v->k[s] = addr; return 1; }
    }
    return 0;                                    /* table full: stop expanding */
}

static int cmp_addr(const void *a, const void *b) {
    uint32_t x = ((const arm_insn *)a)->addr, y = ((const arm_insn *)b)->addr;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* --- the set of addresses that are actually functions ----------------------- */
/*
 * The emitter must never name a symbol it does not define. A call target only
 * becomes `vita_func_<addr>()` if discovery registered it; anything else is a
 * trap.
 *
 * This is not defensive tidying — it is the only correct answer. A linear sweep
 * decodes literal pools and tables as instructions, and some of that data
 * decodes as a BL with a plausible target. Two such targets in this module land
 * *inside the import table*, one of them 0x32 bytes into the first entry: not
 * code, never called at run time, and pure decoding artefact. Emitting calls to
 * them produced C that compiled and then failed to link.
 */

typedef struct { uint32_t *v; uint32_t n; } funcset;

static int fs_cmp(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static int fs_has(const funcset *s, uint32_t addr) {
    uint32_t lo = 0, hi = s->n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (s->v[mid] == addr) return 1;
        if (s->v[mid] <  addr) lo = mid + 1; else hi = mid;
    }
    return 0;
}

/* First function start after `addr`, or 0. */
static uint32_t fs_next(const funcset *s, uint32_t addr) {
    uint32_t lo = 0, hi = s->n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (s->v[mid] <= addr) lo = mid + 1; else hi = mid;
    }
    return lo < s->n ? s->v[lo] : 0;
}

/* Addresses promoted to functions of their own (see collect). Open addressing,
 * plus the insertion order so the caller can walk the new ones. */
typedef struct { uint32_t *k; uint32_t cap; uint32_t *list; uint32_t n, lcap; } xset;

static int xs_has(const xset *s, uint32_t a) {
    if (!s || !s->n) return 0;
    for (uint32_t h = (a * 2654435761u) & (s->cap - 1);; h = (h + 1) & (s->cap - 1)) {
        if (s->k[h] == a) return 1;
        if (s->k[h] == 0) return 0;
    }
}

static void xs_add(xset *s, uint32_t a) {
    uint32_t h = (a * 2654435761u) & (s->cap - 1);
    while (s->k[h] && s->k[h] != a) h = (h + 1) & (s->cap - 1);
    if (s->k[h] == a || s->n >= s->cap / 2) return;   /* present, or full: leave it */
    s->k[h] = a;
    if (s->n == s->lcap) {
        s->lcap = s->lcap ? s->lcap * 2 : 4096;
        s->list = (uint32_t *)realloc(s->list, s->lcap * sizeof(uint32_t));
    }
    s->list[s->n++] = a;
}

/* How far a function's walk may treat code as its own.
 *
 * A function registered because something called it may have been walked by
 * discovery only briefly — its recorded extent can be zero or tiny. The body is
 * still real, so fall back to a generous bound and let control flow terminate
 * the walk, which it does at the first return.
 *
 * Discovery's extent is where ITS walk stopped, and that walk stops at anything
 * another function's walk already decoded — so a long function whose epilogue
 * is shared by many paths can come out cut short. Functions are laid out
 * contiguously, so the code up to the next function start past the extent
 * still belongs to this one (stray starts inside the body do not count).
 *
 * Computed once from discovery's functions and frozen: the promoted entries
 * added later must not move any bound, or the pre-pass and the emit would
 * disagree about which branches leave a function. */
static uint32_t fn_bound(const vf_func *fn, const funcset *fs0) {
    uint32_t bound = (fn->end > fn->addr + 16) ? fn->end : fn->addr + 8192;
    uint32_t next = fs_next(fs0, bound - 1);
    if (next > bound) bound = next;
    return bound;
}

/* Walk one function's body.
 *
 * Flow that leaves `bound` either reaches a known function — a tail call — or
 * code that belongs to no known function. Following that code inline copied it
 * into every function that reached it: the output grew from 14.5 M to 24 M
 * instructions and the build from 4 to 15 minutes. Instead it is PROMOTED: with
 * `pend` set (the pre-pass) the address is recorded and becomes a function of
 * its own, emitted once, and the branch or fall-through to it is a tail call.
 * Entering guest code at any instruction boundary through a C call is sound —
 * registers and stack are guest state, and its return returns for us.
 *
 * `known` must be the final function set in the emit pass; then no promotion
 * is pending and the old inline-follow path is only a fallback. */
static void collect(const vm_image *img, const vf_func *fn, uint32_t bound,
                    const funcset *known, const funcset *real, xset *pend, fbody *b) {
    const uint8_t *text = img->data + img->seg_file;
    const uint32_t base = img->seg_vaddr;

    visited seen;
    memset(&seen, 0, sizeof(seen));

    /* Pending block starts. Large and static: a 512-entry stack silently dropped
     * targets in long functions, leaving their branches with no label. */
    static uint32_t stack[65536];
    int sp_ = 0;
    stack[sp_++] = fn->addr;

    while (sp_ > 0 && b->n < MAX_INSNS_PER_FUNC) {
        uint32_t a = stack[--sp_];
        extern uint32_t g_dbg_trace_addr;
        if (g_dbg_trace_addr && a == g_dbg_trace_addr)
            fprintf(stderr, "[collect] pop objetivo %08X fn=%08X\n", a, fn->addr);
        arm_it its;
        arm_it_reset(&its);              /* a new block never starts inside IT */
        int cmp_reg = -1;
        uint32_t cmp_imm = 0;
        int prev_reg = -1, lo_check = 0;
        uint32_t prev_imm = 0;

        for (;;) {
            if (a < base || a >= base + img->seg_len) break;
            if (a + 0x10000u <= fn->addr || a >= fn->addr + 0x10000u) break;
            if (visit_mark(&seen, a)) {
                extern uint32_t g_dbg_trace_addr;
                if (g_dbg_trace_addr && (a == g_dbg_trace_addr || a + 2 == g_dbg_trace_addr || a + 4 == g_dbg_trace_addr))
                    fprintf(stderr, "[collect] visita %08X\n", a);
            } else {
                extern uint32_t g_dbg_trace_addr;
                if (g_dbg_trace_addr && (a == g_dbg_trace_addr || a + 2 == g_dbg_trace_addr || a + 4 == g_dbg_trace_addr))
                    fprintf(stderr, "[collect] %08X YA EMITIDO aquí paro\n", a);
                break;        /* already emitted */
            }

            /* Past the extent, flow that reaches a known function start is a
             * fall-through into it: emit it as an explicit tail call. Stopping
             * there silently instead ended the C function — a `return` the
             * guest never executed. Anywhere else past the extent is still this
             * function's code (the extent is cut short by stray starts). */
            /* A REAL function start (a call target or export, not a shape guess)
             * reached inside the bound is another function: discovery's extent
             * routinely spans several, and walking into them copied each one
             * into every function whose extent covered it — 3.7 copies of every
             * instruction on average. Shape-heuristic starts are still walked
             * through: they can sit inside a loop, and splitting there would
             * make every iteration a nested C call. */
            int real_a = a != fn->addr && fs_has(real, a);
            int known_a = fs_has(known, a) || xs_has(pend, a);
            if (a >= bound && !known_a && pend) { xs_add(pend, a); known_a = 1; }
            if (real_a || (a >= bound && known_a)) {
                arm_insn ft;
                memset(&ft, 0, sizeof(ft));
                ft.addr = a;
                ft.mode = fn->mode;
                ft.op = OP_B;
                ft.cls = A_BRANCH;
                ft.cond = ARM_COND_AL;
                ft.has_target = 1;
                ft.target = a;
                ft.rd = ft.rn = ft.rm = ft.rt = ft.rt2 = ARM_NO_REG;
                ft.vd = ft.vn = ft.vm = ARM_NO_REG;
                ft.mnemonic = "fall-through";
                fb_push(b, &ft);
                break;
            }

            arm_insn in;
            int n = arm_decode(text, base, img->seg_len, a, fn->mode, &in);
            if (n == 0) break;
            arm_it_apply(&its, &in);
            /* `cmp r0, #0x50; bgt out; cmp r0, #0x1f; ble out; subs r0, #0x20; tbh`: the
             * second cmp is a LOWER bound. Taking it as the count read 32 of 49 entries.
             * A cmp whose very next instruction branches on le/lt/ls/lo keeps the upper
             * bound already known for that register. */
            if (lo_check) {
                lo_check = 0;
                if (in.cls == A_BRANCH && in.has_target && prev_reg == cmp_reg &&
                    (in.cond == 0xD || in.cond == 0xB || in.cond == 0x9 || in.cond == 0x3)) cmp_imm = prev_imm;
            }
            if (in.op == OP_CMP && in.has_imm) {
                prev_reg = cmp_reg; prev_imm = cmp_imm;
                cmp_reg = in.rn; cmp_imm = in.imm; lo_check = 1;
            }
            /* `cmp r2, #44; ...; subs r2, #10; tbb [pc, r2]`: the index is 0..34. Reading 45
             * entries ran into the padding after the table and the table was dropped. */
            else if (in.op == OP_SUB && in.has_imm && cmp_reg >= 0 && in.rn == cmp_reg && in.imm <= cmp_imm) {
                cmp_imm -= in.imm;
                cmp_reg = in.rd;
            }
            if (in.op == OP_TBB || in.op == OP_TBH) {
                /* A jump table: every entry is a block of this function. The
                 * entry count travels in `imm` so the emitter reads the same
                 * table back. */
                uint32_t tt[1024];
                uint32_t cnt = (cmp_reg == in.rm) ? cmp_imm + 1 : 0;
                int nt = arm_table_targets(text, base, img->seg_len, &in, cnt, tt, 1024);
                in.imm = (uint32_t)nt;
                fb_push(b, &in);
                for (int k = 0; k < nt; k++) {
                    if (tt[k] < fn->addr) continue;      /* targets were validated by the reader */
                    fb_label(b, tt[k]);
                    if (sp_ < (int)(sizeof(stack) / sizeof(stack[0]))) stack[sp_++] = tt[k];
                }
                break;
            }
            fb_push(b, &in);

            if (in.cls == A_RETURN && !in.conditional) break;   /* `bxeq lr` in IT falls through */

            /* `bound`, not `fn->end`.
             *
             * These must be the same notion of "inside this function" or the
             * walk and the labelling disagree. The walk above collects up to
             * `bound`, which falls back to a generous range when discovery
             * recorded only a tiny extent — and it records a tiny extent
             * whenever an earlier function's walk had already marked those
             * instructions seen, which is common.
             *
             * Gating labels on `fn->end` instead meant that for exactly those
             * functions, instructions were collected but branches into them got
             * no label. Each then fell through to the call-or-trap path and
             * trapped: 2,547 instructions, and every one of them an ordinary
             * intra-function branch that had already been translated correctly
             * a few lines above. */
            /* Past the bound, a branch to a known function start is a tail
             * call and the emitter calls it. A branch anywhere else is still
             * this function's code — the bound was cut short by a stray start
             * inside the body (shape heuristics find many) — so it is followed
             * too, within 64 KB, rather than left to trap. */
            int near = in.has_target && in.target + 0x10000u > fn->addr &&
                       in.target < fn->addr + 0x10000u &&
                       in.target >= base && in.target < base + img->seg_len;
            int out_known = in.has_target &&
                            (fs_has(known, in.target) || xs_has(pend, in.target));
            if (in.has_target && in.cls == A_BRANCH && near && pend && !out_known &&
                !(in.target >= fn->addr && in.target < bound))
                xs_add(pend, in.target);              /* promote: emitted as a tail call */
            if (in.has_target && in.cls == A_BRANCH &&
                ((in.target >= fn->addr && in.target < bound) ||
                 (!pend && !out_known && near))) {
                fb_label(b, in.target);
                if (in.target == 0x8129990E) { extern uint32_t g_dbg_trace_addr; if (g_dbg_trace_addr) fprintf(stderr, "[collect] etiqueta %08X desde %08X (cond=%d alumno bound=%08X)\n", in.target, in.addr, in.conditional, bound); }
                if (sp_ < (int)(sizeof(stack) / sizeof(stack[0])))
                    stack[sp_++] = in.target;
                if (!in.conditional) break;          /* flow ends here */
            } else if (in.cls == A_BRANCH && !in.conditional) {
                break;                               /* tail call */
            }

            a += (uint32_t)n;
        }
    }

    /* Blocks were discovered out of order; emit in address order so the output
     * reads alongside a disassembly. */
    if (b->n > 1) qsort(b->v, b->n, sizeof(arm_insn), cmp_addr);

    /* Drop labels for blocks that were never collected.
     *
     * A label is recorded the moment a branch target is seen inside the
     * function's extent, but the block behind it is only reached if the walk
     * gets there — and the walk can stop short at any of three limits: the
     * visited table filling, the block stack filling, or the per-function
     * instruction cap. When that happens the label is referenced by a `goto`
     * and never placed, which is C that does not compile.
     *
     * Pruning here rather than raising the limits is the honest fix: the limits
     * exist for a reason, and a `goto` into a block we did not translate would
     * be a lie even if the compiler accepted it. Removing the label routes the
     * branch through the call-or-trap path below, which says what is actually
     * true — either the target is a function, or we cannot get there.
     *
     * This only showed up at 1,500 functions. At 100 nothing reached a limit,
     * so the output compiled and the bug was invisible. */
    uint32_t kept = 0;
    for (uint32_t i = 0; i < b->nlabels; i++) {
        int placed = 0;
        uint32_t lo = 0, hi = b->n;        while (lo < hi) {                      /* body is sorted by address */
            uint32_t mid = lo + (hi - lo) / 2;
            if (b->v[mid].addr == b->labels[i]) { placed = 1; break; }
            if (b->v[mid].addr <  b->labels[i]) lo = mid + 1; else hi = mid;
        }
        if (placed) b->labels[kept++] = b->labels[i];
        else if (g_dbg_trace_addr && b->labels[i] == g_dbg_trace_addr)
            fprintf(stderr, "[collect] etiqueta %08X descartada: bloque no coleccionado (fn=%08X insns=%u)\n",
                    b->labels[i], fn->addr, b->n);
    }
    b->nlabels = kept;
}

/* --- operand rendering ------------------------------------------------------ */

/* A register operand, with r15 resolved.
 *
 * A recompiled program has no PC, so `pc` is not a variable and emitting it
 * produces C that does not compile â€” which is the good failure. The value is
 * known at translation time: reading PC in Thumb yields the instruction's own
 * address plus 4. The high-register ADD/MOV/CMP forms are where this actually
 * shows up, and they are common enough (position-independent address
 * computation) that leaving them to a trap would cost real coverage. */
static void reg_operand(char *dst, size_t cap, int r, const arm_insn *in) {
    if (r == 15) snprintf(dst, cap, "0x%08Xu", in->addr + 4);
    else         snprintf(dst, cap, "%s", reg_name(r));
}

/* A SOURCE register as a C expression, for the many places that want one
 * inline in an fprintf rather than in a buffer.
 *
 * The division this enforces is the one that kept getting lost: a DESTINATION
 * of r15 is a branch and is trapped by the guard in emit_insn, while a SOURCE
 * of r15 is a CONSTANT this pass knows — the instruction's own address plus
 * four, which is how the architecture defines reading the PC. Naming it "pc"
 * is never right in either case, and reg_name() is now left naming only
 * destinations.
 *
 * Rotating buffers because two sources can appear in one fprintf; the most any
 * call site uses is two. */
static const char *src_name(const arm_insn *in, int r) {
    static char buf[4][32];
    static unsigned next;
    if (r != 15) return reg_name(r);
    char *b = buf[next++ & 3];
    snprintf(b, sizeof(buf[0]), "0x%08Xu", in->addr + 4);
    return b;
}

/* The second operand of a data-processing instruction: an immediate, a plain
 * register, or a shifted register. */
static void operand2(char *dst, size_t cap, const arm_insn *in) {
    if (in->has_imm) {
        snprintf(dst, cap, "0x%X", in->imm);
        return;
    }
    if (in->rm == ARM_NO_REG) { snprintf(dst, cap, "/*?*/0"); return; }
    if (in->shift_amt == 0 && in->shift_type == SH_LSL) {
        reg_operand(dst, cap, in->rm, in);
        return;
    }
    if (in->rm == 15) { reg_operand(dst, cap, 15, in); return; }
    if (in->shift_type == SH_ROR && in->shift_amt == 0) {    /* RRX */
        snprintf(dst, cap, "(((uint32_t)flag_c << 31) | (%s >> 1))", src_name(in, in->rm));
        return;
    }
    static const char *fn[4] = { "vita_lsl", "vita_lsr", "vita_asr", "vita_ror" };
    snprintf(dst, cap, "%s(%s, %u)", fn[in->shift_type & 3],
             src_name(in, in->rm), in->shift_amt);
}

/* The address expression for a load or store. */
static int address(char *dst, size_t cap, const arm_insn *in) {
    if (in->rn == ARM_NO_REG) return 0;

    if (in->rn == 15) {
        /* PC-relative. A recompiled program has no PC, so it is resolved here:
         * for Thumb the base is the instruction address plus 4, aligned down to
         * a word. This is what turns a literal-pool load into a constant.
         *
         * Every caller must come through here rather than formatting `rn`
         * itself: `reg_name(15)` is "pc", and cpu.h deliberately has no such
         * variable, so a caller that builds its own address expression emits C
         * that does not compile. That is the good failure — the bad one is a
         * `pc` that exists and holds something plausible. */
        uint32_t pc = (in->addr + 4) & ~3u;
        snprintf(dst, cap, "0x%08X", in->mem_add ? pc + in->imm : pc - in->imm);
        return 1;
    }
    /* The U bit is not decoration: the T32 8-bit form encodes [rN, #-imm]
     * with the same fields as the positive one, so ignoring it reads from the
     * wrong side of the base register. */
    if (in->idx_mode == 2)                       /* post-index: the base itself */
        snprintf(dst, cap, "%s", src_name(in, in->rn));
    else if (in->has_imm && in->imm)
        snprintf(dst, cap, "%s %c 0x%X", src_name(in, in->rn),
                 in->mem_add ? '+' : '-', in->imm);
    else if (in->rm != ARM_NO_REG && in->shift_amt)
        /* `[rn, rm, lsl #2]` is how every word array is indexed. Dropping
         * the shift read element i/4 instead of element i. */
        snprintf(dst, cap, "%s + (%s << %u)", src_name(in, in->rn), src_name(in, in->rm),
                 in->shift_amt);
    else if (in->rm != ARM_NO_REG)
        snprintf(dst, cap, "%s + %s", src_name(in, in->rn), src_name(in, in->rm));
    else
        snprintf(dst, cap, "%s", src_name(in, in->rn));
    return 1;
}

/* --- one instruction --------------------------------------------------------- */

/* Mnemonics are static strings, so the pointer identifies the kind and the
 * table stays a short linear scan. */
static void note_trap(emit_stats *st, const char *what) {
    if (!what) what = "?";
    for (uint32_t i = 0; i < st->trap_kinds; i++) {
        if (st->traps[i].what == what) { st->traps[i].count++; return; }
    }
    if (st->trap_kinds < EM_MAX_TRAP_KINDS) {
        st->traps[st->trap_kinds].what  = what;
        st->traps[st->trap_kinds].count = 1;
        st->trap_kinds++;
        return;
    }
    st->traps_other++;
}

static void trap(FILE *f, const arm_insn *in, const char *why) {
    fprintf(f, "    vita_trap_unimpl(0x%08X, 0x%08X, \"%s\");\n",
            in->addr, in->raw, why);
}

/* Returns 1 if real C was emitted, 0 if a trap was. */
/* The C symbol for an imported firmware function.
 *
 * The resolved name is used when the database knows it, because the whole point
 * of binding imports is that the generated C says `vita_hle_sceGxmDraw()` where
 * it would otherwise say `vita_func_814BB75C()` and leave the reader to work
 * out that the callee is not ours at all. An unresolved NID still gets a stable
 * symbol so the output links either way. */
static void hle_symbol(char *dst, size_t cap, const nid_db *db,
                       const vm_stub *s) {
    const char *n = nid_func_name(db, s->lib_nid, s->func_nid);
    if (n) snprintf(dst, cap, "vita_hle_%s", n);
    else   snprintf(dst, cap, "vita_hle_nid_%08X", s->func_nid);
}

static int emit_insn(const vm_image *img, const vm_module *mod, const nid_db *db,
                     const funcset *fs, const fbody *b,
                     const arm_insn *in, FILE *f, emit_stats *st) {
    char o2[64], addr[64], rn_s[32];

    /* A MOVW/MOVT the loader would relocate to an imported variable: build
     * that variable's cell address instead of the linker's placeholder. */
    arm_insn reloc;
    const vm_varref *vr = vm_find_varref(mod, in->addr);
    if (vr && (in->op == OP_MOV || in->op == OP_MOVT) && in->has_imm) {
        reloc = *in;
        reloc.imm = vr->hi ? (vr->cell >> 16) : (vr->cell & 0xFFFFu);
        in = &reloc;
        st->varrefs++;
    }

    /* The first operand, with r15 resolved. Computed once because several
     * paths below need it and the ternary for "rn defaults to rd" is easy to
     * get inconsistently wrong when repeated. */
    reg_operand(rn_s, sizeof(rn_s),
                in->rn == ARM_NO_REG ? in->rd : in->rn, in);

    /* A call whose target is an import stub is a firmware call, and must be
     * bound before anything else looks at it. Otherwise it decodes as an
     * ordinary call to an ordinary address — the stub body is a placeholder the
     * loader overwrites at load time, so recompiling it would translate filler
     * and then "return" into whatever the filler happened to be. */
    if ((in->cls == A_CALL || in->cls == A_BRANCH) && in->has_target) {
        const vm_stub *s = vm_find_stub(mod, in->target);
        if (s) {
            char sym[96];
            hle_symbol(sym, sizeof(sym), db, s);
            if (in->cls == A_CALL) fprintf(f, "    %s();\n", sym);
            else                   fprintf(f, "    %s(); return;\n", sym);  /* tail call */
            st->import_calls++;
            return 1;
        }
    }

    /* A destination of r15 is a write to the PC, which is a BRANCH. The
     * decoder classifies the real ones as branches, so anything still carrying
     * r15 as a destination here is an encoding this emitter does not model —
     * and emitting it would print `pc`, which cpu.h deliberately does not
     * define.
     *
     * One guard rather than one per case, because one per case is what was
     * tried and it kept not holding: this is the FOURTH distinct path to reach
     * a build emitting a bare `pc` (MOV pc,rN; VLDR from a literal pool; STRD;
     * and MOVT/MOVW/ADDW). Each new translation had to remember the rule
     * independently, and eventually one did not. Checking it in the one place
     * every translation passes through is the only version that stays fixed.
     *
     * Source operands are not covered here and must not be: reg_operand()
     * resolves rm == 15 to the instruction's own address at translation time,
     * which is correct and is how PC-relative arithmetic works at all.
     *
     * Scoped in two ways, both learned by getting it wrong. `rt == 15` only
     * means the PC in a LOAD or STORE: in `VMRS APSR_nzcv, FPSCR` the same
     * field encodes "the condition flags", and a blanket rule trapped 3,827
     * working instructions. And an instruction the decoder already left as
     * OP_NONE is going to trap anyway with a more specific name than this one,
     * so it is left alone rather than relabelled. */
    int writes_pc = (in->rd == 15)
                 || ((in->cls == A_LOAD || in->cls == A_STORE)
                     && (in->rt == 15 || in->rt2 == 15));
    if (in->op != OP_NONE && writes_pc) {
        trap(f, in, "writes pc");
        note_trap(st, "writes pc");
        return 0;
    }

    /* An instruction made conditional by an IT block, or a conditional branch,
     * is wrapped rather than translated differently. Getting this wrong is
     * invisible until a condition happens to be false. */
    int guarded = 0;
    if (in->cls != A_BRANCH && in->cond != ARM_COND_AL && in->op != OP_IT) {
        fprintf(f, "    if (vita_cond(%u)) {\n", in->cond);
        guarded = 1;
    }

    /* Flag-setting logical operations take C from the barrel shifter when
     * the second operand is a shifted register. Computed before the
     * destination is written, since rd is often rm. */
    int shc = in->sets_flags && !in->has_imm && in->rm != ARM_NO_REG && in->rm != 15 &&
              (in->shift_amt || in->shift_type == SH_ROR) &&
              (in->op == OP_MOV || in->op == OP_MVN || in->op == OP_AND || in->op == OP_ORR ||
               in->op == OP_EOR || in->op == OP_BIC || in->op == OP_TST || in->op == OP_TEQ);
    if (shc) {
        if (in->shift_type == SH_ROR && in->shift_amt == 0)
            fprintf(f, "    { int _shc = (int)(%s & 1);\n", src_name(in, in->rm));
        else
            fprintf(f, "    { int _shc = vita_shift_c(%u, %s, %u, flag_c);\n",
                    in->shift_type, src_name(in, in->rm), in->shift_amt);
    }

    int ok = 1;
    switch (in->op) {
        case OP_MOV:
            operand2(o2, sizeof(o2), in);
            fprintf(f, "    %s = %s;\n", reg_name(in->rd), o2);
            if (in->sets_flags) fprintf(f, "    vita_flags_nz(%s);\n", reg_name(in->rd));
            break;

        case OP_MOVT:
            /* Top half written, bottom half preserved. One line, and the
             * difference between a program that builds correct pointers and one
             * whose every constructed address is missing its high 16 bits. */
            fprintf(f, "    %s = (%s & 0x0000FFFFu) | 0x%08Xu;\n",
                    reg_name(in->rd), reg_name(in->rd), in->imm << 16);
            break;

        case OP_MVN:
            operand2(o2, sizeof(o2), in);
            fprintf(f, "    %s = ~(%s);\n", reg_name(in->rd), o2);
            if (in->sets_flags) fprintf(f, "    vita_flags_nz(%s);\n", reg_name(in->rd));
            break;

        case OP_ADD: case OP_SUB: {
            operand2(o2, sizeof(o2), in);
            const char *rn = rn_s;
            if (in->sets_flags) {
                /* The operands have to be captured before the destination is
                 * written, because rd and rn are frequently the same register
                 * and the flag computation needs the ORIGINAL values. */
                fprintf(f, "    { uint32_t _a = %s, _b = %s;\n", rn, o2);
                fprintf(f, "      %s = _a %c _b;\n", reg_name(in->rd),
                        in->op == OP_ADD ? '+' : '-');
                fprintf(f, "      vita_flags_%s(_a, _b, %s); }\n",
                        in->op == OP_ADD ? "add" : "sub", reg_name(in->rd));
            } else {
                fprintf(f, "    %s = %s %c %s;\n", reg_name(in->rd), rn,
                        in->op == OP_ADD ? '+' : '-', o2);
            }
            break;
        }

        case OP_ADC: case OP_SBC: {
            const char *rn = rn_s;
            operand2(o2, sizeof(o2), in);
            fprintf(f, "    { uint32_t _a = %s, _b = %s, _c = (uint32_t)flag_c;\n", rn, o2);
            if (in->op == OP_ADC) {
                fprintf(f, "      %s = _a + _b + _c;\n", reg_name(in->rd));
                if (in->sets_flags)
                    fprintf(f, "      vita_flags_adc(_a, _b, _c, %s);\n", reg_name(in->rd));
            } else {
                fprintf(f, "      %s = _a - _b - (1u - _c);\n", reg_name(in->rd));
                if (in->sets_flags)
                    fprintf(f, "      vita_flags_sub(_a, _b + (1u - _c), %s);\n", reg_name(in->rd));
            }
            fprintf(f, "    }\n");
            break;
        }

        case OP_RSB:
            /* rd = operand2 - rn. The 16-bit NEG form decodes as operand2 = #0;
             * hard-wiring 0 here turned every RSB #imm (rsbs r1, r1, #31) into
             * a negate. */
            operand2(o2, sizeof(o2), in);
            fprintf(f, "    { uint32_t _a = %s, _b = %s;\n", o2, src_name(in, in->rn));
            fprintf(f, "      %s = _a - _b;\n", reg_name(in->rd));
            if (in->sets_flags)
                fprintf(f, "      vita_flags_sub(_a, _b, %s);\n", reg_name(in->rd));
            fprintf(f, "    }\n");
            break;

        case OP_AND: case OP_ORR: case OP_EOR: case OP_BIC: {
            operand2(o2, sizeof(o2), in);
            const char *rn = rn_s;
            const char *ops = in->op == OP_AND ? "&" : in->op == OP_ORR ? "|" : "^";
            if (in->op == OP_BIC)
                fprintf(f, "    %s = %s & ~(%s);\n", reg_name(in->rd), rn, o2);
            else
                fprintf(f, "    %s = %s %s %s;\n", reg_name(in->rd), rn, ops, o2);
            if (in->sets_flags) fprintf(f, "    vita_flags_nz(%s);\n", reg_name(in->rd));
            break;
        }

        case OP_LSL: case OP_LSR: case OP_ASR: case OP_ROR: {
            static const char *fn2[4] = { "vita_lsl", "vita_lsr", "vita_asr", "vita_ror" };
            int which = in->op == OP_LSL ? 0 : in->op == OP_LSR ? 1
                      : in->op == OP_ASR ? 2 : 3;
            /* Immediate form: the value is rm. Register form: the value is rn
             * (the 16-bit form decodes rn = rd) and rm is the amount; shifting rd
             * instead was wrong for every LSL.W/LSR.W rd, rn, rm. */
            char amt[64];
            const char *src;
            if (in->has_imm) {
                src = src_name(in, in->rm == ARM_NO_REG ? in->rd : in->rm);
                snprintf(amt, sizeof(amt), "%u", in->imm);
            } else {
                src = src_name(in, in->rn != ARM_NO_REG ? in->rn : in->rd);
                snprintf(amt, sizeof(amt), "(%s & 0xFF)", src_name(in, in->rm));
            }
            /* Through temporaries: the carry needs the original value and amount. */
            fprintf(f, "    { uint32_t _x = %s, _s = %s;\n", src, amt);
            fprintf(f, "      %s = %s(_x, _s);\n", reg_name(in->rd), fn2[which]);
            if (in->sets_flags) {
                fprintf(f, "      vita_flags_nz(%s);\n", reg_name(in->rd));
                fprintf(f, "      flag_c = vita_shift_c(%d, _x, _s, flag_c);\n", which);
            }
            fprintf(f, "    }\n");
            break;
        }

        case OP_MUL:
            /* rd = rn * rm. The 16-bit form decodes rn = rd, so this covers both;
             * multiplying by rd instead ignored the first operand of every MUL.W. */
            fprintf(f, "    %s = %s * %s;\n", reg_name(in->rd),
                    src_name(in, in->rn != ARM_NO_REG ? in->rn : in->rd), src_name(in, in->rm));
            if (in->sets_flags) fprintf(f, "    vita_flags_nz(%s);\n", reg_name(in->rd));
            break;

        case OP_CMP: case OP_CMN:
            operand2(o2, sizeof(o2), in);
            fprintf(f, "    { uint32_t _a = %s, _b = %s;\n",
                    rn_s, o2);
            fprintf(f, "      vita_flags_%s(_a, _b, _a %c _b); }\n",
                    in->op == OP_CMP ? "sub" : "add",
                    in->op == OP_CMP ? '-' : '+');
            break;

        case OP_TST: case OP_TEQ:
            operand2(o2, sizeof(o2), in);
            fprintf(f, "    vita_flags_nz(%s %c %s);\n",
                    rn_s, in->op == OP_TEQ ? '^' : '&', o2);
            break;

        case OP_ADR:
            fprintf(f, "    %s = 0x%08X;\n", reg_name(in->rd),
                    ((in->addr + 4) & ~3u) + in->imm);
            break;

        case OP_LDR: case OP_LDRB: case OP_LDRH: case OP_LDRSB: case OP_LDRSH: {
            if (!address(addr, sizeof(addr), in)) { ok = 0; trap(f, in, "load addr"); break; }
            /* A literal-pool load has a fixed address, so the value is known at
             * translation time and becomes a constant rather than a memory
             * access. This is also what stops the pool from being mistaken for
             * code later. */
            if (in->rn == 15) {
                uint32_t pc = (in->addr + 4) & ~3u;
                uint32_t at = in->mem_add ? pc + in->imm : pc - in->imm;
                int sz = in->op == OP_LDR ? 4 : (in->op == OP_LDRH || in->op == OP_LDRSH) ? 2 : 1;
                const uint8_t *p = vm_va(img, at, (uint32_t)sz);
                if (p) {
                    uint32_t v = sz == 4 ? (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                                         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24)
                               : sz == 2 ? (uint32_t)p[0] | ((uint32_t)p[1] << 8) : p[0];
                    if (in->op == OP_LDRSB) v = (uint32_t)(int32_t)(int8_t)v;
                    if (in->op == OP_LDRSH) v = (uint32_t)(int32_t)(int16_t)v;
                    if (in->rt == 15) { ok = 0; break; }
                    fprintf(f, "    %s = 0x%08X;  /* literal */\n", reg_name(in->rt), v);
                    st->literals++;
                    break;
                }
            }
            const char *rd = in->op == OP_LDR   ? "vita_read32(%s)"
                           : in->op == OP_LDRB  ? "vita_read8(%s)"
                           : in->op == OP_LDRH  ? "vita_read16(%s)"
                           : in->op == OP_LDRSB ? "vita_sxtb(vita_read8(%s))"
                                                : "vita_sxth(vita_read16(%s))";
            if (in->rt == 15) { ok = 0; break; }
            if (in->idx_mode) {
                /* The base moves: through a temporary, so a loaded register
                 * that is also the base cannot change the address. */
                fprintf(f, "    { uint32_t _ea = %s;\n      %s = ", addr, reg_name(in->rt));
                fprintf(f, rd, "_ea");
                fprintf(f, ";\n      %s = _ea", reg_name(in->rn));
                if (in->idx_mode == 2) fprintf(f, " %c 0x%X", in->mem_add ? '+' : '-', in->imm);
                fprintf(f, "; }\n");
                break;
            }
            fprintf(f, "    %s = ", reg_name(in->rt));
            fprintf(f, rd, addr);
            fprintf(f, ";\n");
            break;
        }

        case OP_LDRD: case OP_STRD: {
            if (in->rn == ARM_NO_REG || in->rt2 == ARM_NO_REG) { ok = 0; break; }
            /* rt/rt2 are named as variables, so neither may be the PC — there
             * is no `pc` in cpu.h to load into or store from, deliberately. */
            if (in->rt == 15 || in->rt2 == 15) { ok = 0; trap(f, in, "ldrd/strd pc"); break; }
            /* Two adjacent words. The offset is added or subtracted per the U
             * bit, and the second register is always four bytes above the
             * first regardless of direction.
             *
             * Through the shared address(), which is the only thing that knows
             * how to resolve a PC-relative base. Building the string here is
             * what put a bare `pc` in the output — the same mistake VLDR made,
             * in the third place it could be made. */
            char base[64];
            if (!address(base, sizeof(base), in)) { ok = 0; trap(f, in, "ldrd/strd addr"); break; }

            fprintf(f, "    { uint32_t _ea = %s;\n", base);
            if (in->op == OP_LDRD) {
                fprintf(f, "      %s = vita_read32(_ea);\n", reg_name(in->rt));
                fprintf(f, "      %s = vita_read32(_ea + 4);\n", reg_name(in->rt2));
            } else {
                fprintf(f, "      vita_write32(_ea, %s);\n", reg_name(in->rt));
                fprintf(f, "      vita_write32(_ea + 4, %s);\n", reg_name(in->rt2));
            }
            if (in->idx_mode && in->rn != 15) {
                fprintf(f, "      %s = _ea", reg_name(in->rn));
                if (in->idx_mode == 2) fprintf(f, " %c 0x%X", in->mem_add ? '+' : '-', in->imm);
                fprintf(f, ";\n");
            }
            fprintf(f, "    }\n");
            break;
        }

        case OP_STR: case OP_STRB: case OP_STRH: {
            if (!address(addr, sizeof(addr), in)) { ok = 0; trap(f, in, "store addr"); break; }
            const char *w = in->op == OP_STR ? "vita_write32"
                          : in->op == OP_STRB ? "vita_write8" : "vita_write16";
            { char rv[64]; reg_operand(rv, sizeof(rv), in->rt, in);
              if (in->idx_mode) {
                  fprintf(f, "    { uint32_t _ea = %s;\n      %s(_ea, %s);\n      %s = _ea",
                          addr, w, rv, reg_name(in->rn));
                  if (in->idx_mode == 2) fprintf(f, " %c 0x%X", in->mem_add ? '+' : '-', in->imm);
                  fprintf(f, "; }\n");
              } else {
                  fprintf(f, "    %s(%s, %s);\n", w, addr, rv);
              } }
            break;
        }

        case OP_PUSH: {
            /* ARM pushes in descending register order to descending addresses,
             * so the lowest-numbered register ends up at the lowest address.
             * Emitting the stores in ascending order with a pre-decremented sp
             * gives the same layout without simulating the order. */
            int count = 0;
            for (int i = 0; i < 16; i++) if (in->reglist & (1u << i)) count++;
            fprintf(f, "    sp -= %d;\n", count * 4);
            int slot = 0;
            for (int i = 0; i < 16; i++) {
                if (!(in->reglist & (1u << i))) continue;
                /* A register list can name the PC. Its stored value is this
                 * instruction's own address plus four, which is known here —
                 * the same rule as any other PC source, and the reason this
                 * goes through reg_operand rather than reg_name. */
                { char rv[64]; reg_operand(rv, sizeof(rv), i, in);
                  fprintf(f, "    vita_write32(sp + %d, %s);\n", slot * 4, rv); }
                slot++;
            }
            break;
        }

        case OP_POP: {
            int slot = 0;
            for (int i = 0; i < 16; i++) {
                if (!(in->reglist & (1u << i))) continue;
                if (i == 15) { slot++; continue; }   /* PC: this is the return */
                fprintf(f, "    %s = vita_read32(sp + %d);\n", reg_name(i), slot * 4);
                slot++;
            }
            fprintf(f, "    sp += %d;\n", slot * 4);
            if (in->reglist & 0x8000) fprintf(f, "    return;\n");
            break;
        }

        case OP_SXTB: case OP_SXTH: case OP_UXTB: case OP_UXTH: {
            const char *fn3 = in->op == OP_SXTB ? "vita_sxtb"
                            : in->op == OP_SXTH ? "vita_sxth"
                            : in->op == OP_UXTB ? "vita_uxtb" : "vita_uxth";
            /* Wide forms may rotate the source first and may add rn (UXTAB...).
             * The 16-bit forms decode with no rotation and rn = ARM_NO_REG. */
            char src[64];
            if (in->shift_amt) snprintf(src, sizeof(src), "vita_ror(%s, %u)", src_name(in, in->rm), in->shift_amt);
            else               snprintf(src, sizeof(src), "%s", src_name(in, in->rm));
            if (in->rn != ARM_NO_REG && in->width == 4)
                fprintf(f, "    %s = %s + %s(%s);\n", reg_name(in->rd), src_name(in, in->rn), fn3, src);
            else
                fprintf(f, "    %s = %s(%s);\n", reg_name(in->rd), fn3, src);
            break;
        }

        case OP_REV:
            fprintf(f, "    %s = vita_rev(%s);\n", reg_name(in->rd), src_name(in, in->rm));
            break;

        case OP_REV16: case OP_REVSH: case OP_RBIT: case OP_CLZ:
            fprintf(f, "    %s = %s(%s);\n", reg_name(in->rd),
                    in->op == OP_REV16 ? "vita_rev16" : in->op == OP_REVSH ? "vita_revsh"
                  : in->op == OP_RBIT ? "vita_rbit" : "vita_clz", src_name(in, in->rm));
            break;

        case OP_SBFX: case OP_UBFX:
            fprintf(f, "    %s = %s(%s, %u, %u);\n", reg_name(in->rd),
                    in->op == OP_SBFX ? "vita_sbfx" : "vita_ubfx",
                    src_name(in, in->rn), in->bf_lsb, in->bf_width);
            break;

        case OP_BFI:
            fprintf(f, "    %s = vita_bfi(%s, %s, %u, %u);\n", reg_name(in->rd),
                    reg_name(in->rd), src_name(in, in->rn), in->bf_lsb, in->bf_width);
            break;

        case OP_BFC:
            fprintf(f, "    %s = vita_bfi(%s, 0, %u, %u);\n", reg_name(in->rd),
                    reg_name(in->rd), in->bf_lsb, in->bf_width);
            break;

        case OP_CBZ: case OP_CBNZ:
            if (fb_has_label(b, in->target))
                fprintf(f, "    if (%s %s 0) goto L_%08X;\n", src_name(in, in->rn),
                        in->op == OP_CBZ ? "==" : "!=", in->target);
            else
                if (fs_has(fs, in->target)) fprintf(f, "    if (%s %s 0) { vita_func_%08X(); return; }\n", src_name(in, in->rn), in->op == OP_CBZ ? "==" : "!=", in->target); else { fprintf(f, "    vita_trap_indirect(0x%08X, 0x%08X);\n", in->addr, in->target); ok = 0; }
            break;

        case OP_B:
            if (in->width == 0) {        /* synthetic fall-through into a known function */
                fprintf(f, "    vita_func_%08X(); return;\n", in->target);
                break;
            }
            if (fb_has_label(b, in->target)) {
                if (in->cond == ARM_COND_AL) fprintf(f, "    goto L_%08X;\n", in->target);
                else fprintf(f, "    if (vita_cond(%u)) goto L_%08X;\n",
                             in->cond, in->target);
            } else if (in->target < img->seg_vaddr ||
                       in->target >= img->seg_vaddr + img->seg_len) {
                /* Outside the segment entirely. This is not a branch the
                 * program makes — it is a linear sweep decoding data as code,
                 * which happens wherever a literal pool or a table sits inside
                 * .text. Naming it separately keeps it out of the count of
                 * things worth implementing. */
                fprintf(f, "    vita_trap_unimpl(0x%08X, 0x%08X, \"branch out of segment\");\n",
                        in->addr, in->raw);
                note_trap(st, "branch: target off-segment");
            } else {
                /* A branch out of this function's extent is a tail call. */
                if (in->cond == ARM_COND_AL)
                    if (fs_has(fs, in->target)) fprintf(f, "    vita_func_%08X(); return;\n", in->target); else { fprintf(f, "    vita_trap_indirect(0x%08X, 0x%08X);\n", in->addr, in->target); note_trap(st, "branch: target not a function"); }
                else
                    if (fs_has(fs, in->target)) fprintf(f, "    if (vita_cond(%u)) { vita_func_%08X(); return; }\n", in->cond, in->target); else { fprintf(f, "    vita_trap_indirect(0x%08X, 0x%08X);\n", in->addr, in->target); note_trap(st, "branch: target not a function"); }
            }
            break;

        case OP_BX: case OP_BLX:
            /* An indirect transfer through a register. The destination is a
             * value, so it can only be resolved while running — this is the one
             * place a recompiler genuinely needs a lookup rather than a
             * translation. BX is a tail transfer and does not come back; BLX is
             * a call and does. */
            if (in->rm == ARM_NO_REG) { ok = 0; break; }
            /* MLB 15 port: `bx lr` / `mov pc, lr` are returns. Calls are emitted
             * as C calls (`bl` does not set lr), so lr never holds the guest
             * return address and dispatching to it jumps to garbage. */
            if (in->op == OP_BX && in->cls == A_RETURN) {
                fprintf(f, "    return;\n");
                break;
            }
            /* Through reg_operand, not reg_name. `bx pc` is a real (if odd)
             * instruction, and as a SOURCE the PC is a constant this pass
             * knows — resolving it is correct where naming it would not
             * compile. */
            { char rm[64]; reg_operand(rm, sizeof(rm), in->rm, in);
              if (in->op == OP_BX) fprintf(f, "    vita_dispatch(0x%08X, %s); return;\n", in->addr, rm);
              else                 fprintf(f, "    vita_dispatch(0x%08X, %s);\n", in->addr, rm); }
            break;

        /* --- scalar VFP ---------------------------------------------------
         *
         * Emitted through accessors rather than as direct array indexing, so
         * the single/double aliasing lives in one place. `dp` selects between
         * the float and double forms of everything.
         */
        case OP_VLDR: case OP_VSTR: {
            if (in->vd == ARM_NO_REG) { ok = 0; break; }
            /* Through the shared address(), not a local snprintf. VFP constants
             * live in the same literal pools as integer ones, so `rn == 15` is
             * common here — and building the expression locally emitted a bare
             * `pc`, which does not compile. */
            char a[64];
            if (!address(a, sizeof(a), in)) { ok = 0; trap(f, in, "vldr addr"); break; }

            /* A VFP literal-pool load has a fixed address, so fold it to a
             * constant exactly as the integer path does — same reason, too:
             * it keeps the pool from being read back as code later. */
            if (in->rn == 15) {
                uint32_t pc = (in->addr + 4) & ~3u;
                uint32_t at = in->mem_add ? pc + in->imm : pc - in->imm;
                const uint8_t *p = vm_va(img, at, in->vfp_dp ? 8 : 4);
                if (p && in->op == OP_VLDR) {
                    for (int w = 0; w < (in->vfp_dp ? 2 : 1); w++) {
                        uint32_t v = (uint32_t)p[w*4] | ((uint32_t)p[w*4+1] << 8)
                                   | ((uint32_t)p[w*4+2] << 16) | ((uint32_t)p[w*4+3] << 24);
                        if (in->vfp_dp)
                            fprintf(f, "    vfp_s[%d].u = 0x%08Xu;  /* literal */\n",
                                    in->vd * 2 + w, v);
                        else
                            fprintf(f, "    vfp_setu(%d, 0x%08Xu);  /* literal */\n",
                                    in->vd, v);
                    }
                    st->literals++;
                    break;
                }
            }
            if (in->op == OP_VLDR) {
                if (in->vfp_dp) {
                    fprintf(f, "    vfp_s[%d].u = vita_read32(%s);\n", in->vd * 2, a);
                    fprintf(f, "    vfp_s[%d].u = vita_read32((%s) + 4);\n", in->vd * 2 + 1, a);
                } else {
                    fprintf(f, "    vfp_setu(%d, vita_read32(%s));\n", in->vd, a);
                }
            } else {
                if (in->vfp_dp) {
                    fprintf(f, "    vita_write32(%s, vfp_s[%d].u);\n", a, in->vd * 2);
                    fprintf(f, "    vita_write32((%s) + 4, vfp_s[%d].u);\n", a, in->vd * 2 + 1);
                } else {
                    fprintf(f, "    vita_write32(%s, vfp_getu(%d));\n", a, in->vd);
                }
            }
            break;
        }

        case OP_VADD: case OP_VSUB: case OP_VMUL: case OP_VDIV: {
            if (in->vd == ARM_NO_REG || in->vn == ARM_NO_REG || in->vm == ARM_NO_REG) { ok = 0; break; }
            char c = in->op == OP_VADD ? '+' : in->op == OP_VSUB ? '-'
                   : in->op == OP_VMUL ? '*' : '/';
            if (in->vfp_dp)
                fprintf(f, "    vfp_setd(%d, vfp_getd(%d) %c vfp_getd(%d));\n",
                        in->vd, in->vn, c, in->vm);
            else
                fprintf(f, "    vfp_setf(%d, vfp_getf(%d) %c vfp_getf(%d));\n",
                        in->vd, in->vn, c, in->vm);
            break;
        }

        case OP_VLDM: case OP_VSTM: {
            if (in->rn == ARM_NO_REG || in->vd == ARM_NO_REG) { ok = 0; break; }
            /* Registers occupy ascending addresses regardless of direction. A
             * decrement-before form (VPUSH) moves the base down by the whole
             * block first, then writes upward from there — the same shape as
             * the integer PUSH, and for the same reason. */
            uint32_t n     = in->imm;
            uint32_t words = in->vfp_dp ? n * 2 : n;
            /* This base is written back, so it is an lvalue and not a
             * source: resolving r15 to a constant here would emit an
             * assignment to one. There is no such addressing mode. */
            if (in->rn == 15) { ok = 0; trap(f, in, "vldm/vstm pc base"); break; }
            const char *rn = reg_name(in->rn);

            if (!in->mem_add) fprintf(f, "    %s -= %u;\n", rn, words * 4);
            for (uint32_t k = 0; k < words; k++) {
                int reg = (in->vfp_dp ? in->vd * 2 : in->vd) + (int)k;
                if (in->op == OP_VLDM)
                    fprintf(f, "    vfp_s[%d].u = vita_read32(%s + %u);\n", reg & 63, rn, k * 4);
                else
                    fprintf(f, "    vita_write32(%s + %u, vfp_s[%d].u);\n", rn, k * 4, reg & 63);
            }
            if (in->mem_add && in->writeback)
                fprintf(f, "    %s += %u;\n", rn, words * 4);
            break;
        }

        case OP_VMLA: case OP_VMLS: {
            if (in->vd == ARM_NO_REG || in->vn == ARM_NO_REG || in->vm == ARM_NO_REG) { ok = 0; break; }
            char c = (in->op == OP_VMLA) ? '+' : '-';
            if (in->vfp_dp)
                fprintf(f, "    vfp_setd(%d, vfp_getd(%d) %c (vfp_getd(%d) * vfp_getd(%d)));\n",
                        in->vd, in->vd, c, in->vn, in->vm);
            else
                fprintf(f, "    vfp_setf(%d, vfp_getf(%d) %c (vfp_getf(%d) * vfp_getf(%d)));\n",
                        in->vd, in->vd, c, in->vn, in->vm);
            break;
        }

        case OP_VNMLA: case OP_VNMLS: {
            if (in->vd == ARM_NO_REG || in->vn == ARM_NO_REG || in->vm == ARM_NO_REG) { ok = 0; break; }
            const char *g = in->vfp_dp ? "vfp_getd" : "vfp_getf";
            const char *s = in->vfp_dp ? "vfp_setd" : "vfp_setf";
            if (in->op == OP_VNMLA)
                fprintf(f, "    %s(%d, -(%s(%d) * %s(%d)) - %s(%d));\n", s, in->vd, g, in->vn, g, in->vm, g, in->vd);
            else
                fprintf(f, "    %s(%d, (%s(%d) * %s(%d)) - %s(%d));\n", s, in->vd, g, in->vn, g, in->vm, g, in->vd);
            break;
        }

        case OP_VNMUL:
            if (in->vd == ARM_NO_REG || in->vn == ARM_NO_REG || in->vm == ARM_NO_REG) { ok = 0; break; }
            if (in->vfp_dp)
                fprintf(f, "    vfp_setd(%d, -(vfp_getd(%d) * vfp_getd(%d)));\n", in->vd, in->vn, in->vm);
            else
                fprintf(f, "    vfp_setf(%d, -(vfp_getf(%d) * vfp_getf(%d)));\n", in->vd, in->vn, in->vm);
            break;

        case OP_VMOV:
            if (in->vd == ARM_NO_REG || in->vm == ARM_NO_REG) { ok = 0; break; }
            if (in->vfp_dp) fprintf(f, "    vfp_setd(%d, vfp_getd(%d));\n", in->vd, in->vm);
            else            fprintf(f, "    vfp_setu(%d, vfp_getu(%d));\n", in->vd, in->vm);
            break;

        case OP_VABS:
            if (in->vd == ARM_NO_REG || in->vm == ARM_NO_REG) { ok = 0; break; }
            /* Clearing the sign bit rather than calling fabs: it is the
             * architecture's definition, and it preserves NaN payloads where
             * a library call need not. */
            /* Double: clear bit 31 of the high word, the same rule as single
             * precision (a `x < 0 ? -x : x` got -0.0 wrong, and its `?` was
             * taken by fix_recomp.py for the unknown-operand placeholder). */
            if (in->vfp_dp) fprintf(f, "    vfp_s[%d].u = vfp_s[%d].u; vfp_s[%d].u = vfp_s[%d].u & 0x7FFFFFFFu;\n",
                                    (in->vd * 2) & 63, (in->vm * 2) & 63,
                                    (in->vd * 2 + 1) & 63, (in->vm * 2 + 1) & 63);
            else            fprintf(f, "    vfp_setu(%d, vfp_getu(%d) & 0x7FFFFFFFu);\n", in->vd, in->vm);
            break;

        case OP_VNEG:
            if (in->vd == ARM_NO_REG || in->vm == ARM_NO_REG) { ok = 0; break; }
            if (in->vfp_dp) fprintf(f, "    vfp_setd(%d, -vfp_getd(%d));\n", in->vd, in->vm);
            else            fprintf(f, "    vfp_setu(%d, vfp_getu(%d) ^ 0x80000000u);\n", in->vd, in->vm);
            break;

        case OP_VSQRT:
            if (in->vd == ARM_NO_REG || in->vm == ARM_NO_REG) { ok = 0; break; }
            if (in->vfp_dp) fprintf(f, "    vfp_setd(%d, sqrt(vfp_getd(%d)));\n", in->vd, in->vm);
            else            fprintf(f, "    vfp_setf(%d, (float)sqrt((double)vfp_getf(%d)));\n", in->vd, in->vm);
            break;

        case OP_VCMP:
            if (in->vd == ARM_NO_REG) { ok = 0; break; }
            /* Writes FPSCR, not the integer flags. A later VMRS moves them. */
            if (in->vm == ARM_NO_REG) {
                if (in->vfp_dp) fprintf(f, "    vfp_cmp_f64(vfp_getd(%d), 0.0);\n", in->vd);
                else            fprintf(f, "    vfp_cmp_f32(vfp_getf(%d), 0.0f);\n", in->vd);
            } else {
                if (in->vfp_dp) fprintf(f, "    vfp_cmp_f64(vfp_getd(%d), vfp_getd(%d));\n", in->vd, in->vm);
                else            fprintf(f, "    vfp_cmp_f32(vfp_getf(%d), vfp_getf(%d));\n", in->vd, in->vm);
            }
            break;

        case OP_VMRS:
            fprintf(f, "    vfp_mrs_apsr();\n");
            break;

        case OP_VMOV_TO_V:
            if (in->rt == ARM_NO_REG || in->vn == ARM_NO_REG) { ok = 0; break; }
            fprintf(f, "    vfp_setu(%d, %s);\n", in->vn, reg_name(in->rt));
            break;

        case OP_VMOV_TO_C:
            if (in->rt == ARM_NO_REG || in->vn == ARM_NO_REG) { ok = 0; break; }
            fprintf(f, "    %s = vfp_getu(%d);\n", reg_name(in->rt), in->vn);
            break;

        case OP_VCVT_I2F:
            if (in->vd == ARM_NO_REG || in->vm == ARM_NO_REG) { ok = 0; break; }
            if (in->vfp_dp)
                fprintf(f, "    vfp_setd(%d, (double)(%s)vfp_getu(%d));\n",
                        in->vd, in->vfp_unsigned ? "uint32_t" : "int32_t", in->vm);
            else
                fprintf(f, "    vfp_setf(%d, (float)(%s)vfp_getu(%d));\n",
                        in->vd, in->vfp_unsigned ? "uint32_t" : "int32_t", in->vm);
            break;

        case OP_VMOV_IMM: {
            /* VFPExpandImm: sign, an exponent built from NOT(b6):b6 repeated:b5:4,
             * and four fraction bits. Folded to the exact bits here. */
            uint32_t i8 = in->imm & 0xFF, b6 = (i8 >> 6) & 1;
            if (in->vfp_dp) {
                uint32_t hi = ((i8 >> 7) << 31) | ((b6 ^ 1) << 30) | (b6 ? 0x3FC00000u : 0)
                            | (((i8 >> 4) & 3) << 20) | ((i8 & 0xF) << 16);
                fprintf(f, "    vfp_s[%d].u = 0u; vfp_s[%d].u = 0x%08Xu;\n",
                        (in->vd * 2) & 63, (in->vd * 2 + 1) & 63, hi);
            } else {
                uint32_t v = ((i8 >> 7) << 31) | ((b6 ^ 1) << 30) | (b6 ? 0x3E000000u : 0)
                           | (((i8 >> 4) & 3) << 23) | ((i8 & 0xF) << 19);
                fprintf(f, "    vfp_setu(%d, 0x%08Xu);\n", in->vd, v);
            }
            break;
        }

        case OP_VCVT_FF:
            if (in->vfp_dp) fprintf(f, "    vfp_setd(%d, (double)vfp_getf(%d));\n", in->vd, in->vm);
            else            fprintf(f, "    vfp_setf(%d, (float)vfp_getd(%d));\n", in->vd, in->vm);
            break;

        case OP_VMOV_SC_TO_C:
            fprintf(f, "    %s = vfp_s[%d].u;\n", reg_name(in->rt), (in->vd * 2 + (int)in->imm) & 63);
            break;

        case OP_VMOV_C_TO_SC:
            fprintf(f, "    vfp_s[%d].u = %s;\n", (in->vd * 2 + (int)in->imm) & 63, src_name(in, in->rt));
            break;

        case OP_VMOV_RR_TO_V: {
            int w = in->vfp_dp ? in->vm * 2 : in->vm;
            fprintf(f, "    vfp_s[%d].u = %s; vfp_s[%d].u = %s;\n", w & 63, src_name(in, in->rt),
                    (w + 1) & 63, src_name(in, in->rt2));
            break;
        }

        case OP_VMOV_V_TO_RR: {
            int w = in->vfp_dp ? in->vm * 2 : in->vm;
            fprintf(f, "    %s = vfp_s[%d].u; %s = vfp_s[%d].u;\n", reg_name(in->rt), w & 63,
                    reg_name(in->rt2), (w + 1) & 63);
            break;
        }

        case OP_VCVT_F2I:
            if (in->vd == ARM_NO_REG || in->vm == ARM_NO_REG) { ok = 0; break; }
            /* Toward zero, which is what a C cast does — the only rounding
             * mode claimed by the decoder for exactly that reason. */
            {
                const char *src = in->vfp_dp ? "vfp_getd" : "vfp_getf";
                /* Saturating, as the hardware is; a C cast is undefined out of range. */
                const char *cv  = in->vfp_unsigned ? "vfp_cvt_u32" : "vfp_cvt_s32";
                if (in->has_imm)    /* VCVTR: FPSCR rounding, round-to-nearest-even by default */
                    fprintf(f, "    vfp_setu(%d, %s(nearbyint((double)%s(%d))));\n",
                            in->vd, cv, src, in->vm);
                else
                    fprintf(f, "    vfp_setu(%d, %s(trunc((double)%s(%d))));\n", in->vd, cv, src, in->vm);
            }
            break;

        case OP_MLA: case OP_MLS:
            fprintf(f, "    %s = %s %c %s * %s;\n", reg_name(in->rd), src_name(in, in->rt),
                    in->op == OP_MLA ? '+' : '-', src_name(in, in->rn), src_name(in, in->rm));
            break;

        case OP_UMULL: case OP_SMULL: case OP_UMLAL: case OP_SMLAL: {
            int sgn = in->op == OP_SMULL || in->op == OP_SMLAL;
            int acc = in->op == OP_UMLAL || in->op == OP_SMLAL;
            if (sgn)
                fprintf(f, "    { uint64_t _p = (uint64_t)((int64_t)(int32_t)%s * (int64_t)(int32_t)%s);\n",
                        src_name(in, in->rn), src_name(in, in->rm));
            else
                fprintf(f, "    { uint64_t _p = (uint64_t)%s * (uint64_t)%s;\n",
                        src_name(in, in->rn), src_name(in, in->rm));
            if (acc)
                fprintf(f, "      _p += ((uint64_t)%s << 32) | %s;\n",
                        src_name(in, in->rt2), src_name(in, in->rt));
            fprintf(f, "      %s = (uint32_t)_p; %s = (uint32_t)(_p >> 32); }\n",
                    reg_name(in->rt), reg_name(in->rt2));
            break;
        }

        case OP_LDM: case OP_STM: {
            int count = 0;
            for (int i = 0; i < 16; i++) if (in->reglist & (1u << i)) count++;
            /* PC as the base or in the list, or SP in the list, is UNPREDICTABLE
             * or a transfer: in practice it is data decoded as code. Trap. */
            if (in->rn == 15 || (in->reglist & 0xA000)) { ok = 0; break; }
            /* _b is the lowest address touched; DB forms start count words down. */
            fprintf(f, "    { uint32_t _b = %s - %d;\n", src_name(in, in->rn),
                    in->ldm_db ? count * 4 : 0);
            int slot = 0;
            for (int i = 0; i < 16; i++) {
                if (!(in->reglist & (1u << i))) continue;
                if (in->op == OP_LDM)
                    fprintf(f, "      %s = vita_read32(_b + %d);\n", reg_name(i), slot * 4);
                else {
                    char rv[64]; reg_operand(rv, sizeof(rv), i, in);
                    fprintf(f, "      vita_write32(_b + %d, %s);\n", slot * 4, rv);
                }
                slot++;
            }
            if (in->writeback && !(in->op == OP_LDM && (in->reglist & (1u << in->rn))))
                fprintf(f, "      %s = _b + %d;\n", reg_name(in->rn), in->ldm_db ? 0 : count * 4);
            fprintf(f, "    }\n");
            break;
        }

        case OP_LDREX:
            fprintf(f, "    %s = vita_ldrex(%s + 0x%X, %d);\n", reg_name(in->rt),
                    src_name(in, in->rn), in->imm, in->ex_size);
            break;

        case OP_STREX:
            fprintf(f, "    %s = vita_strex(%s + 0x%X, %s, %d);\n", reg_name(in->rd),
                    src_name(in, in->rn), in->imm, src_name(in, in->rt), in->ex_size);
            break;

        case OP_CLREX:
            fprintf(f, "    vita_clrex();\n");
            break;

        case OP_BARRIER:
            fprintf(f, "    /* barrier */\n");
            break;

        case OP_TBB: case OP_TBH: {
            uint32_t tt[1024];
            int nt = in->imm ? arm_table_targets(img->data + img->seg_file, img->seg_vaddr,
                                                 img->seg_len, in, in->imm, tt, 1024) : 0;
            if (nt == 0) { ok = 0; break; }
            fprintf(f, "    switch (%s) {\n", src_name(in, in->rm));
            for (int k = 0; k < nt; k++) {
                if (fb_has_label(b, tt[k])) fprintf(f, "    case %d: goto L_%08X;\n", k, tt[k]);
                else fprintf(f, "    case %d: vita_trap_indirect(0x%08X, 0x%08X); return;\n", k, in->addr, tt[k]);
            }
            fprintf(f, "    default: vita_trap_indirect(0x%08X, 0); return;\n    }\n", in->addr);
            break;
        }

        case OP_NOP:
            fprintf(f, "    /* nop */\n");
            break;

        case OP_IT:
            /* The IT instruction itself emits nothing; its effect is that the
             * following instructions carry a condition, which the decoder has
             * already attached to them. */
            fprintf(f, "    /* it %s */\n", arm_cond_name(in->cond));
            break;

        default:
            ok = 0;
            break;
    }

    /* Control-flow classes the operand decoder does not cover are handled by
     * class rather than by op, so a call is still a call even when its exact
     * encoding was not decoded. */
    if (!ok) {
        if (in->cls == A_CALL && in->has_target && fs_has(fs, in->target)) {
            fprintf(f, "    vita_func_%08X();\n", in->target);
            ok = 1;
        } else if (in->cls == A_CALL && in->has_target) {
            fprintf(f, "    vita_trap_indirect(0x%08X, 0x%08X);\n", in->addr, in->target);
        } else if (in->cls == A_RETURN) {
            fprintf(f, "    return;\n");
            ok = 1;
        } else if (in->cls == A_CALL || in->cls == A_INDIRECT) {
            fprintf(f, "    vita_trap_indirect(0x%08X, 0);\n", in->addr);
            note_trap(st, "indirect transfer");
        } else if (in->cls == A_SIMD && neon_kind(in->raw)) {
            /* NEON runs in the runtime from its encoding (neon.h). */
            uint32_t h1 = in->raw >> 16, h2 = in->raw & 0xFFFF;
            int rt = (int)((h2 >> 12) & 15), rn = (int)(h1 & 15), rm = (int)(h2 & 15);
            switch (neon_kind(in->raw)) {
                case 1:
                    /* the busiest forms as plain C (neon_emit.h); the rest from the encoding */
                    if (!getenv("ARMRECOMP_NEON_RUNTIME") && neon_emit_inline(f, in->raw)) st->neon_inline++;
                    else fprintf(f, "    neon_dp(0x%08Xu, 0x%08Xu);\n", in->addr, in->raw);
                    ok = 1;
                    break;
                case 2:
                    if (rn == 15) break;
                    fprintf(f, "    { uint32_t _nb = neon_ldst(0x%08Xu, 0x%08Xu, %s);",
                            in->addr, in->raw, reg_name(rn));
                    if (rm == 13)      fprintf(f, " %s += _nb;", reg_name(rn));
                    else if (rm != 15) fprintf(f, " %s += %s;", reg_name(rn), reg_name(rm));
                    fprintf(f, " (void)_nb; }\n");
                    ok = 1;
                    break;
                case 3: case 4:
                    if (rt == 15) break;
                    fprintf(f, "    %s(0x%08Xu, %s);\n",
                            neon_kind(in->raw) == 3 ? "neon_dup_core" : "neon_set_lane",
                            in->raw, reg_name(rt));
                    ok = 1;
                    break;
                case 5:
                    if (rt == 15) break;
                    fprintf(f, "    %s = neon_get_lane(0x%08Xu);\n", reg_name(rt), in->raw);
                    ok = 1;
                    break;
            }
            if (!ok) { trap(f, in, "simd"); note_trap(st, "simd/vfp"); }
            else st->neon++;
        } else if (in->cls == A_SIMD) {
            trap(f, in, "simd");
            note_trap(st, "simd/vfp");
        } else {
            trap(f, in, in->mnemonic);
            note_trap(st, in->mnemonic);
        }
    }

    if (shc) fprintf(f, "    flag_c = _shc; }\n");
    if (guarded) fprintf(f, "    }\n");
    return ok;
}

/* --- the whole module -------------------------------------------------------- */

int em_emit(const vm_image *img, const vm_module *mod, const nid_db *db,
            const vf_result *disc, FILE *f, const char *impdir,
            uint32_t limit, emit_stats *st) {
    memset(st, 0, sizeof(*st));

    uint32_t n_funcs = disc->count;
    if (limit && limit < n_funcs) n_funcs = limit;

    /* Every address that is a defined symbol in this output, sorted for lookup.
     * Under --limit the un-emitted ones still get trapping stubs, so they are
     * genuinely defined and belong in the set. */
    funcset fs;
    fs.n = disc->count;
    fs.v = (uint32_t *)malloc((fs.n ? fs.n : 1) * sizeof(uint32_t));
    if (!fs.v) return 1;
    for (uint32_t i = 0; i < disc->count; i++) fs.v[i] = disc->funcs[i].addr;
    qsort(fs.v, fs.n, sizeof(uint32_t), fs_cmp);

    /* Entries: discovery's functions, then the addresses the pre-pass promotes
     * (see collect). Bounds are computed against discovery's set and frozen. */
    uint32_t n_ent = disc->count, cap_ent = disc->count + 4096;
    vf_func  *ent   = (vf_func *)malloc(cap_ent * sizeof(vf_func));
    uint32_t *bnd   = (uint32_t *)malloc(cap_ent * sizeof(uint32_t));
    if (!ent || !bnd) return 1;
    for (uint32_t i = 0; i < disc->count; i++) {
        ent[i] = disc->funcs[i];
        bnd[i] = fn_bound(&ent[i], &fs);
    }

    /* Starts that are certainly functions: found as call targets or exports. */
    funcset real;
    real.n = 0;
    real.v = (uint32_t *)malloc((disc->count ? disc->count : 1) * sizeof(uint32_t));
    if (!real.v) return 1;
    for (uint32_t i = 0; i < disc->count; i++) {
        uint32_t a = disc->funcs[i].addr;
        int sure = !disc->funcs[i].from_heuristic;
        /* A shape-found start that opens with PUSH / PUSH.W is a prologue:
         * compiled code does not save registers in the middle of a loop. */
        const uint8_t *p = vm_va(img, a, 2);
        if (!sure && p && disc->funcs[i].mode == ARM_T32) {
            uint16_t h = (uint16_t)(p[0] | (p[1] << 8));
            sure = (h & 0xFE00) == 0xB400 || h == 0xE92D;
        }
        if (sure) real.v[real.n++] = a;
    }
    qsort(real.v, real.n, sizeof(uint32_t), fs_cmp);
    {
        xset pend;
        memset(&pend, 0, sizeof(pend));
        pend.cap = 1u << 21;
        pend.k = (uint32_t *)calloc(pend.cap, sizeof(uint32_t));
        if (!pend.k) return 1;
        uint32_t taken = 0;
        for (uint32_t i = 0; i < n_ent; i++) {
            if (i < disc->count && i >= n_funcs) continue;   /* not emitted under --limit */
            fbody pb;
            memset(&pb, 0, sizeof(pb));
            collect(img, &ent[i], bnd[i], &fs, &real, &pend, &pb);
            free(pb.v);
            free(pb.labels);
            for (; taken < pend.n; taken++) {
                uint32_t a = pend.list[taken];
                if (fs_has(&fs, a)) continue;
                if (n_ent == cap_ent) {
                    cap_ent *= 2;
                    ent = (vf_func *)realloc(ent, cap_ent * sizeof(vf_func));
                    bnd = (uint32_t *)realloc(bnd, cap_ent * sizeof(uint32_t));
                    if (!ent || !bnd) return 1;
                }
                memset(&ent[n_ent], 0, sizeof(vf_func));
                ent[n_ent].addr = a;
                ent[n_ent].mode = ent[i].mode;
                ent[n_ent].from_heuristic = 1;
                /* Up to the next discovered start: the code between belongs to
                 * nobody else. */
                uint32_t nx = fs_next(&fs, a);
                bnd[n_ent] = (nx && nx < a + 8192) ? nx : a + 8192;
                ent[n_ent].end = bnd[n_ent];
                n_ent++;
            }
        }
        st->promoted = n_ent - disc->count;
        free(pend.k);
        free(pend.list);
    }

    /* The final set, used by every emit decision and the dispatch table. */
    free(fs.v);
    fs.n = n_ent;
    fs.v = (uint32_t *)malloc((n_ent ? n_ent : 1) * sizeof(uint32_t));
    if (!fs.v) return 1;
    for (uint32_t i = 0; i < n_ent; i++) fs.v[i] = ent[i].addr;
    qsort(fs.v, fs.n, sizeof(uint32_t), fs_cmp);

    fprintf(f, "/* Generated by armrecomp. Do not edit.\n");
    fprintf(f, " *\n");
    fprintf(f, " * module: %s\n", mod->info.name);
    fprintf(f, " * %u functions\n", disc->count);
    fprintf(f, " */\n\n");
    fprintf(f, "#include \"vitarecomp/recomp_rt.h\"\n");
    fprintf(f, "#include \"vitarecomp/dispatch.h\"\n\n");

    /* Declare every import the module could call. Emitting these unconditionally
     * — rather than only the ones a limited emit happens to reach — keeps the
     * declaration set stable, so adding functions to the emit does not change
     * which symbols exist. */
    if (mod->stub_count) {
        fprintf(f, "/* firmware imports */\n");
        for (uint32_t i = 0; i < mod->stub_count; i++) {
            char sym[96];
            hle_symbol(sym, sizeof(sym), db, &mod->stubs[i]);
            fprintf(f, "void %s(void);\n", sym);
        }
        fprintf(f, "\n");
    }

    /* Forward declarations: the call graph has cycles, so every function must
     * be visible before any body is emitted. */
    for (uint32_t i = 0; i < n_ent; i++)
        fprintf(f, "void vita_func_%08X(void);\n", ent[i].addr);
    fprintf(f, "\n");

    /* A limited emit still references functions it did not emit, so those get
     * a stub that traps. Without this the output would not link, and a silently
     * missing function is exactly the failure this project refuses. */
    if (n_funcs < disc->count) {
        fprintf(f, "/* Not emitted under --limit; present so the output links. */\n");
        for (uint32_t i = n_funcs; i < disc->count; i++)
            fprintf(f, "void vita_func_%08X(void) { vita_trap_unimpl(0x%08X, 0, \"not emitted\"); }\n",
                    disc->funcs[i].addr, disc->funcs[i].addr);
        fprintf(f, "\n");
    }

    for (uint32_t i = 0; i < n_ent; i++) {
        if (i < disc->count && i >= n_funcs) continue;       /* stubbed above */
        const vf_func *fn = &ent[i];
        fbody b;
        memset(&b, 0, sizeof(b));
        collect(img, fn, bnd[i], &fs, &real, NULL, &b);

        fprintf(f, "/* ---------------------------------------------------------------\n");
        fprintf(f, " * vita_func_%08X  --  %u instructions, %u bytes%s\n",
                fn->addr, b.n, fn->end - fn->addr,
                fn->from_heuristic ? ", discovered by shape (heuristic)" : "");
        fprintf(f, " * ------------------------------------------------------------- */\n");
        fprintf(f, "void vita_func_%08X(void) {\n", fn->addr);

        /* Overlapping decodes (MLB 15 port, 2026-11-07).
         *
         * A jump table read past its end (the index bound is not always visible
         * to the walk) yields entries that point into the MIDDLE of real
         * instructions; each becomes a label and a second, bogus decode. Sorted
         * by address, that bogus instruction lands right after the real one it
         * overlaps, so the real one's fall-through ran into it: `bl` at
         * 0x81769F8C returned into a trap decoded at 0x81769F8E instead of
         * continuing at 0x81769F90 (Franchise/RTTS simulation). Fall-through
         * now jumps explicitly to the true next address whenever the next
         * emitted instruction does not start there. */
        for (uint32_t k = 0; k + 1 < b.n; k++) {
            const arm_insn *p = &b.v[k];
            uint32_t end = p->addr + p->width;
            if (b.v[k + 1].addr >= end) continue;
            for (uint32_t j = k + 1; j < b.n && b.v[j].addr <= end; j++)
                if (b.v[j].addr == end) { fb_label(&b, end); break; }
        }
        for (uint32_t k = 0; k < b.n; k++) {
            const arm_insn *in = &b.v[k];
            if (k > 0) {
                const arm_insn *p = &b.v[k - 1];
                uint32_t end = p->addr + p->width;
                int ends_flow = (p->cls == A_BRANCH || p->cls == A_RETURN || p->cls == A_INDIRECT) && !p->conditional;
                if (p->op == OP_TBB || p->op == OP_TBH) ends_flow = 1;
                if (in->addr < end && !ends_flow && fb_has_label(&b, end)) {
                    fprintf(f, "    goto L_%08X;   /* fall-through past an overlapping decode */\n", end);
                    st->overlaps++;
                }
            }
            if (fb_has_label(&b, in->addr))
                fprintf(f, "L_%08X:\n", in->addr);

            fprintf(f, "    /* %08X  %-16s */\n", in->addr, in->mnemonic);
            if (emit_insn(img, mod, db, &fs, &b, in, f, st)) st->translated++;
            else                               st->trapped++;
            st->insns++;
        }

        fprintf(f, "}\n\n");
        st->funcs++;

        free(b.v);
        free(b.labels);
    }

    /* --- the HLE defaults ----------------------------------------------------
     *
     * One default implementation per import, each of which traps by name. This
     * is what makes the HLE incrementally implementable: the generated C links
     * from the first build, and every unimplemented firmware call announces
     * exactly which function the game wanted rather than failing to link or,
     * worse, silently returning zero.
     *
     * One FILE per import, for the reason in emit.h: at one archive member per
     * function, writing a real implementation and relinking is the entire
     * override procedure. No list to keep in sync, and no generated code to
     * hand-edit.
     */
    if (impdir) {
        for (uint32_t i = 0; i < mod->stub_count; i++) {
            const vm_stub *s = &mod->stubs[i];
            char sym[96];
            hle_symbol(sym, sizeof(sym), db, s);
            const char *lib = nid_lib_name(db, s->lib_nid);
            const char *fn  = nid_func_name(db, s->lib_nid, s->func_nid);

            char path[1200];
            snprintf(path, sizeof(path), "%s/%s.c", impdir, sym);
            FILE *one = fopen(path, "w");
            if (!one) continue;

            fprintf(one, "/* Generated by armrecomp. Do not edit.\n");
            fprintf(one, " *\n");
            fprintf(one, " * %s::%s  --  stub 0x%08X in module '%s'.\n",
                    lib ? lib : "?", fn ? fn : "(unresolved)", s->addr,
                    mod->info.name);
            fprintf(one, " *\n");
            fprintf(one, " * The default: trap by name. To implement it, define this symbol\n");
            fprintf(one, " * in the runtime and relink -- this file is one member of a static\n");
            fprintf(one, " * library, so the linker stops extracting it. Nothing to delete.\n");
            fprintf(one, " */\n\n");
            fprintf(one, "#include \"vitarecomp/recomp_rt.h\"\n\n");
            /* The name goes into the trap, not just the comment: at run time
             * the NID alone is a hash, and the whole reason imports are bound
             * is so a gap says which firmware function the game wanted. */
            if (fn)
                fprintf(one,
                        "void %s(void) { vita_trap_import(0x%08X, 0x%08X, \"%s::%s\"); }\n",
                        sym, s->addr, s->func_nid, lib ? lib : "?", fn);
            else
                fprintf(one,
                        "void %s(void) { vita_trap_import(0x%08X, 0x%08X, 0); }\n",
                        sym, s->addr, s->func_nid);
            fclose(one);
            st->imports_used++;
        }
    }

    /* The dispatch table: every function AND every import stub, sorted by
     * guest address, so an indirect transfer can find either at run time.
     * Functions are emitted for ALL discovered addresses — including any not
     * emitted under --limit, which have trapping stubs — because a dispatch
     * that misses should report "we never translated that" rather than "that
     * address is not a function".
     *
     * The stubs belong in the same table because a transfer to one is a
     * firmware call. Imports bind at emit time on a direct BL, but a module
     * reaches firmware through pointers too — `module_start` makes its very
     * first firmware call that way — and those never pass through the BL path.
     * Without this the dispatcher reports the game's first C++ runtime call as
     * an unresolved address, which names the wrong problem: the code is not
     * missing, it is firmware.
     *
     * One merged table rather than a second lookup: the entries have the same
     * shape already (guest address -> void(void)), both lists arrive sorted,
     * and merging keeps vita_dispatch a single binary search.
     */
    fprintf(f, "/* ---------------------------------------------------------------\n");
    fprintf(f, " * dispatch table -- %u functions + %u import stubs,\n",
            disc->count, mod->stub_count);
    fprintf(f, " * sorted by guest address\n");
    fprintf(f, " * ------------------------------------------------------------- */\n");
    fprintf(f, "static const vita_dispatch_entry vita_functions[] = {\n");
    {
        uint32_t fi = 0, si = 0;
        while (fi < fs.n || si < mod->stub_count) {
            int take_stub;
            if (fi >= fs.n)                            take_stub = 1;
            else if (si >= mod->stub_count)            take_stub = 0;
            else if (mod->stubs[si].addr < fs.v[fi])   take_stub = 1;
            else if (mod->stubs[si].addr > fs.v[fi])   take_stub = 0;
            else {
                /* Both lists claim this address. The stub wins: being in the
                 * import table is what the address MEANS, and the bytes
                 * discovery found there are the placeholder body the loader
                 * was supposed to overwrite. Translating that placeholder and
                 * returning from it is the failure this whole layer exists to
                 * prevent. */
                take_stub = 1;
                fi++;
                st->stub_shadowed++;
            }

            if (take_stub) {
                char sym[96];
                hle_symbol(sym, sizeof(sym), db, &mod->stubs[si]);
                fprintf(f, "    { 0x%08Xu, %s },\n", mod->stubs[si].addr, sym);
                si++;
            } else {
                fprintf(f, "    { 0x%08Xu, vita_func_%08X },\n", fs.v[fi], fs.v[fi]);
                fi++;
            }
        }
    }
    fprintf(f, "};\n\n");
    fprintf(f, "void vita_register_functions(void) {\n");
    fprintf(f, "    vita_dispatch_init(vita_functions,\n");
    fprintf(f, "        (uint32_t)(sizeof(vita_functions) / sizeof(vita_functions[0])));\n");
    fprintf(f, "}\n\n");

    /* Imported variables: every MOVW/MOVT that reaches one was emitted with
     * its cell's address (module.h); the host fills the cells by NID. */
    fprintf(f, "/* imported variables -- %u, %u references; the host fills each cell */\n",
            mod->var_count, mod->varref_count);
    fprintf(f, "const vita_import_var vita_import_vars[] = {\n");
    for (uint32_t i = 0; i < mod->var_count; i++)
        fprintf(f, "    { 0x%08Xu, 0x%08Xu, 0x%08Xu },  /* %s */\n", mod->vars[i].lib_nid,
                mod->vars[i].nid, mod->vars[i].cell,
                nid_func_name(db, mod->vars[i].lib_nid, mod->vars[i].nid)
                    ? nid_func_name(db, mod->vars[i].lib_nid, mod->vars[i].nid) : "?");
    if (!mod->var_count) fprintf(f, "    { 0, 0, 0 }\n");
    fprintf(f, "};\n");
    fprintf(f, "const uint32_t vita_import_var_count = %uu;\n", mod->var_count);

    free(fs.v);
    free(ent);
    free(bnd);
    free(real.v);
    return 0;
}



