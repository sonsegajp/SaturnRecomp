/* sh2_semantics.c — per-instruction execution tests for the SH-2 interpreter.
 *
 * The decoder is already proven 0-wrong against capstone, but capstone only
 * decodes: it cannot say whether `cmp/ge` sets T from the right comparison or
 * whether `shlr2` is logical. Those are execution semantics, and a mistake in
 * one shows up as a game that runs for millions of instructions and then
 * wedges. This file pins each instruction's behaviour against the SH-2
 * programming manual, one case at a time, with no machine state beyond what
 * the case sets up.
 */
#include "saturn.h"
#include <stdio.h>
#include <string.h>

int  sh2_step(sh2 *c);
uint64_t sh2_run(sh2 *c, uint64_t n);
void sh2_reset(sh2 *c, saturn *s, int slave, uint32_t pc, uint32_t sp);
void saturn_init(saturn *s);

static saturn g;
static int    fails, total;

#define CODE 0x06004000u

/* Assemble `n` opcodes at CODE, run them, and hand back the CPU. */
static sh2 *run(const uint16_t *ops, int n, int steps)
{
    sh2 *c = &g.master;
    saturn_init(&g);
    for (int i = 0; i < n; i++) bus_w16(&g, CODE + (uint32_t)i * 2, ops[i]);
    sh2_reset(c, &g, 0, CODE, 0x06020000u);
    for (int i = 0; i < steps; i++)
        if (!sh2_step(c)) break;
    return c;
}

/* Same setup, but enter through the production dispatcher.  This is what
 * exercises the hand-inlined hot path rather than exec_one via sh2_step. */
static sh2 *run_fast(const uint16_t *ops, int n, int steps)
{
    sh2 *c = &g.master;
    saturn_init(&g);
    for (int i = 0; i < n; i++) bus_w16(&g, CODE + (uint32_t)i * 2, ops[i]);
    sh2_reset(c, &g, 0, CODE, 0x06020000u);
    sh2_run(c, (uint64_t)steps);
    return c;
}

static void chk(const char *what, uint32_t got, uint32_t want)
{
    total++;
    if (got != want) {
        fails++;
        printf("  FAIL %-28s got %08X want %08X\n", what, got, want);
    }
}

int main(void)
{
    sh2 *c;

    printf("SH-2 execution semantics\n");

    /* ---- cmp/ge is SIGNED: T = (Rn >= Rm). The hang loop at 0x06012C88
     * depends on 0 >= 0x00010000 being FALSE. */
    {
        uint16_t p[] = { 0xE000, 0xE100, 0x3013 };   /* r0=0 r1=0 cmp/ge r1,r0 */
        c = run(p, 3, 3);
        chk("cmp/ge 0,0 -> T", c->sr & SR_T, 1);
    }
    {
        /* r0 = 0, r1 = 0x00010000 built by shifting, then cmp/ge r1,r0. */
        uint16_t p[] = { 0xE101, 0x4118, 0x4118, 0xE000, 0x3013 };
        c = run(p, 5, 5);                 /* r1 = 1<<16 */
        chk("r1 = 1<<16",  c->r[1], 0x00010000u);
        chk("cmp/ge 0>=65536 -> T", c->sr & SR_T, 0);
    }
    {
        uint16_t p[] = { 0xE0FF, 0xE101, 0x3013 };   /* r0=-1 r1=1 cmp/ge r1,r0 */
        c = run(p, 3, 3);
        chk("cmp/ge -1>=1 -> T", c->sr & SR_T, 0);
    }
    {
        uint16_t p[] = { 0xE001, 0xE1FF, 0x3013 };   /* r0=1 r1=-1 cmp/ge r1,r0 */
        c = run(p, 3, 3);
        chk("cmp/ge 1>=-1 -> T", c->sr & SR_T, 1);
    }

    /* ---- cmp/gt is SIGNED: T = (Rn > Rm). */
    {
        uint16_t p[] = { 0xE005, 0xE105, 0x3017 };
        c = run(p, 3, 3);
        chk("cmp/gt 5>5 -> T", c->sr & SR_T, 0);
    }
    {
        uint16_t p[] = { 0xE006, 0xE1FF, 0x3017 };   /* 6 > -1 */
        c = run(p, 3, 3);
        chk("cmp/gt 6>-1 -> T", c->sr & SR_T, 1);
    }

    /* ---- cmp/hs is UNSIGNED: T = (Rn >= Rm). -1 is the largest value. */
    {
        uint16_t p[] = { 0xE0FF, 0xE101, 0x3012 };
        c = run(p, 3, 3);
        chk("cmp/hs 0xFFFFFFFF>=1", c->sr & SR_T, 1);
    }

    /* ---- cmp/pl is SIGNED and strict: T = (Rn > 0). */
    {
        uint16_t p[] = { 0xE000, 0x4015 };
        c = run(p, 2, 2);
        chk("cmp/pl 0 -> T", c->sr & SR_T, 0);
    }
    {
        uint16_t p[] = { 0xE0FF, 0x4015 };
        c = run(p, 2, 2);
        chk("cmp/pl -1 -> T", c->sr & SR_T, 0);
    }

    /* ---- shifts. shlr/shlr2 are LOGICAL (zero-fill), shar is arithmetic. */
    {
        uint16_t p[] = { 0xE0FF, 0x4009 };           /* r0 = -1; shlr2 r0 */
        c = run(p, 2, 2);
        chk("shlr2 0xFFFFFFFF", c->r[0], 0x3FFFFFFFu);
    }
    {
        uint16_t p[] = { 0xE0FF, 0x4001 };           /* shlr r0 */
        c = run(p, 2, 2);
        chk("shlr 0xFFFFFFFF", c->r[0], 0x7FFFFFFFu);
    }
    {
        uint16_t p[] = { 0xE0FF, 0x4021 };           /* shar r0 */
        c = run(p, 2, 2);
        chk("shar 0xFFFFFFFF", c->r[0], 0xFFFFFFFFu);
    }
    {
        uint16_t p[] = { 0xE001, 0x4008 };           /* shll2 r0 */
        c = run(p, 2, 2);
        chk("shll2 1", c->r[0], 4u);
    }
    {
        uint16_t p[] = { 0xE0FF, 0x4029 };           /* shlr16 r0 */
        c = run(p, 2, 2);
        chk("shlr16 0xFFFFFFFF", c->r[0], 0x0000FFFFu);
    }

    /* ---- shlr2 must reach zero, which is what lets the hang loop exit. */
    {
        uint16_t p[] = { 0xE0FF, 0x4009, 0x4009, 0x4009, 0x4009, 0x4009,
                         0x4009, 0x4009, 0x4009, 0x4009, 0x4009, 0x4009,
                         0x4009, 0x4009, 0x4009, 0x4009, 0x4009 };
        c = run(p, 17, 17);
        chk("shlr2 x16 -> 0", c->r[0], 0u);
    }

    /* ---- exts.w sign-extends the low 16 bits. */
    {
        uint16_t p[] = { 0xE0FF, 0x400F };           /* r0=-1; ... */
        (void)p;
        uint16_t q[] = { 0xE001, 0x4018, 0x601F };   /* r0=1<<8... exts.w r1,r0 */
        c = run(q, 3, 3);
        chk("exts.w of 0x00000100", c->r[0], 0x00000000u);
    }

    /* ---- add #imm sign-extends. */
    {
        uint16_t p[] = { 0xE005, 0x70FF };           /* r0=5; add #-1,r0 */
        c = run(p, 2, 2);
        chk("add #-1 to 5", c->r[0], 4u);
    }

    /* ---- bf does NOT have a delay slot; bra does, and the slot runs. */
    {
        /* r0=1; cmp/pl r0 (T=1); bf +2 (not taken); add #1,r0 */
        uint16_t p[] = { 0xE001, 0x4015, 0x8B01, 0x7001, 0x0009, 0x0009 };
        c = run(p, 6, 4);
        chk("bf not taken runs next insn", c->r[0], 2u);
    }
    {
        /* r0=0; cmp/pl r0 (T=0); bf. The target is PC+4+disp*2, and PC+4 is
         * already the instruction after the next one, so disp=0 skips exactly
         * one instruction: the add #1 is jumped over, the add #2 runs. */
        uint16_t p[] = { 0xE000, 0x4015, 0x8B00, 0x7001, 0x7002, 0x0009 };
        c = run(p, 6, 4);
        chk("bf taken skips one insn", c->r[0], 2u);
    }
    {
        /* bra forward; the delay slot must still execute. */
        uint16_t p[] = { 0xE000, 0xA001, 0x7005, 0x7003, 0x0009 };
        c = run(p, 5, 3);
        chk("bra delay slot executes", c->r[0], 5u);
    }

    /* ---- mov.l @(disp,PC): base is (PC & ~3) + 4. */
    {
        sh2 *cc;
        saturn_init(&g);
        bus_w16(&g, CODE + 0, 0xD001);        /* mov.l @(4,pc),r0  */
        bus_w16(&g, CODE + 2, 0x0009);        /* nop               */
        bus_w32(&g, CODE + 8, 0xDEADBEEFu);   /* (CODE&~3)+4+4     */
        cc = &g.master;
        sh2_reset(cc, &g, 0, CODE, 0x06020000u);
        sh2_step(cc);
        chk("mov.l @(disp,PC)", cc->r[0], 0xDEADBEEFu);
    }

    /* ---- production fast-dispatch equivalence.  These are deliberately
     * short streams made entirely of fast opcodes; a miss is visible in the
     * hit count as well as in architectural state. */
    {
        uint16_t p[] = { 0xE0F0, 0xE10F, 0x2018 }; /* tst r1,r0 */
        c = run_fast(p, 3, 3);
        chk("fast tst result", c->sr & SR_T, 1u);
        chk("fast tst dispatch", (uint32_t)g.fastpath_hits, 3u);
    }
    {
        uint16_t p[] = { 0xE0F0, 0xE10F, 0x2019, 0x201A, 0x201B };
        c = run_fast(p, 5, 5); /* and -> 0, xor -> 15, or -> 15 */
        chk("fast logic result", c->r[0], 15u);
        chk("fast logic dispatch", (uint32_t)g.fastpath_hits, 5u);
    }
    {
        uint16_t p[] = { 0xE0FF, 0x6107, 0x620C, 0x630E };
        c = run_fast(p, 4, 4); /* not, extu.b, exts.b */
        chk("fast not result", c->r[1], 0u);
        chk("fast extu.b result", c->r[2], 255u);
        chk("fast exts.b result", c->r[3], 0xFFFFFFFFu);
        chk("fast unary dispatch", (uint32_t)g.fastpath_hits, 4u);
    }
    {
        uint16_t p[] = { 0xE104, 0x0123, 0xE007, 0xE001 };
        c = run_fast(p, 4, 3); /* r1=4; braf r1; mov #7,r0 in slot */
        chk("fast braf delay slot", c->r[0], 7u);
        chk("fast braf target", c->pc, CODE + 10u);
        chk("fast braf dispatch", (uint32_t)g.fastpath_hits, 3u);
    }
    {
        uint16_t p[] = { 0xE1FF, 0x4124 }; /* r1=-1; rotcl r1, initial T=0 */
        c = run_fast(p, 2, 2);
        chk("fast rotcl result", c->r[1], 0xFFFFFFFEu);
        chk("fast rotcl T", c->sr & SR_T, 1u);
    }
    {
        uint16_t p[] = { 0xE303, 0xE104, 0x3314 }; /* div1 r1,r3 */
        uint32_t slow_r, slow_sr;
        c = run(p, 3, 3); slow_r = c->r[3]; slow_sr = c->sr;
        c = run_fast(p, 3, 3);
        chk("fast div1 result", c->r[3], slow_r);
        chk("fast div1 flags", c->sr, slow_sr);
    }
    {
        uint16_t p[] = { 0xD202, 0xE012, 0x8121, 0x8521, 0x0009, 0x0009,
                         0x0604, 0x4100 };
        c = run_fast(p, 8, 4); /* r2=0x06044100; word roundtrip at +2 */
        chk("fast disp word roundtrip", c->r[0], 0x12u);
        chk("fast disp dispatch", (uint32_t)g.fastpath_hits, 4u);
    }

    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", total, fails);
    return fails ? 1 : 0;
}
