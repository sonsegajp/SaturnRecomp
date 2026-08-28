/* m68k.c -- MC68000 interpreter.
 *
 * Written for the Saturn's sound CPU. The scope is "everything a sound driver
 * uses", which in practice is the whole user-mode 68000 instruction set plus
 * the supervisor bits the driver's reset and interrupt paths touch. The
 * instruction timings are the published cycle counts rounded to the nearest
 * useful value: the sound driver is interrupt- and timer-paced, so being a few
 * cycles out per instruction changes nothing audible, whereas getting the
 * FLAGS wrong breaks its arithmetic immediately.
 *
 * Flag conventions follow the 68000 manual exactly:
 *   X is set like C by the arithmetic that defines it, and left alone by MOVE
 *   and the logicals -- that distinction is what ADDX/SUBX chains depend on.
 */
#include "m68k.h"
#include <stdio.h>
#include <string.h>

#define SR_ALL (M68K_SR_C|M68K_SR_V|M68K_SR_Z|M68K_SR_N|M68K_SR_X)

static void ex_group0(m68k *m, int vector);

/* ------------------------------------------------------------ registers -- */

static uint32_t sign_ext8 (uint8_t v)  { return (uint32_t)(int32_t)(int8_t)v; }
static uint32_t sign_ext16(uint16_t v) { return (uint32_t)(int32_t)(int16_t)v; }

static void set_sr(m68k *m, uint16_t v)
{
    int was_s = (m->sr & M68K_SR_S) != 0;
    int now_s = (v & M68K_SR_S) != 0;
    if (was_s != now_s) {
        /* The active stack pointer swaps with the saved one. */
        if (was_s) { m->ssp = m->a[7]; m->a[7] = m->usp; }
        else       { m->usp = m->a[7]; m->a[7] = m->ssp; }
    }
    m->sr = v;
}

/* --------------------------------------------------------------- fetch -- */

static uint16_t fetch16(m68k *m)
{
    uint16_t v = m68k_r16(m, m->pc);
    m->pc += 2;
    return v;
}

static uint32_t fetch32(m68k *m)
{
    uint32_t v = m68k_r32(m, m->pc);
    m->pc += 4;
    return v;
}

/* ---------------------------------------------------- effective address -- */

/* Size codes: 0 = byte, 1 = word, 2 = long. */
static uint32_t ea_size_bytes(int sz) { return sz == 0 ? 1u : sz == 1 ? 2u : 4u; }

/* Address-register indirect with index, and PC-relative with index, share the
 * brief extension word: bit 15 picks D/A, bit 11 picks word/long index. */
static uint32_t brief_index(m68k *m, uint32_t base)
{
    uint16_t ext = fetch16(m);
    uint32_t idx = (ext & 0x8000u) ? m->a[(ext >> 12) & 7u] : m->d[(ext >> 12) & 7u];
    if (!(ext & 0x0800u)) idx = sign_ext16((uint16_t)idx);
    return base + idx + sign_ext8((uint8_t)(ext & 0xFFu));
}

/* Compute an effective address. Register-direct modes have no address; the
 * callers handle those separately. */
static uint32_t ea_addr(m68k *m, int mode, int reg, int sz)
{
    uint32_t a;
    switch (mode) {
    case 2:  return m->a[reg];                       /* (An)        */
    case 3:                                          /* (An)+       */
        a = m->a[reg];
        /* A7 always moves by 2 so the stack stays word aligned. */
        m->a[reg] += (reg == 7 && sz == 0) ? 2u : ea_size_bytes(sz);
        return a;
    case 4:                                          /* -(An)       */
        m->a[reg] -= (reg == 7 && sz == 0) ? 2u : ea_size_bytes(sz);
        return m->a[reg];
    case 5:  return m->a[reg] + sign_ext16(fetch16(m));   /* d16(An) */
    case 6:  return brief_index(m, m->a[reg]);           /* d8(An,X) */
    case 7:
        switch (reg) {
        case 0: return sign_ext16(fetch16(m));           /* abs.w    */
        case 1: return fetch32(m);                       /* abs.l    */
        case 2: { uint32_t pc = m->pc; return pc + sign_ext16(fetch16(m)); }  /* d16(PC) */
        case 3: { uint32_t pc = m->pc; return brief_index(m, pc); }           /* d8(PC,X) */
        default: break;
        }
        break;
    default: break;
    }
    return 0;
}

static uint32_t ea_read(m68k *m, int mode, int reg, int sz)
{
    if (mode == 0) {                                  /* Dn */
        uint32_t v = m->d[reg];
        return sz == 0 ? (v & 0xFFu) : sz == 1 ? (v & 0xFFFFu) : v;
    }
    if (mode == 1) return m->a[reg];                  /* An (word reads sign-extend at use) */
    if (mode == 7 && reg == 4) {                      /* immediate */
        if (sz == 0) return fetch16(m) & 0xFFu;
        if (sz == 1) return fetch16(m);
        return fetch32(m);
    }
    {
        uint32_t a = ea_addr(m, mode, reg, sz);
        return sz == 0 ? m68k_r8(m, a) : sz == 1 ? m68k_r16(m, a) : m68k_r32(m, a);
    }
}

static void ea_write(m68k *m, int mode, int reg, int sz, uint32_t v)
{
    if (mode == 0) {
        if (sz == 0)      m->d[reg] = (m->d[reg] & 0xFFFFFF00u) | (v & 0xFFu);
        else if (sz == 1) m->d[reg] = (m->d[reg] & 0xFFFF0000u) | (v & 0xFFFFu);
        else              m->d[reg] = v;
        return;
    }
    if (mode == 1) { m->a[reg] = (sz == 1) ? sign_ext16((uint16_t)v) : v; return; }
    {
        uint32_t a = ea_addr(m, mode, reg, sz);
        if (sz == 0)      m68k_w8 (m, a, (uint8_t)v);
        else if (sz == 1) m68k_w16(m, a, (uint16_t)v);
        else              m68k_w32(m, a, v);
    }
}

/* A read-modify-write operand has to compute its address ONCE: recomputing it
 * would step a postincrement twice and push a predecrement twice. */
static uint32_t ea_rmw_addr(m68k *m, int mode, int reg, int sz, int *is_reg)
{
    if (mode == 0 || mode == 1) { *is_reg = 1; return (uint32_t)reg; }
    *is_reg = 0;
    return ea_addr(m, mode, reg, sz);
}

static uint32_t rmw_read(m68k *m, int is_reg, int mode, uint32_t addr, int sz)
{
    if (is_reg) {
        uint32_t v = (mode == 1) ? m->a[addr] : m->d[addr];
        return sz == 0 ? (v & 0xFFu) : sz == 1 ? (v & 0xFFFFu) : v;
    }
    return sz == 0 ? m68k_r8(m, addr) : sz == 1 ? m68k_r16(m, addr) : m68k_r32(m, addr);
}

static void rmw_write(m68k *m, int is_reg, int mode, uint32_t addr, int sz, uint32_t v)
{
    if (is_reg) {
        if (mode == 1) { m->a[addr] = (sz == 1) ? sign_ext16((uint16_t)v) : v; return; }
        if (sz == 0)      m->d[addr] = (m->d[addr] & 0xFFFFFF00u) | (v & 0xFFu);
        else if (sz == 1) m->d[addr] = (m->d[addr] & 0xFFFF0000u) | (v & 0xFFFFu);
        else              m->d[addr] = v;
        return;
    }
    if (sz == 0)      m68k_w8 (m, addr, (uint8_t)v);
    else if (sz == 1) m68k_w16(m, addr, (uint16_t)v);
    else              m68k_w32(m, addr, v);
}

/* ---------------------------------------------------------------- flags -- */

static uint32_t msb_of(int sz) { return sz == 0 ? 0x80u : sz == 1 ? 0x8000u : 0x80000000u; }
static uint32_t mask_of(int sz) { return sz == 0 ? 0xFFu : sz == 1 ? 0xFFFFu : 0xFFFFFFFFu; }

static void set_nz(m68k *m, uint32_t r, int sz)
{
    uint32_t msk = mask_of(sz);
    m->sr &= (uint16_t)~(M68K_SR_N | M68K_SR_Z);
    if ((r & msk) == 0)        m->sr |= M68K_SR_Z;
    if (r & msb_of(sz))        m->sr |= M68K_SR_N;
}

static void set_logic_flags(m68k *m, uint32_t r, int sz)
{
    m->sr &= (uint16_t)~(M68K_SR_C | M68K_SR_V);
    set_nz(m, r, sz);
}

static uint32_t do_add(m68k *m, uint32_t a, uint32_t b, int sz, int with_x)
{
    uint32_t msk = mask_of(sz), msb = msb_of(sz);
    uint32_t x = (with_x && (m->sr & M68K_SR_X)) ? 1u : 0u;
    uint64_t full = (uint64_t)(a & msk) + (uint64_t)(b & msk) + x;
    uint32_t r = (uint32_t)full & msk;
    int carry = (full & ((uint64_t)msk + 1u)) != 0;
    int ovf = ((~(a ^ b) & (a ^ r)) & msb) != 0;
    uint16_t old_z = (uint16_t)(m->sr & M68K_SR_Z);

    m->sr &= (uint16_t)~SR_ALL;
    if (carry) m->sr |= M68K_SR_C | M68K_SR_X;
    if (ovf)   m->sr |= M68K_SR_V;
    if (r & msb) m->sr |= M68K_SR_N;
    /* ADDX leaves Z alone when the result is zero: it ANDs into the old Z so a
     * multi-word sum is zero only if every word was. */
    if (with_x) { if (r == 0 && old_z) m->sr |= M68K_SR_Z; }
    else if (r == 0) m->sr |= M68K_SR_Z;
    return r;
}

static uint32_t do_sub(m68k *m, uint32_t a, uint32_t b, int sz, int with_x, int keep_z)
{
    /* Computes b - a. */
    uint32_t msk = mask_of(sz), msb = msb_of(sz);
    uint32_t x = (with_x && (m->sr & M68K_SR_X)) ? 1u : 0u;
    uint64_t full = (uint64_t)(b & msk) - (uint64_t)(a & msk) - x;
    uint32_t r = (uint32_t)full & msk;
    int borrow = (full & ((uint64_t)msk + 1u)) != 0;
    int ovf = (((a ^ b) & (b ^ r)) & msb) != 0;
    uint16_t old_z = (uint16_t)(m->sr & M68K_SR_Z);

    m->sr &= (uint16_t)~SR_ALL;
    if (borrow) m->sr |= M68K_SR_C | M68K_SR_X;
    if (ovf)    m->sr |= M68K_SR_V;
    if (r & msb) m->sr |= M68K_SR_N;
    if (keep_z) { if (r == 0 && old_z) m->sr |= M68K_SR_Z; }
    else if (r == 0) m->sr |= M68K_SR_Z;
    return r;
}

/* CMP is SUB without the X flag and without writing back. */
static void do_cmp(m68k *m, uint32_t a, uint32_t b, int sz)
{
    uint16_t x = (uint16_t)(m->sr & M68K_SR_X);
    do_sub(m, a, b, sz, 0, 0);
    m->sr = (uint16_t)((m->sr & ~M68K_SR_X) | x);
}

/* ----------------------------------------------------------- conditions -- */

static int cond_true(m68k *m, int c)
{
    int n = (m->sr & M68K_SR_N) != 0, z = (m->sr & M68K_SR_Z) != 0;
    int v = (m->sr & M68K_SR_V) != 0, cc = (m->sr & M68K_SR_C) != 0;
    switch (c) {
    case 0:  return 1;                 /* T  */
    case 1:  return 0;                 /* F  */
    case 2:  return !cc && !z;         /* HI */
    case 3:  return cc || z;           /* LS */
    case 4:  return !cc;               /* CC */
    case 5:  return cc;                /* CS */
    case 6:  return !z;                /* NE */
    case 7:  return z;                 /* EQ */
    case 8:  return !v;                /* VC */
    case 9:  return v;                 /* VS */
    case 10: return !n;                /* PL */
    case 11: return n;                 /* MI */
    case 12: return n == v;            /* GE */
    case 13: return n != v;            /* LT */
    case 14: return !z && (n == v);    /* GT */
    default: return z || (n != v);     /* LE */
    }
}

/* ---------------------------------------------------------- exceptions -- */

static void push32(m68k *m, uint32_t v) { m->a[7] -= 4; m68k_w32(m, m->a[7], v); }
static void push16(m68k *m, uint16_t v) { m->a[7] -= 2; m68k_w16(m, m->a[7], v); }
static uint32_t pop32(m68k *m) { uint32_t v = m68k_r32(m, m->a[7]); m->a[7] += 4; return v; }
static uint16_t pop16(m68k *m) { uint16_t v = m68k_r16(m, m->a[7]); m->a[7] += 2; return v; }

static void exception(m68k *m, int vector)
{
    uint16_t old = m->sr;
    /* A real 68000 halts on a double fault (exception while processing an
     * exception). NiGHTS's sound driver hits this when it gets an interrupt
     * it can't handle -- the vector table entry points at uninitialised RAM,
     * the handler faults, and the fault handler faults. Without this guard
     * the C-level recursion blows the host stack (Windows exit code 116). */
    if (m->in_exception) {
        m->halted = 1;
        m->irq_level = 0;
        return;
    }
    m->in_exception = 1;
    set_sr(m, (uint16_t)((m->sr | M68K_SR_S) & ~M68K_SR_T));
    push32(m, m->pc);
    push16(m, old);
    m->pc = m68k_r32(m, (uint32_t)vector * 4u);
    m->stopped = 0;
    m->cycles += 34;
    m->in_exception = 0;
}

/* Address and bus errors push a larger frame. Nothing in a sound driver should
 * ever take one; if it does, say so once rather than silently looping. */
static void ex_group0(m68k *m, int vector)
{
    static int told;
    if (!told) {
        told = 1;
        printf("[m68k] group-0 exception %d at PC=%06X\n", vector, m->pc);
    }
    m->halted = 1;
}

void m68k_set_irq(m68k *m, int level, int vector)
{
    m->irq_level  = level;
    m->irq_vector = vector;
}

static void check_irq(m68k *m)
{
    int lvl = m->irq_level;
    int cur;
    if (lvl <= 0) return;
    if (m->halted) { m->irq_level = 0; return; }
    cur = (m->sr & M68K_SR_I) >> 8;
    /* Level 7 is non-maskable; everything else must exceed the mask. */
    if (lvl != 7 && lvl <= cur) return;

    {
        int vec = m->irq_vector >= 0 ? m->irq_vector : (24 + lvl);
        uint16_t old = m->sr;
        set_sr(m, (uint16_t)((m->sr | M68K_SR_S) & ~M68K_SR_T));
        m->sr = (uint16_t)((m->sr & ~M68K_SR_I) | ((uint16_t)lvl << 8));
        push32(m, m->pc);
        push16(m, old);
        m->pc = m68k_r32(m, (uint32_t)vec * 4u);
        m->stopped = 0;
        m->cycles += 44;
        m->irq_level = 0;          /* the source re-asserts if still pending */
    }
}

/* --------------------------------------------------------------- reset -- */

void m68k_reset(m68k *m, void *sys)
{
    memset(m, 0, sizeof(*m));
    m->sys = sys;
    m->sr  = M68K_SR_S | 0x0700u;      /* supervisor, all interrupts masked */
    m->ssp = m68k_r32(m, 0);
    m->a[7] = m->ssp;
    m->pc  = m68k_r32(m, 4);
    m->irq_vector = -1;
}

/* ------------------------------------------------------------- MOVEM ---- */

static void do_movem(m68k *m, uint16_t op)
{
    uint16_t list = fetch16(m);
    int sz   = (op & 0x0040u) ? 2 : 1;          /* long : word */
    int dir  = (op & 0x0400u) != 0;             /* 1 = memory -> registers */
    int mode = (op >> 3) & 7, reg = op & 7;
    uint32_t step = (sz == 2) ? 4u : 2u;
    int i;

    if (!dir && mode == 4) {
        /* -(An): registers are stored A7..D0, and An itself is written with
         * its ORIGINAL value on a 68000. */
        uint32_t addr = m->a[reg];
        for (i = 0; i < 16; i++) {
            if (!(list & (1u << i))) continue;
            {
                int r = 15 - i;
                uint32_t v = (r < 8) ? m->d[r] : m->a[r - 8];
                addr -= step;
                if (sz == 2) m68k_w32(m, addr, v); else m68k_w16(m, addr, (uint16_t)v);
            }
        }
        m->a[reg] = addr;
        return;
    }
    {
        uint32_t addr = (mode == 3) ? m->a[reg] : ea_addr(m, mode, reg, sz);
        for (i = 0; i < 16; i++) {
            if (!(list & (1u << i))) continue;
            if (dir) {
                uint32_t v = (sz == 2) ? m68k_r32(m, addr)
                                       : sign_ext16(m68k_r16(m, addr));
                if (i < 8) m->d[i] = v; else m->a[i - 8] = v;
            } else {
                uint32_t v = (i < 8) ? m->d[i] : m->a[i - 8];
                if (sz == 2) m68k_w32(m, addr, v); else m68k_w16(m, addr, (uint16_t)v);
            }
            addr += step;
        }
        if (mode == 3) m->a[reg] = addr;         /* (An)+ writes back */
    }
}

/* -------------------------------------------------------------- shifts -- */

static uint32_t do_shift(m68k *m, uint32_t v, int cnt, int sz, int type, int left)
{
    uint32_t msk = mask_of(sz), msb = msb_of(sz);
    int carry = 0, ovf = 0;
    int bits = sz == 0 ? 8 : sz == 1 ? 16 : 32;
    int i;

    v &= msk;
    if (cnt == 0) {
        /* A zero shift count clears C and V but leaves X alone. */
        m->sr &= (uint16_t)~(M68K_SR_C | M68K_SR_V);
        set_nz(m, v, sz);
        return v;
    }
    for (i = 0; i < cnt; i++) {
        switch (type) {
        case 0:  /* arithmetic */
            if (left) {
                carry = (v & msb) != 0;
                { uint32_t before = v; v = (v << 1) & msk;
                  if (((before ^ v) & msb) != 0) ovf = 1; }
            } else {
                carry = v & 1u;
                v = ((v & msb) ? (v | ~msk) : v) >> 1;
                v &= msk;
                if (v & (msb >> 1)) { /* sign preserved by the OR above */ }
                if (msb & (v << 1)) { /* keep the sign bit set */ }
                v |= (carry, 0u);
                /* Re-apply the sign: an arithmetic right shift keeps bit N. */
                if (m->sr & 0) { }
            }
            break;
        case 1:  /* logical */
            if (left) { carry = (v & msb) != 0; v = (v << 1) & msk; }
            else      { carry = v & 1u;         v = (v >> 1) & msk; }
            break;
        case 2:  /* rotate through X */
            if (left) {
                int nx = (v & msb) != 0;
                v = ((v << 1) | ((m->sr & M68K_SR_X) ? 1u : 0u)) & msk;
                carry = nx;
                m->sr = (uint16_t)(nx ? (m->sr | M68K_SR_X) : (m->sr & ~M68K_SR_X));
            } else {
                int nx = v & 1u;
                v = ((v >> 1) | ((m->sr & M68K_SR_X) ? msb : 0u)) & msk;
                carry = nx;
                m->sr = (uint16_t)(nx ? (m->sr | M68K_SR_X) : (m->sr & ~M68K_SR_X));
            }
            break;
        default: /* rotate */
            if (left) { carry = (v & msb) != 0; v = ((v << 1) | (carry ? 1u : 0u)) & msk; }
            else      { carry = v & 1u;         v = ((v >> 1) | (carry ? msb : 0u)) & msk; }
            break;
        }
    }
    (void)bits;

    m->sr &= (uint16_t)~(M68K_SR_C | M68K_SR_V);
    if (carry) m->sr |= M68K_SR_C;
    if (ovf)   m->sr |= M68K_SR_V;
    /* ASL/ASR/LSL/LSR set X from the last bit out; the rotates through X did
     * it themselves above, and plain ROL/ROR leave X alone. */
    if (type == 0 || type == 1)
        m->sr = (uint16_t)(carry ? (m->sr | M68K_SR_X) : (m->sr & ~M68K_SR_X));
    set_nz(m, v, sz);
    return v;
}

/* An arithmetic right shift written plainly: the loop above got clumsy trying
 * to preserve the sign in place, and this is the operation the sound driver's
 * volume scaling actually uses, so keep it obviously correct. */
static uint32_t do_asr(m68k *m, uint32_t v, int cnt, int sz)
{
    uint32_t msk = mask_of(sz), msb = msb_of(sz);
    int carry = 0, i;
    v &= msk;
    if (cnt == 0) {
        m->sr &= (uint16_t)~(M68K_SR_C | M68K_SR_V);
        set_nz(m, v, sz);
        return v;
    }
    for (i = 0; i < cnt; i++) {
        carry = v & 1u;
        v = ((v >> 1) | (v & msb)) & msk;
    }
    m->sr &= (uint16_t)~(M68K_SR_C | M68K_SR_V);
    if (carry) m->sr |= M68K_SR_C | M68K_SR_X;
    else       m->sr &= (uint16_t)~M68K_SR_X;
    set_nz(m, v, sz);
    return v;
}

/* --------------------------------------------------------------- execute - */

static void step(m68k *m)
{
    uint16_t op;
    int mode, reg, sz;

    if (m->stopped) { m->cycles += 4; return; }
    if ((m->pc & 0x00FFFFFFu) >= 0x100000u && (m->pc & 0x00FFFFFFu) < 0x100000u + 0x1000u) {
        /* SCSP register space: not executable */
    } else if ((m->pc & 0x00FFFFFFu) >= 0x080000u) {
        m->halted = 1; return;
    }
    op = fetch16(m);
    m->cycles += 4;

    switch (op >> 12) {

    case 0x0: {                                   /* immediates and bit ops */
        if ((op & 0x0138u) == 0x0108u) {          /* MOVEP */
            int dr = (op >> 7) & 1, szl = (op >> 6) & 1;
            uint32_t addr = m->a[op & 7] + sign_ext16(fetch16(m));
            int dn = (op >> 9) & 7;
            if (dr) {                              /* register -> memory */
                if (szl) {
                    m68k_w8(m, addr,      (uint8_t)(m->d[dn] >> 24));
                    m68k_w8(m, addr + 2u, (uint8_t)(m->d[dn] >> 16));
                    m68k_w8(m, addr + 4u, (uint8_t)(m->d[dn] >> 8));
                    m68k_w8(m, addr + 6u, (uint8_t)m->d[dn]);
                } else {
                    m68k_w8(m, addr,      (uint8_t)(m->d[dn] >> 8));
                    m68k_w8(m, addr + 2u, (uint8_t)m->d[dn]);
                }
            } else {
                uint32_t v = 0;
                if (szl) {
                    v = ((uint32_t)m68k_r8(m, addr) << 24) |
                        ((uint32_t)m68k_r8(m, addr + 2u) << 16) |
                        ((uint32_t)m68k_r8(m, addr + 4u) << 8) |
                         (uint32_t)m68k_r8(m, addr + 6u);
                    m->d[dn] = v;
                } else {
                    v = ((uint32_t)m68k_r8(m, addr) << 8) | m68k_r8(m, addr + 2u);
                    m->d[dn] = (m->d[dn] & 0xFFFF0000u) | (v & 0xFFFFu);
                }
            }
            m->cycles += 16;
            break;
        }
        if ((op & 0x0F00u) == 0x0800u || (op & 0x0100u)) {   /* BTST/BCHG/BCLR/BSET */
            int dynamic = (op & 0x0100u) != 0;
            int bitop = dynamic ? ((op >> 6) & 3) : ((op >> 6) & 3);
            uint32_t bit;
            mode = (op >> 3) & 7; reg = op & 7;
            if (dynamic) bit = m->d[(op >> 9) & 7];
            else         bit = fetch16(m) & 0xFFu;
            if (mode == 0) {
                uint32_t mask = 1u << (bit & 31u);
                uint32_t v = m->d[reg];
                m->sr = (uint16_t)((v & mask) ? (m->sr & ~M68K_SR_Z)
                                              : (m->sr | M68K_SR_Z));
                if (bitop == 1) m->d[reg] = v ^ mask;
                if (bitop == 2) m->d[reg] = v & ~mask;
                if (bitop == 3) m->d[reg] = v | mask;
                m->cycles += 6;
            } else {
                int is_reg;
                uint32_t addr = ea_rmw_addr(m, mode, reg, 0, &is_reg);
                uint32_t mask = 1u << (bit & 7u);
                uint32_t v = rmw_read(m, is_reg, mode, addr, 0);
                m->sr = (uint16_t)((v & mask) ? (m->sr & ~M68K_SR_Z)
                                              : (m->sr | M68K_SR_Z));
                if (bitop == 1) rmw_write(m, is_reg, mode, addr, 0, v ^ mask);
                if (bitop == 2) rmw_write(m, is_reg, mode, addr, 0, v & ~mask);
                if (bitop == 3) rmw_write(m, is_reg, mode, addr, 0, v | mask);
                m->cycles += 8;
            }
            break;
        }
        /* ORI / ANDI / SUBI / ADDI / EORI / CMPI, plus the CCR/SR forms. */
        sz = (op >> 6) & 3;
        if (sz == 3) { exception(m, 4); break; }
        mode = (op >> 3) & 7; reg = op & 7;
        {
            int which = (op >> 9) & 7;
            uint32_t imm = (sz == 2) ? fetch32(m) : fetch16(m);
            if (sz == 0) imm &= 0xFFu;

            if (mode == 7 && reg == 4 && (which == 0 || which == 1 || which == 5)) {
                /* ORI/ANDI/EORI to CCR or SR. */
                if (sz == 0) {
                    uint16_t ccr = (uint16_t)(m->sr & 0x1Fu);
                    uint16_t v = (uint16_t)(imm & 0x1Fu);
                    ccr = (uint16_t)(which == 0 ? (ccr | v) : which == 1 ? (ccr & v) : (ccr ^ v));
                    m->sr = (uint16_t)((m->sr & ~0x1Fu) | ccr);
                } else {
                    if (!(m->sr & M68K_SR_S)) { exception(m, 8); break; }
                    {
                        uint16_t v = (uint16_t)imm;
                        uint16_t nv = (uint16_t)(which == 0 ? (m->sr | v)
                                              : which == 1 ? (m->sr & v)
                                                           : (m->sr ^ v));
                        set_sr(m, nv);
                    }
                }
                m->cycles += 16;
                break;
            }
            {
                int is_reg;
                uint32_t addr = ea_rmw_addr(m, mode, reg, sz, &is_reg);
                uint32_t v = rmw_read(m, is_reg, mode, addr, sz);
                uint32_t r = 0;
                switch (which) {
                case 0: r = v | imm;  set_logic_flags(m, r, sz); break;
                case 1: r = v & imm;  set_logic_flags(m, r, sz); break;
                case 2: r = do_sub(m, imm, v, sz, 0, 0); break;
                case 3: r = do_add(m, imm, v, sz, 0);    break;
                case 5: r = v ^ imm;  set_logic_flags(m, r, sz); break;
                case 6: do_cmp(m, imm, v, sz); r = v; break;
                default: r = v; break;
                }
                if (which != 6) rmw_write(m, is_reg, mode, addr, sz, r);
                m->cycles += 8;
            }
        }
        break;
    }

    case 0x1: case 0x2: case 0x3: {                     /* MOVE / MOVEA */
        int op_sz = (op >> 12) == 1 ? 0 : (op >> 12) == 3 ? 1 : 2;
        int src_mode = (op >> 3) & 7, src_reg = op & 7;
        int dst_mode = (op >> 6) & 7, dst_reg = (op >> 9) & 7;
        uint32_t v = ea_read(m, src_mode, src_reg, op_sz);
        if (src_mode == 1 && op_sz == 1) v = sign_ext16((uint16_t)v);
        if (dst_mode == 1) {                            /* MOVEA: no flags */
            m->a[dst_reg] = (op_sz == 1) ? sign_ext16((uint16_t)v) : v;
        } else {
            ea_write(m, dst_mode, dst_reg, op_sz, v);
            set_logic_flags(m, v, op_sz);
        }
        m->cycles += 4;
        break;
    }

    case 0x4: {                                          /* miscellaneous */
        if ((op & 0x01C0u) == 0x01C0u) {                 /* LEA */
            m->a[(op >> 9) & 7] = ea_addr(m, (op >> 3) & 7, op & 7, 2);
            m->cycles += 4;
            break;
        }
        if ((op & 0x0180u) == 0x0180u) {                 /* CHK */
            int32_t bound = (int32_t)(int16_t)ea_read(m, (op >> 3) & 7, op & 7, 1);
            int32_t v = (int32_t)(int16_t)m->d[(op >> 9) & 7];
            if (v < 0 || v > bound) exception(m, 6);
            m->cycles += 10;
            break;
        }
        switch (op & 0x0FC0u) {
        case 0x0000: case 0x0040: case 0x0080: {         /* NEGX */
            int s2 = (op >> 6) & 3;
            int is_reg; uint32_t addr = ea_rmw_addr(m, (op >> 3) & 7, op & 7, s2, &is_reg);
            uint32_t v = rmw_read(m, is_reg, (op >> 3) & 7, addr, s2);
            uint32_t r = do_sub(m, v, 0, s2, 1, 1);
            rmw_write(m, is_reg, (op >> 3) & 7, addr, s2, r);
            m->cycles += 4;
            break;
        }
        case 0x0200: case 0x0240: case 0x0280: {         /* CLR */
            int s2 = (op >> 6) & 3;
            ea_write(m, (op >> 3) & 7, op & 7, s2, 0);
            m->sr = (uint16_t)((m->sr & ~(M68K_SR_N|M68K_SR_V|M68K_SR_C)) | M68K_SR_Z);
            m->cycles += 4;
            break;
        }
        case 0x0400: case 0x0440: case 0x0480: {         /* NEG */
            int s2 = (op >> 6) & 3;
            int is_reg; uint32_t addr = ea_rmw_addr(m, (op >> 3) & 7, op & 7, s2, &is_reg);
            uint32_t v = rmw_read(m, is_reg, (op >> 3) & 7, addr, s2);
            uint32_t r = do_sub(m, v, 0, s2, 0, 0);
            rmw_write(m, is_reg, (op >> 3) & 7, addr, s2, r);
            m->cycles += 4;
            break;
        }
        case 0x00C0: {                                   /* MOVE from SR */
            int is_reg; uint32_t addr = ea_rmw_addr(m, (op >> 3) & 7, op & 7, 1, &is_reg);
            rmw_write(m, is_reg, (op >> 3) & 7, addr, 1, m->sr);
            m->cycles += 6;
            break;
        }
        case 0x04C0: {                                   /* MOVE to CCR */
            uint32_t v = ea_read(m, (op >> 3) & 7, op & 7, 1);
            m->sr = (uint16_t)((m->sr & 0xFF00u) | (v & 0x1Fu));
            m->cycles += 12;
            break;
        }
        case 0x06C0: {                                   /* MOVE to SR */
            /* The first instruction of essentially every 68000 driver is
             * MOVE #$2700,SR. Without this it decoded as illegal, the core
             * took an exception into an empty vector table, and then ran
             * whatever sound RAM happened to hold -- which is precisely the
             * wandering PC that made the sound side look alive but silent. */
            uint32_t v;
            if (!(m->sr & M68K_SR_S)) { exception(m, 8); break; }
            v = ea_read(m, (op >> 3) & 7, op & 7, 1);
            set_sr(m, (uint16_t)v);
            m->cycles += 12;
            break;
        }
        case 0x0600: case 0x0640: case 0x0680: {         /* NOT */
            int s2 = (op >> 6) & 3;
            int is_reg; uint32_t addr = ea_rmw_addr(m, (op >> 3) & 7, op & 7, s2, &is_reg);
            uint32_t v = rmw_read(m, is_reg, (op >> 3) & 7, addr, s2);
            uint32_t r = ~v & mask_of(s2);
            rmw_write(m, is_reg, (op >> 3) & 7, addr, s2, r);
            set_logic_flags(m, r, s2);
            m->cycles += 4;
            break;
        }
        case 0x0880: {                                   /* EXT.W / MOVEM.W */
            int dn = op & 7;
            if (((op >> 3) & 7) != 0) { do_movem(m, op); m->cycles += 12; break; }
            m->d[dn] = (m->d[dn] & 0xFFFF0000u) | (sign_ext8((uint8_t)m->d[dn]) & 0xFFFFu);
            set_logic_flags(m, m->d[dn], 1);
            m->cycles += 4;
            break;
        }
        case 0x08C0: {                                   /* EXT.L / MOVEM.L */
            int dn = op & 7;
            if (((op >> 3) & 7) != 0) { do_movem(m, op); m->cycles += 12; break; }
            m->d[dn] = sign_ext16((uint16_t)m->d[dn]);
            set_logic_flags(m, m->d[dn], 2);
            m->cycles += 4;
            break;
        }
        case 0x0840: {                                   /* SWAP / PEA */
            if (((op >> 3) & 7) == 0) {
                int dn = op & 7;
                m->d[dn] = (m->d[dn] >> 16) | (m->d[dn] << 16);
                set_logic_flags(m, m->d[dn], 2);
            } else {
                push32(m, ea_addr(m, (op >> 3) & 7, op & 7, 2));
            }
            m->cycles += 6;
            break;
        }
        case 0x0A00: case 0x0A40: case 0x0A80: {         /* TST */
            int s2 = (op >> 6) & 3;
            uint32_t v = ea_read(m, (op >> 3) & 7, op & 7, s2);
            set_logic_flags(m, v, s2);
            m->cycles += 4;
            break;
        }
        case 0x0AC0: {                                   /* TAS */
            int is_reg; uint32_t addr = ea_rmw_addr(m, (op >> 3) & 7, op & 7, 0, &is_reg);
            uint32_t v = rmw_read(m, is_reg, (op >> 3) & 7, addr, 0);
            set_logic_flags(m, v, 0);
            rmw_write(m, is_reg, (op >> 3) & 7, addr, 0, v | 0x80u);
            m->cycles += 10;
            break;
        }
        case 0x0C00: case 0x0C40: case 0x0C80:           /* MOVEM reg->mem */
        case 0x0CC0:
            do_movem(m, op);
            m->cycles += 12;
            break;
        default:
            if ((op & 0x0FF0u) == 0x0E40u) {             /* TRAP */
                exception(m, 32 + (op & 15));
                break;
            }
            if ((op & 0x0FF8u) == 0x0E50u) {             /* LINK */
                int an = op & 7;
                push32(m, m->a[an]);
                m->a[an] = m->a[7];
                m->a[7] += sign_ext16(fetch16(m));
                m->cycles += 16;
                break;
            }
            if ((op & 0x0FF8u) == 0x0E58u) {             /* UNLK */
                int an = op & 7;
                m->a[7] = m->a[an];
                m->a[an] = pop32(m);
                m->cycles += 12;
                break;
            }
            if ((op & 0x0FF0u) == 0x0E60u) {             /* MOVE USP */
                if (!(m->sr & M68K_SR_S)) { exception(m, 8); break; }
                if (op & 8) m->a[op & 7] = m->usp;
                else        m->usp = m->a[op & 7];
                m->cycles += 4;
                break;
            }
            switch (op) {
            case 0x4E70:                                  /* RESET */
                if (!(m->sr & M68K_SR_S)) { exception(m, 8); break; }
                m->cycles += 132;
                break;
            case 0x4E71: m->cycles += 4; break;            /* NOP */
            case 0x4E72:                                   /* STOP */
                if (!(m->sr & M68K_SR_S)) { exception(m, 8); break; }
                set_sr(m, fetch16(m));
                m->stopped = 1;
                m->cycles += 4;
                break;
            case 0x4E73: {                                 /* RTE */
                if (!(m->sr & M68K_SR_S)) { exception(m, 8); break; }
                { uint16_t nsr = pop16(m); uint32_t npc = pop32(m);
                  set_sr(m, nsr); m->pc = npc; }
                m->cycles += 20;
                break;
            }
            case 0x4E75: m->pc = pop32(m); m->cycles += 16; break;   /* RTS */
            case 0x4E76:                                             /* TRAPV */
                if (m->sr & M68K_SR_V) exception(m, 7);
                m->cycles += 4;
                break;
            case 0x4E77: {                                           /* RTR */
                uint16_t ccr = pop16(m);
                m->sr = (uint16_t)((m->sr & ~0x1Fu) | (ccr & 0x1Fu));
                m->pc = pop32(m);
                m->cycles += 20;
                break;
            }
            default:
                if ((op & 0x0FC0u) == 0x0E80u) {          /* JSR */
                    uint32_t t = ea_addr(m, (op >> 3) & 7, op & 7, 2);
                    push32(m, m->pc);
                    m->pc = t;
                    m->cycles += 16;
                    break;
                }
                if ((op & 0x0FC0u) == 0x0EC0u) {          /* JMP */
                    m->pc = ea_addr(m, (op >> 3) & 7, op & 7, 2);
                    m->cycles += 8;
                    break;
                }
                {
                    static int told2;
                    if (!told2) {
                        told2 = 1;
                        printf("[m68k] illegal/unimpl in group 4: %04X at PC=%06X\n", op, m->pc - 2);
                    }
                }
                m->halted = 1;
                break;
            }
            break;
        }
        break;
    }

    case 0x5: {                                    /* ADDQ / SUBQ / Scc / DBcc */
        sz = (op >> 6) & 3;
        if (sz == 3) {
            int cond = (op >> 8) & 15;
            if (((op >> 3) & 7) == 1) {            /* DBcc */
                int dn = op & 7;
                int32_t disp = (int32_t)sign_ext16(fetch16(m));
                if (!cond_true(m, cond)) {
                    uint16_t c = (uint16_t)(m->d[dn] & 0xFFFFu);
                    c = (uint16_t)(c - 1u);
                    m->d[dn] = (m->d[dn] & 0xFFFF0000u) | c;
                    if (c != 0xFFFFu) m->pc = m->pc - 2u + (uint32_t)disp;
                }
                m->cycles += 10;
            } else {                                /* Scc */
                int is_reg;
                uint32_t addr = ea_rmw_addr(m, (op >> 3) & 7, op & 7, 0, &is_reg);
                rmw_write(m, is_reg, (op >> 3) & 7, addr, 0,
                          cond_true(m, cond) ? 0xFFu : 0x00u);
                m->cycles += 6;
            }
            break;
        }
        {
            uint32_t data = (op >> 9) & 7;
            int is_add = ((op >> 8) & 1) == 0;
            int mode2 = (op >> 3) & 7, reg2 = op & 7;
            if (data == 0) data = 8;
            if (mode2 == 1) {
                /* Address registers take the whole long and set no flags. */
                m->a[reg2] = is_add ? m->a[reg2] + data : m->a[reg2] - data;
                m->cycles += 8;
                break;
            }
            {
                int is_reg;
                uint32_t addr = ea_rmw_addr(m, mode2, reg2, sz, &is_reg);
                uint32_t v = rmw_read(m, is_reg, mode2, addr, sz);
                uint32_t r = is_add ? do_add(m, data, v, sz, 0)
                                    : do_sub(m, data, v, sz, 0, 0);
                rmw_write(m, is_reg, mode2, addr, sz, r);
                m->cycles += 4;
            }
        }
        break;
    }

    case 0x6: {                                          /* Bcc / BSR / BRA */
        int cond = (op >> 8) & 15;
        int32_t disp = (int32_t)(int8_t)(op & 0xFFu);
        uint32_t base = m->pc;
        if (disp == 0)  disp = (int32_t)sign_ext16(fetch16(m));
        else if (disp == -1) { disp = (int32_t)fetch32(m); }
        if (cond == 1) {                                  /* BSR */
            push32(m, m->pc);
            m->pc = base + (uint32_t)disp;
            m->cycles += 18;
        } else if (cond == 0 || cond_true(m, cond)) {
            m->pc = base + (uint32_t)disp;
            m->cycles += 10;
        } else {
            m->cycles += 8;
        }
        break;
    }

    case 0x7:                                             /* MOVEQ */
        m->d[(op >> 9) & 7] = sign_ext8((uint8_t)(op & 0xFFu));
        set_logic_flags(m, m->d[(op >> 9) & 7], 2);
        m->cycles += 4;
        break;

    case 0x8: {                                    /* OR / DIVU / DIVS / SBCD */
        int dn = (op >> 9) & 7;
        sz = (op >> 6) & 3;
        if (sz == 3) {                                    /* DIVU / DIVS */
            int is_signed = (op & 0x0100u) != 0;
            uint32_t src = ea_read(m, (op >> 3) & 7, op & 7, 1);
            if ((src & 0xFFFFu) == 0) { exception(m, 5); break; }
            m->sr &= (uint16_t)~(M68K_SR_C | M68K_SR_V);
            if (is_signed) {
                int32_t a = (int32_t)m->d[dn];
                int32_t b = (int32_t)(int16_t)src;
                int32_t q = a / b, r = a % b;
                if (q > 32767 || q < -32768) { m->sr |= M68K_SR_V; }
                else {
                    m->d[dn] = ((uint32_t)r << 16) | ((uint32_t)q & 0xFFFFu);
                    set_nz(m, (uint32_t)q & 0xFFFFu, 1);
                }
            } else {
                uint32_t a = m->d[dn], b = src & 0xFFFFu;
                uint32_t q = a / b, r = a % b;
                if (q > 0xFFFFu) { m->sr |= M68K_SR_V; }
                else {
                    m->d[dn] = (r << 16) | (q & 0xFFFFu);
                    set_nz(m, q & 0xFFFFu, 1);
                }
            }
            m->cycles += 140;
            break;
        }
        if ((op & 0x01F0u) == 0x0100u) {                  /* SBCD */
            int rm = (op & 8) != 0, sreg = op & 7;
            uint32_t s, d, r;
            if (rm) {
                m->a[sreg] -= 1; s = m68k_r8(m, m->a[sreg]);
                m->a[dn] -= 1;   d = m68k_r8(m, m->a[dn]);
            } else { s = m->d[sreg] & 0xFFu; d = m->d[dn] & 0xFFu; }
            {
                int x = (m->sr & M68K_SR_X) ? 1 : 0;
                int lo = (int)(d & 15u) - (int)(s & 15u) - x;
                int hi = (int)((d >> 4) & 15u) - (int)((s >> 4) & 15u);
                int borrow = 0;
                if (lo < 0) { lo += 10; hi -= 1; }
                if (hi < 0) { hi += 10; borrow = 1; }
                r = (uint32_t)((hi << 4) | lo) & 0xFFu;
                m->sr &= (uint16_t)~(M68K_SR_C | M68K_SR_X);
                if (borrow) m->sr |= M68K_SR_C | M68K_SR_X;
                if (r) m->sr &= (uint16_t)~M68K_SR_Z;
            }
            if (rm) m68k_w8(m, m->a[dn], (uint8_t)r);
            else    m->d[dn] = (m->d[dn] & 0xFFFFFF00u) | r;
            m->cycles += 6;
            break;
        }
        {
            int dir = (op >> 8) & 1;
            if (!dir) {
                uint32_t v = ea_read(m, (op >> 3) & 7, op & 7, sz);
                uint32_t r = (m->d[dn] | v) & mask_of(sz);
                ea_write(m, 0, dn, sz, r);
                set_logic_flags(m, r, sz);
            } else {
                int is_reg;
                uint32_t addr = ea_rmw_addr(m, (op >> 3) & 7, op & 7, sz, &is_reg);
                uint32_t v = rmw_read(m, is_reg, (op >> 3) & 7, addr, sz);
                uint32_t r = (v | m->d[dn]) & mask_of(sz);
                rmw_write(m, is_reg, (op >> 3) & 7, addr, sz, r);
                set_logic_flags(m, r, sz);
            }
            m->cycles += 4;
        }
        break;
    }

    case 0x9: case 0xD: {                                 /* SUB / ADD family */
        int is_add = (op >> 12) == 0xD;
        int dn = (op >> 9) & 7;
        sz = (op >> 6) & 3;
        if (sz == 3) {                                    /* SUBA / ADDA */
            int asz = (op & 0x0100u) ? 2 : 1;
            uint32_t v = ea_read(m, (op >> 3) & 7, op & 7, asz);
            if (asz == 1) v = sign_ext16((uint16_t)v);
            m->a[dn] = is_add ? m->a[dn] + v : m->a[dn] - v;
            m->cycles += 8;
            break;
        }
        if ((op & 0x0130u) == 0x0100u) {                  /* SUBX / ADDX */
            int rm = (op & 8) != 0, sreg = op & 7;
            uint32_t s, d, r;
            if (rm) {
                uint32_t st = (sreg == 7 && sz == 0) ? 2u : ea_size_bytes(sz);
                m->a[sreg] -= st;
                s = sz == 0 ? m68k_r8(m, m->a[sreg]) : sz == 1 ? m68k_r16(m, m->a[sreg])
                                                               : m68k_r32(m, m->a[sreg]);
                m->a[dn] -= (dn == 7 && sz == 0) ? 2u : ea_size_bytes(sz);
                d = sz == 0 ? m68k_r8(m, m->a[dn]) : sz == 1 ? m68k_r16(m, m->a[dn])
                                                             : m68k_r32(m, m->a[dn]);
            } else {
                s = m->d[sreg]; d = m->d[dn];
            }
            r = is_add ? do_add(m, s, d, sz, 1) : do_sub(m, s, d, sz, 1, 1);
            if (rm) {
                if (sz == 0) m68k_w8(m, m->a[dn], (uint8_t)r);
                else if (sz == 1) m68k_w16(m, m->a[dn], (uint16_t)r);
                else m68k_w32(m, m->a[dn], r);
            } else ea_write(m, 0, dn, sz, r);
            m->cycles += 4;
            break;
        }
        {
            int dir = (op >> 8) & 1;
            if (!dir) {
                uint32_t v = ea_read(m, (op >> 3) & 7, op & 7, sz);
                uint32_t r = is_add ? do_add(m, v, m->d[dn], sz, 0)
                                    : do_sub(m, v, m->d[dn], sz, 0, 0);
                ea_write(m, 0, dn, sz, r);
            } else {
                int is_reg;
                uint32_t addr = ea_rmw_addr(m, (op >> 3) & 7, op & 7, sz, &is_reg);
                uint32_t v = rmw_read(m, is_reg, (op >> 3) & 7, addr, sz);
                uint32_t r = is_add ? do_add(m, m->d[dn], v, sz, 0)
                                    : do_sub(m, m->d[dn], v, sz, 0, 0);
                rmw_write(m, is_reg, (op >> 3) & 7, addr, sz, r);
            }
            m->cycles += 4;
        }
        break;
    }

    case 0xB: {                                    /* CMP / CMPA / EOR / CMPM */
        int dn = (op >> 9) & 7;
        sz = (op >> 6) & 3;
        if (sz == 3) {                                    /* CMPA */
            int asz = (op & 0x0100u) ? 2 : 1;
            uint32_t v = ea_read(m, (op >> 3) & 7, op & 7, asz);
            if (asz == 1) v = sign_ext16((uint16_t)v);
            do_cmp(m, v, m->a[dn], 2);
            m->cycles += 6;
            break;
        }
        if ((op & 0x0100u) == 0) {                        /* CMP */
            uint32_t v = ea_read(m, (op >> 3) & 7, op & 7, sz);
            do_cmp(m, v, m->d[dn], sz);
            m->cycles += 4;
            break;
        }
        if (((op >> 3) & 7) == 1) {                       /* CMPM */
            int sreg = op & 7;
            uint32_t st = ea_size_bytes(sz);
            uint32_t s = sz == 0 ? m68k_r8(m, m->a[sreg]) : sz == 1 ? m68k_r16(m, m->a[sreg])
                                                                    : m68k_r32(m, m->a[sreg]);
            uint32_t d;
            m->a[sreg] += (sreg == 7 && sz == 0) ? 2u : st;
            d = sz == 0 ? m68k_r8(m, m->a[dn]) : sz == 1 ? m68k_r16(m, m->a[dn])
                                                         : m68k_r32(m, m->a[dn]);
            m->a[dn] += (dn == 7 && sz == 0) ? 2u : st;
            do_cmp(m, s, d, sz);
            m->cycles += 12;
            break;
        }
        {                                                  /* EOR */
            int is_reg;
            uint32_t addr = ea_rmw_addr(m, (op >> 3) & 7, op & 7, sz, &is_reg);
            uint32_t v = rmw_read(m, is_reg, (op >> 3) & 7, addr, sz);
            uint32_t r = (v ^ m->d[dn]) & mask_of(sz);
            rmw_write(m, is_reg, (op >> 3) & 7, addr, sz, r);
            set_logic_flags(m, r, sz);
            m->cycles += 4;
        }
        break;
    }

    case 0xC: {                                    /* AND / MUL / ABCD / EXG */
        int dn = (op >> 9) & 7;
        sz = (op >> 6) & 3;
        if (sz == 3) {                                    /* MULU / MULS */
            int is_signed = (op & 0x0100u) != 0;
            uint32_t src = ea_read(m, (op >> 3) & 7, op & 7, 1);
            if (is_signed) m->d[dn] = (uint32_t)((int32_t)(int16_t)m->d[dn] *
                                                 (int32_t)(int16_t)src);
            else           m->d[dn] = (m->d[dn] & 0xFFFFu) * (src & 0xFFFFu);
            set_logic_flags(m, m->d[dn], 2);
            m->cycles += 70;
            break;
        }
        if ((op & 0x01F0u) == 0x0100u) {                  /* ABCD */
            int rm = (op & 8) != 0, sreg = op & 7;
            uint32_t s, d, r;
            if (rm) {
                m->a[sreg] -= 1; s = m68k_r8(m, m->a[sreg]);
                m->a[dn] -= 1;   d = m68k_r8(m, m->a[dn]);
            } else { s = m->d[sreg] & 0xFFu; d = m->d[dn] & 0xFFu; }
            {
                int x = (m->sr & M68K_SR_X) ? 1 : 0;
                int lo = (int)(d & 15u) + (int)(s & 15u) + x;
                int hi = (int)((d >> 4) & 15u) + (int)((s >> 4) & 15u);
                int carry = 0;
                if (lo > 9) { lo -= 10; hi += 1; }
                if (hi > 9) { hi -= 10; carry = 1; }
                r = (uint32_t)((hi << 4) | lo) & 0xFFu;
                m->sr &= (uint16_t)~(M68K_SR_C | M68K_SR_X);
                if (carry) m->sr |= M68K_SR_C | M68K_SR_X;
                if (r) m->sr &= (uint16_t)~M68K_SR_Z;
            }
            if (rm) m68k_w8(m, m->a[dn], (uint8_t)r);
            else    m->d[dn] = (m->d[dn] & 0xFFFFFF00u) | r;
            m->cycles += 6;
            break;
        }
        if ((op & 0x0130u) == 0x0100u) {                  /* EXG */
            int rx = dn, ry = op & 7;
            switch (op & 0x00F8u) {
            case 0x0040: { uint32_t t = m->d[rx]; m->d[rx] = m->d[ry]; m->d[ry] = t; break; }
            case 0x0048: { uint32_t t = m->a[rx]; m->a[rx] = m->a[ry]; m->a[ry] = t; break; }
            default:     { uint32_t t = m->d[rx]; m->d[rx] = m->a[ry]; m->a[ry] = t; break; }
            }
            m->cycles += 6;
            break;
        }
        {
            int dir = (op >> 8) & 1;
            if (!dir) {
                uint32_t v = ea_read(m, (op >> 3) & 7, op & 7, sz);
                uint32_t r = (m->d[dn] & v) & mask_of(sz);
                ea_write(m, 0, dn, sz, r);
                set_logic_flags(m, r, sz);
            } else {
                int is_reg;
                uint32_t addr = ea_rmw_addr(m, (op >> 3) & 7, op & 7, sz, &is_reg);
                uint32_t v = rmw_read(m, is_reg, (op >> 3) & 7, addr, sz);
                uint32_t r = (v & m->d[dn]) & mask_of(sz);
                rmw_write(m, is_reg, (op >> 3) & 7, addr, sz, r);
                set_logic_flags(m, r, sz);
            }
            m->cycles += 4;
        }
        break;
    }

    case 0xE: {                                            /* shifts/rotates */
        sz = (op >> 6) & 3;
        if (sz == 3) {                                     /* memory, 1 bit */
            int type = (op >> 9) & 3, left = (op >> 8) & 1;
            int is_reg;
            uint32_t addr = ea_rmw_addr(m, (op >> 3) & 7, op & 7, 1, &is_reg);
            uint32_t v = rmw_read(m, is_reg, (op >> 3) & 7, addr, 1);
            uint32_t r = (type == 0 && !left) ? do_asr(m, v, 1, 1)
                                              : do_shift(m, v, 1, 1, type, left);
            rmw_write(m, is_reg, (op >> 3) & 7, addr, 1, r);
            m->cycles += 8;
            break;
        }
        {
            int left = (op >> 8) & 1;
            int type = (op >> 3) & 3;
            int ir   = (op >> 5) & 1;
            int dn   = op & 7;
            int cnt  = ir ? (int)(m->d[(op >> 9) & 7] & 63u)
                          : (((op >> 9) & 7) ? ((op >> 9) & 7) : 8);
            uint32_t v = m->d[dn];
            uint32_t r = (type == 0 && !left) ? do_asr(m, v, cnt, sz)
                                              : do_shift(m, v, cnt, sz, type, left);
            ea_write(m, 0, dn, sz, r);
            m->cycles += 6 + 2 * cnt;
        }
        break;
    }

    default:
        {
            static int told;
            if (!told) {
                told = 1;
                printf("[m68k] unimpl opcode %04X at PC=%06X (halting sound CPU)\n",
                       op, m->pc - 2);
            }
        }
        m->halted = 1;
        m->irq_level = 0;
        break;
    }
}

uint32_t m68k_run(m68k *m, uint32_t cycles)
{
    uint64_t start = m->cycles;
    if (m->halted) return cycles;
    while (m->cycles - start < cycles) {
        m->stepping = 1;
        check_irq(m);
        if (m->halted) { m->stepping = 0; break; }
        if (m->cycles - start >= cycles) { m->stepping = 0; break; }
        step(m);
        m->stepping = 0;
    }
    return (uint32_t)(m->cycles - start);
}

int m68k_disasm(m68k *m, uint32_t pc, char *out, int outsz)
{
    uint16_t op = m68k_r16(m, pc);
    snprintf(out, (size_t)outsz, "%04X", op);
    return 2;
}
