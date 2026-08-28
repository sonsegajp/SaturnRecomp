/* sh2_fastpath_fuzz.c -- differential test: inlined fast path vs sh2_step.
 *
 * sh2_run() carries a hand-inlined dispatcher for the opcodes that dominate
 * real instruction streams; sh2_step() runs everything through the decoder and
 * exec_one. The two MUST be observationally identical -- the fast path is an
 * optimisation, not a second architecture. When they disagree the failure is
 * silent and data-dependent: a game runs for millions of instructions and then
 * decodes one video frame wrong, or wedges.
 *
 * That is not hypothetical. An optimisation pass that grew this fast path
 * shipped two such divergences at once: BSR/JSR/BSRF wrote PR *after* the
 * delay slot (hardware writes it before), and `mov.b/w/l Rm,@-Rn` decremented
 * Rn before reading Rm, so the stored value was wrong whenever m == n.
 * Hand-written cases had covered neither. This file generates the cases
 * instead.
 *
 * Method: build a random straight-line program from the opcodes the fast path
 * claims, run it from an identical seeded machine state through both engines,
 * and compare every architectural register plus the scratch memory window.
 * Branches are excluded here on purpose -- they make the two engines'
 * instruction accounting differ (sh2_step returns 2 for a delayed branch) and
 * are pinned by hand in sh2_semantics.c.
 *
 * Addressing is constrained rather than masked: r13/r14/GBR are dedicated
 * pointer registers seeded into a scratch page and never used as ALU
 * destinations, so every generated access lands in WRAM-H no matter what the
 * arithmetic does to the other registers.
 */
#include "saturn.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int  sh2_step(sh2 *c);
uint64_t sh2_run(sh2 *c, uint64_t n);
void sh2_reset(sh2 *c, saturn *s, int slave, uint32_t pc, uint32_t sp);
void saturn_init(saturn *s);
int  sh2_format(uint16_t op, uint32_t pc, char *out);

static saturn g;

#define CODE     0x06004000u
#define SCRATCH  0x06008000u          /* pointer registers live in here      */
#define MEM_LO   (SCRATCH - 0x400u)   /* compared window: covers @-Rn walks  */
#define MEM_LEN  0x2000u
#define PROG_MAX 24

/* Dedicated pointer registers. Never an ALU destination, so every memory
 * access stays inside the scratch page. */
#define PA 13
#define PB 14

static uint64_t rng_s;
static uint32_t rnd(void)
{
    rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17;
    return (uint32_t)(rng_s >> 32);
}
/* Any register at all. Used for SOURCE operands, which cannot make an
 * address unsafe -- and which must be able to collide with the base register,
 * because `mov.b Rn,@-Rn` (m == n) is exactly where a real bug once hid. */
static uint32_t rnd_reg(void)
{
    return rnd() % 16u;
}

/* A destination register. Usually avoids the two pointer registers so that
 * later memory operands stay inside the scratch page, but not always: letting
 * a pointer be clobbered occasionally is what covers the m == n forms. */
static uint32_t rnd_dst(void)
{
    uint32_t r = rnd() % 16u;
    if ((r == PA || r == PB) && (rnd() % 8u) != 0u) r = rnd() % 13u;
    return r;
}

/* A base register for a memory access: nearly always a seeded pointer, so
 * writes land in the window that gets compared. */
static uint32_t rnd_base(void)
{
    if ((rnd() % 8u) == 0u) return rnd() % 16u;
    return (rnd() & 1u) ? PA : PB;
}

typedef struct {
    uint16_t ops[PROG_MAX];
    int      n;
    uint32_t r[16], mach, macl, pr, gbr, sr;
} prog;

typedef struct {
    uint32_t r[16], mach, macl, pr, gbr, sr, pc;
    uint8_t  mem[MEM_LEN];
} state;

/* One random straight-line opcode. Every encoding here is legal SH-2 and
 * cannot redirect control flow. */
static uint16_t gen_op(void)
{
    uint32_t n = rnd_dst(), m = rnd_reg();
    uint32_t p = rnd_base();               /* base register for memory */
    uint32_t q = rnd_base();

    switch (rnd() % 13u) {
    case 0: {   /* 0x6: forms 0,1,2,4,5,6 load through Rm */
        uint32_t f = rnd() % 16u;
        if (f <= 6u && f != 3u)
            return (uint16_t)(0x6000u | (n << 8) | (p << 4) | f);
        return (uint16_t)(0x6000u | (n << 8) | (m << 4) | f);
    }
    case 1: {   /* 0x2: memory forms need a pointer register in Rn */
        uint32_t f = rnd() % 16u;
        if (f <= 6u && f != 3u) return (uint16_t)(0x2000u | (p << 8) | (m << 4) | f);
        return (uint16_t)(0x2000u | (n << 8) | (m << 4) | f);
    }
    case 2:  return (uint16_t)(0x3000u | (n << 8) | (m << 4) | (rnd() % 16u));
    case 3: {   /* 0x4: shifts, rotates, MAC/PR transfers */
        static const uint8_t c[] = { 0x00,0x01,0x04,0x05,0x08,0x09,0x0A,0x10,
                                     0x11,0x15,0x18,0x19,0x1A,0x20,0x21,0x24,
                                     0x25,0x28,0x29,0x2A };
        static const uint8_t cm[] = { 0x02,0x06,0x12,0x16,0x22,0x26 }; /* memory */
        if (rnd() % 4u == 0u)
            return (uint16_t)(0x4000u | (p << 8) | cm[rnd() % sizeof cm]);
        return (uint16_t)(0x4000u | (n << 8) | c[rnd() % sizeof c]);
    }
    case 4:  return (uint16_t)(0x5000u | (n << 8) | (p << 4) | (rnd() % 16u));
    /* (base already in p) */
    case 5:  return (uint16_t)(0x1000u | (p << 8) | (m << 4) | (rnd() % 16u));
    case 6: {   /* 0x8: byte/word displacement, cmp/eq #imm */
        static const uint8_t c[] = { 0x0, 0x1, 0x4, 0x5 };
        if (rnd() % 5u == 0u) return (uint16_t)(0x8800u | (rnd() & 0xFFu));
        return (uint16_t)(0x8000u | ((uint32_t)c[rnd() % 4u] << 8) |
                          (p << 4) | (rnd() % 16u));
    }
    case 7:  return (uint16_t)(0x7000u | (n << 8) | (rnd() & 0xFFu));
    case 8:  return (uint16_t)(0xE000u | (n << 8) | (rnd() & 0xFFu));
    case 9: {   /* PC-relative loads: read our own program area */
        if (rnd() & 1u) return (uint16_t)(0x9000u | (n << 8) | (rnd() % 24u));
        return (uint16_t)(0xD000u | (n << 8) | (rnd() % 24u));
    }
    case 10: {  /* 0xC: GBR displacement + immediate logic (never 0xC3 trapa) */
        static const uint8_t c[] = { 0x0,0x1,0x2,0x4,0x5,0x6,0x7,0x8,0x9,0xA,0xB };
        return (uint16_t)(0xC000u | ((uint32_t)c[rnd() % sizeof c] << 8) |
                          (rnd() & 0xFFu));
    }
    case 11: { /* 0x0 group: flags, MAC transfers, mac.l */
        static const uint16_t c[] = { 0x0009, 0x0028, 0x0008, 0x0018, 0x0019 };
        switch (rnd() % 6u) {
        case 0: return c[rnd() % 5u];
        case 1: return (uint16_t)(0x0029u | (n << 8));   /* movt     */
        case 2: return (uint16_t)(0x0002u | (n << 8));   /* stc sr   */
        case 3: return (uint16_t)(0x000Au | (n << 8));   /* sts mach */
        case 4: return (uint16_t)(0x001Au | (n << 8));   /* sts macl */
        default: return (uint16_t)(0x000Fu | (p << 8) | (q << 4)); /* mac.l */
        }
    }
    default: return (uint16_t)(0x0007u | (n << 8) | (m << 4)); /* mul.l */
    }
}

static void gen_prog(prog *pr)
{
    int i;
    pr->n = 4 + (int)(rnd() % (PROG_MAX - 4u));
    for (i = 0; i < pr->n; i++) pr->ops[i] = gen_op();
    for (i = 0; i < 16; i++) pr->r[i] = rnd();
    pr->r[PA] = SCRATCH + 0x100u;
    pr->r[PB] = SCRATCH + 0x900u;
    pr->r[15] = SCRATCH + 0x1800u;
    pr->gbr   = SCRATCH + 0x200u;
    pr->mach  = rnd(); pr->macl = rnd(); pr->pr = rnd();
    /* Keep interrupts masked; randomise only the arithmetic flags. */
    pr->sr = 0xF0u | (rnd() & 0x303u);
}

static saturn ga, gb;          /* one machine per engine, stepped in lockstep */

static void snap(state *st, saturn *g, sh2 *c)
{
    int i;
    for (i = 0; i < 16; i++) st->r[i] = c->r[i];
    st->mach = c->mach; st->macl = c->macl; st->pr = c->pr;
    st->gbr = c->gbr;   st->sr = c->sr;     st->pc = c->pc;
    memcpy(st->mem, &g->wram_h[MEM_LO - 0x06000000u], MEM_LEN);
}

static void boot(saturn *g, const prog *pr)
{
    sh2 *c = &g->master;
    int i;
    saturn_init(g);
    for (i = 0; i < pr->n; i++)
        bus_w16(g, CODE + (uint32_t)i * 2u, pr->ops[i]);
    sh2_reset(c, g, 0, CODE, pr->r[15]);
    for (i = 0; i < 16; i++) c->r[i] = pr->r[i];
    c->mach = pr->mach; c->macl = pr->macl; c->pr = pr->pr;
    c->gbr  = pr->gbr;  c->sr   = pr->sr;
}

/* What differs between two snapshots, as a short human-readable string. */
static const char *diff_of(const state *a, const state *b, uint32_t *sa, uint32_t *sb)
{
    static char buf[16];
    int i;
    for (i = 0; i < 16; i++)
        if (a->r[i] != b->r[i]) { sprintf(buf, "r%d", i);
                                  *sa = a->r[i]; *sb = b->r[i]; return buf; }
    if (a->mach != b->mach) { *sa = a->mach; *sb = b->mach; return "mach"; }
    if (a->macl != b->macl) { *sa = a->macl; *sb = b->macl; return "macl"; }
    if (a->pr   != b->pr)   { *sa = a->pr;   *sb = b->pr;   return "pr";   }
    if (a->gbr  != b->gbr)  { *sa = a->gbr;  *sb = b->gbr;  return "gbr";  }
    if (a->sr   != b->sr)   { *sa = a->sr;   *sb = b->sr;   return "sr";   }
    if (a->pc   != b->pc)   { *sa = a->pc;   *sb = b->pc;   return "pc";   }
    if (memcmp(a->mem, b->mem, MEM_LEN) != 0) {
        uint32_t o = 0;
        while (o < MEM_LEN && a->mem[o] == b->mem[o]) o++;
        sprintf(buf, "[%08X]", MEM_LO + o);
        *sa = a->mem[o]; *sb = b->mem[o];
        return buf;
    }
    return NULL;
}

static int fails;

/* Step both engines one instruction at a time and report the FIRST opcode
 * whose effect differs -- the whole program is context, not the finding. */
static void lockstep(const prog *pr)
{
    static state a, b;
    int i;

    boot(&ga, pr);
    boot(&gb, pr);

    for (i = 0; i < pr->n; i++) {
        uint32_t va = 0, vb = 0;
        const char *what;
        char txt[64];

        if (!sh2_step(&ga.master)) break;
        sh2_run(&gb.master, 1);

        snap(&a, &ga, &ga.master);
        snap(&b, &gb, &gb.master);
        what = diff_of(&a, &b, &va, &vb);
        if (!what) continue;

        if (++fails > 10) return;
        if (!sh2_format(pr->ops[i], CODE + (uint32_t)i * 2u, txt)) strcpy(txt, "?");
        printf("  FAIL op %d/%d  %04X  %-24s %s: step=%08X fast=%08X\n",
               i + 1, pr->n, pr->ops[i], txt, what, va, vb);
        {
            int k;
            printf("        context:");
            for (k = 0; k < pr->n; k++) printf(" %04X", pr->ops[k]);
            printf("\n        regs in:");
            for (k = 0; k < 16; k++) printf(" r%d=%08X", k, pr->r[k]);
            printf(" mach=%08X macl=%08X pr=%08X sr=%04X\n",
                   pr->mach, pr->macl, pr->pr, pr->sr);
        }
        return;
    }
}

int main(int argc, char **argv)
{
    unsigned long iters = 20000, k;
    prog pr;

    rng_s = 0x9E3779B97F4A7C15ull;      /* fixed: a failure reproduces */
    if (argc > 1) iters = strtoul(argv[1], NULL, 0);
    if (argc > 2) rng_s = strtoull(argv[2], NULL, 0) | 1ull;

    for (k = 0; k < iters && fails <= 10; k++) {
        gen_prog(&pr);
        lockstep(&pr);
    }

    printf("%s: %lu random programs, %d divergence(s)\n",
           fails ? "FAIL" : "PASS", k, fails);
    return fails ? 1 : 0;
}
