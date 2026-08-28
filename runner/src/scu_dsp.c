/* Saturn SCU geometry DSP.
 *
 * The BIOS uses this processor to build the genuine boot-logo planar model;
 * leaving 0x05FE0080 as a passive register makes the master SH-2 wait forever
 * on PPAF.EX.  The instruction, pipeline, data-RAM and DSP-DMA behaviour here
 * follows Ymir's scu_dsp.cpp, expressed in C and wired to SaturnRecomp's bus.
 */
#include "saturn.h"

#include <stdint.h>
#include <string.h>

#define MASK48 0x0000FFFFFFFFFFFFULL

/* SCU::Reset(false) preserves DSP program/data RAM but resets every execution
 * register and pending DMA, matching Ymir SCUDSP::Reset(false). */
void scu_dsp_soft_reset(saturn *s)
{
    uint32_t program[256];
    uint32_t data[4][64];
    memcpy(program, s->scu_dsp.program, sizeof program);
    memcpy(data, s->scu_dsp.data, sizeof data);
    memset(&s->scu_dsp, 0, sizeof s->scu_dsp);
    memcpy(s->scu_dsp.program, program, sizeof program);
    memcpy(s->scu_dsp.data, data, sizeof data);
}

static uint32_t bits(uint32_t v, unsigned lo, unsigned n)
{
    return (v >> lo) & ((1u << n) - 1u);
}

static int32_t sext(uint32_t v, unsigned n)
{
    uint32_t m = 1u << (n - 1u);
    return (int32_t)((v ^ m) - m);
}

static uint64_t sx48(uint32_t v)
{
    return (uint64_t)(int64_t)(int32_t)v & MASK48;
}

static void inc_pc(scu_dsp_state *d)
{
    if (d->looping) {
        if (d->loop_count == 0) {
            d->looping = 0;
            d->pc++;
        }
        d->loop_count = (uint16_t)((d->loop_count - 1u) & 0x0FFFu);
    } else {
        d->pc++;
    }
}

static int cond(const scu_dsp_state *d, uint8_t c)
{
    int result = 0;
    if ((c & 1u) && d->zero)    result = 1;
    if ((c & 2u) && d->sign)    result = 1;
    if ((c & 4u) && d->carry)   result = 1;
    if ((c & 8u) && d->dma_run) result = 1;
    return result == (int)((c >> 5) & 1u);
}

static void alu_op(scu_dsp_state *d, unsigned op)
{
    uint64_t a, b, r;
    uint32_t al = (uint32_t)d->ac, pl = (uint32_t)d->p, rl;

    d->alu = d->ac;
    switch (op) {
    case 0x0: return;
    case 0x1: rl = al & pl; d->carry = 0; break;
    case 0x2: rl = al | pl; d->carry = 0; break;
    case 0x3: rl = al ^ pl; d->carry = 0; break;
    case 0x4:
        a = al; b = pl; r = a + b; rl = (uint32_t)r;
        d->carry = (uint8_t)((r >> 32) & 1u);
        d->overflow |= (uint8_t)((((~(a ^ b)) & (a ^ r)) >> 31) & 1u);
        break;
    case 0x5:
        a = al; b = pl; r = a - b; rl = (uint32_t)r;
        d->carry = (uint8_t)((r >> 32) & 1u);
        d->overflow |= (uint8_t)((((a ^ b) & (a ^ r)) >> 31) & 1u);
        break;
    case 0x6:
        a = d->ac & MASK48; b = d->p & MASK48; r = a + b;
        d->zero = (uint8_t)((r << 16) == 0);
        d->sign = (uint8_t)((int64_t)(r << 16) < 0);
        d->carry = (uint8_t)((r >> 48) & 1u);
        d->overflow |= (uint8_t)((((~(a ^ b)) & (a ^ r)) >> 47) & 1u);
        d->alu = r & MASK48;
        return;
    case 0x8:
        d->carry = al & 1u; rl = (uint32_t)((int32_t)al >> 1); break;
    case 0x9:
        d->carry = al & 1u; rl = (al >> 1) | (al << 31); break;
    case 0xA:
        d->carry = (al >> 31) & 1u; rl = al << 1; break;
    case 0xB:
        d->carry = (al >> 31) & 1u; rl = (al << 1) | (al >> 31); break;
    case 0xF:
        d->carry = (al >> 24) & 1u; rl = (al << 8) | (al >> 24); break;
    default:
        return;
    }
    d->alu = (d->alu & 0xFFFF00000000ULL) | rl;
    d->zero = (uint8_t)(rl == 0);
    d->sign = (uint8_t)((int32_t)rl < 0);
}

static int is_bbus(uint32_t a)
{
    a &= 0x07FFFFFFu;
    return a >= 0x05A00000u && a < 0x06000000u;
}

/* Ymir currently completes a pending DSP DMA as one burst whenever an
 * instruction needs its bank.  Preserve that ordering; bus timing can be
 * made incremental later without changing the architectural result. */
static void run_dma(saturn *s)
{
    scu_dsp_state *d = &s->scu_dsp;
    unsigned guard = 256;
    unsigned bank = d->dma_to_d0 ? d->dma_src : d->dma_dst;
    int use_data = bank <= 3u;
    int use_prog = !d->dma_to_d0 && bank == 4u;
    uint8_t prog = (uint8_t)d->dma_pc;

    if (!d->dma_run) return;
    do {
        uint32_t value;
        if (d->dma_to_d0) {
            value = use_data ? d->data[bank][d->ct[bank] & 0x3Fu] : 0xFFFFFFFFu;
            if (is_bbus(d->dma_addr_d0)) {
                bus_w16(s, d->dma_addr_d0, (uint16_t)(value >> 16));
                d->dma_addr_d0 += d->dma_addr_inc;
                bus_w16(s, d->dma_addr_d0, (uint16_t)value);
                d->dma_addr_d0 += d->dma_addr_inc;
            } else {
                bus_w32(s, d->dma_addr_d0 & ~3u, value);
                d->dma_addr_d0 += d->dma_addr_inc;
            }
        } else {
            if (is_bbus(d->dma_addr_d0)) {
                value = ((uint32_t)bus_r16(s, d->dma_addr_d0 | 0u) << 16) |
                        bus_r16(s, d->dma_addr_d0 | 2u);
                d->dma_addr_d0 += 4u;
            } else {
                value = bus_r32(s, d->dma_addr_d0);
                d->dma_addr_d0 += d->dma_addr_inc;
            }
            if (use_data) d->data[bank][d->ct[bank] & 0x3Fu] = value;
            else if (use_prog) d->program[prog++] = value;
        }
        d->dma_addr_d0 &= 0x07FFFFFFu;
        if (use_data) d->ct[bank] = (uint8_t)((d->ct[bank] + 1u) & 0x3Fu);
        d->dma_count--;
    } while (d->dma_count != 0 && --guard != 0);

    if (d->dma_count == 0) {
        if (!d->dma_hold) {
            if (d->dma_addr_inc == 0) {
                if (d->dma_to_d0) d->dma_write_addr += 4u;
                else              d->dma_read_addr  += 4u;
            } else if (d->dma_to_d0) {
                if (is_bbus(d->dma_addr_d0)) d->dma_addr_d0 -= d->dma_addr_inc * 2u;
                else                         d->dma_addr_d0 -= d->dma_addr_inc;
                d->dma_write_addr = (d->dma_addr_d0 + 4u) & ~3u;
            } else {
                d->dma_read_addr = d->dma_addr_d0;
            }
        }
        d->dma_run = 0;
        if (use_prog) {
            d->next_instr = 0;
            d->pc = d->loop_top;
        }
    }
}

static uint32_t read_source(saturn *s, unsigned index)
{
    scu_dsp_state *d = &s->scu_dsp;
    if (index < 8u) {
        unsigned b = index & 3u;
        if (d->dma_run && b == (d->dma_to_d0 ? d->dma_src : d->dma_dst)) run_dma(s);
        d->inc_ct |= ((index >> 2) & 1u) << (b * 8u);
        return d->data[b][d->ct[b] & 0x3Fu];
    }
    if (index == 9u)  return (uint32_t)d->alu;
    if (index == 10u) return (uint32_t)(d->alu >> 16);
    return 0xFFFFFFFFu;
}

static void write_d1(saturn *s, unsigned index, uint32_t value)
{
    scu_dsp_state *d = &s->scu_dsp;
    if (index < 4u) {
        if (d->dma_run && index == (d->dma_to_d0 ? d->dma_src : d->dma_dst)) run_dma(s);
        d->data[index][d->ct[index] & 0x3Fu] = value;
        d->inc_ct |= 1u << (index * 8u);
    } else switch (index) {
    case 4: d->rx = (int32_t)value; break;
    case 5: d->p = sx48(value); break;
    case 6: d->dma_read_addr  = (value << 2) & 0x07FFFFFCu; break;
    case 7: d->dma_write_addr = (value << 2) & 0x07FFFFFCu; break;
    case 10: d->loop_count = (uint16_t)(value & 0xFFFu); break;
    case 11: d->loop_top = (uint8_t)value; break;
    case 12: case 13: case 14: case 15: {
        unsigned b = index & 3u;
        if (d->dma_run && b == (d->dma_to_d0 ? d->dma_src : d->dma_dst)) run_dma(s);
        d->ct[b] = (uint8_t)(value & 0x3Fu);
        d->inc_ct &= ~(1u << (b * 8u));
        break;
    }
    }
}

static void write_imm(saturn *s, unsigned index, uint32_t value)
{
    scu_dsp_state *d = &s->scu_dsp;
    if (index < 4u) {
        if (d->dma_run && index == (d->dma_to_d0 ? d->dma_src : d->dma_dst)) run_dma(s);
        d->data[index][d->ct[index] & 0x3Fu] = value;
        d->ct[index] = (uint8_t)((d->ct[index] + 1u) & 0x3Fu);
    } else switch (index) {
    case 4: d->rx = (int32_t)value; break;
    case 5: d->p = sx48(value); break;
    case 6: d->dma_read_addr  = (value << 2) & 0x07FFFFFCu; break;
    case 7: d->dma_write_addr = (value << 2) & 0x07FFFFFCu; break;
    case 10: d->loop_count = (uint16_t)(value & 0xFFFu); break;
    case 12:
        d->loop_top = d->pc;
        d->pc = (uint8_t)value;
        if (d->dma_run) d->dma_pc = d->pc;
        break;
    }
}

static void cmd_operation(saturn *s, uint32_t op)
{
    scu_dsp_state *d = &s->scu_dsp;
    unsigned xop = bits(op, 23, 3), xsrc = bits(op, 20, 3);
    unsigned yop = bits(op, 17, 3), ysrc = bits(op, 14, 3);
    unsigned dop = bits(op, 12, 2), dst = bits(op, 8, 4);
    uint8_t reads = 0;
    uint32_t value;

    inc_pc(d);
    alu_op(d, bits(op, 26, 4));

    if ((xop & 3u) == 2u)
        d->p = (uint64_t)((int64_t)d->rx * (int64_t)d->ry) & MASK48;
    if (xop >= 3u) {
        value = read_source(s, xsrc); if (xsrc < 8u) reads |= 1u << (xsrc & 3u);
        if ((xop & 3u) == 3u) d->p = sx48(value);
        if (xop & 4u) d->rx = (int32_t)value;
    }

    if ((yop & 3u) == 1u) d->ac = 0;
    else if ((yop & 3u) == 2u) d->ac = d->alu;
    if (yop >= 3u) {
        value = read_source(s, ysrc); if (ysrc < 8u) reads |= 1u << (ysrc & 3u);
        if ((yop & 3u) == 3u) d->ac = sx48(value);
        if (yop & 4u) d->ry = (int32_t)value;
    }

    if (dop == 1u) {
        value = (uint32_t)(int32_t)(int8_t)op;
        if (dst < 4u && (reads & (1u << dst))) d->ct[dst] &= (uint8_t)~1u;
        else if (!(dst == 4u && (xop & 4u)) && !(dst == 5u && (xop & 2u)))
            write_d1(s, dst, value);
    } else if (dop == 3u) {
        unsigned src = op & 15u;
        if (src < 8u) reads |= 1u << (src & 3u);
        if (dst >= 4u || !(reads & (1u << dst))) {
            if (dst == 4u && (xop & 4u)) {
                /* X bus wins. */
            } else if (dst == 5u && (xop & 2u)) {
                (void)read_source(s, src);
            } else {
                write_d1(s, dst, read_source(s, src));
            }
        } else if (src >= 4u && src < 8u && dst != (src & 3u)) {
            d->inc_ct |= 1u << ((src & 3u) * 8u);
        }
    }

    for (unsigned b = 0; b < 4; b++)
        d->ct[b] = (uint8_t)((d->ct[b] + ((d->inc_ct >> (b * 8u)) & 0xFFu)) & 0x3Fu);
    d->inc_ct = 0;
}

static void cmd_load(saturn *s, uint32_t op)
{
    scu_dsp_state *d = &s->scu_dsp;
    unsigned dst = bits(op, 26, 4);
    int write_pc = dst == 12u;
    int32_t imm;

    if (d->looping) {
        if (d->loop_count == 0) {
            d->looping = 0;
            if (!write_pc) d->pc++;
        }
        d->loop_count = (uint16_t)((d->loop_count - 1u) & 0xFFFu);
    } else if (!write_pc) d->pc++;

    if (op & (1u << 25)) {
        imm = sext(op & 0x7FFFFu, 19);
        if (!cond(d, (uint8_t)bits(op, 19, 6))) return;
    } else imm = sext(op & 0x1FFFFFFu, 25);
    write_imm(s, dst, (uint32_t)imm);
}

static void cmd_special(saturn *s, uint32_t op)
{
    scu_dsp_state *d = &s->scu_dsp;
    unsigned sub = bits(op, 28, 2);

    if (sub == 0u) {
        unsigned stride, b;
        inc_pc(d);
        if (d->dma_run) run_dma(s);
        d->dma_run = 1;
        d->dma_to_d0 = (uint8_t)bits(op, 12, 1);
        d->dma_hold = (uint8_t)bits(op, 14, 1);
        if (op & (1u << 13)) {
            b = op & 3u;
            d->dma_count = (uint8_t)d->data[b][d->ct[b] & 0x3Fu];
            if (op & 4u) d->ct[b] = (uint8_t)((d->ct[b] + 1u) & 0x3Fu);
        } else d->dma_count = (uint8_t)op;
        b = bits(op, 8, 3);
        stride = bits(op, 15, 3);
        if (d->dma_to_d0) {
            d->dma_src = (uint8_t)b;
            d->dma_addr_inc = (1u << stride) & ~1u;
            d->dma_addr_d0 = d->dma_write_addr;
        } else {
            d->dma_dst = (uint8_t)b;
            d->dma_addr_inc = (1u << (stride & 2u)) & ~1u;
            d->dma_addr_d0 = d->dma_read_addr;
        }
    } else if (sub == 1u) {
        inc_pc(d);
        if ((op & (1u << 25)) && bits(op, 19, 6) != 0 &&
            !cond(d, (uint8_t)bits(op, 19, 6))) return;
        d->pc = (uint8_t)op;
    } else if (sub == 2u) {
        if (op & (1u << 27)) {
            d->looping = 1;
            inc_pc(d);
        } else {
            if (d->loop_count != 0) d->pc = d->loop_top;
            else inc_pc(d);
            d->loop_count = (uint16_t)((d->loop_count - 1u) & 0xFFFu);
        }
    } else {
        inc_pc(d);
        d->executing = 0;
        if ((op & (1u << 27)) && !d->ended) {
            d->ended = 1;
            scu_raise(s, 5);
        }
    }
}

void scu_dsp_tick(saturn *s, uint32_t cycles)
{
    scu_dsp_state *d = &s->scu_dsp;
    uint64_t n = cycles + d->cycle_spill;
    d->cycle_spill = n & 1u;
    n >>= 1;

    while (n--) {
        uint32_t op;
        if (!d->executing && !d->step) { run_dma(s); return; }
        if (d->paused) { run_dma(s); return; }
        op = d->next_instr;
        d->next_instr = d->program[d->pc];
        if (d->dma_run) { d->dma_pc = d->pc; run_dma(s); }
        switch (op >> 30) {
        case 0: cmd_operation(s, op); break;
        case 2: cmd_load(s, op); break;
        case 3: cmd_special(s, op); break;
        default: break;
        }
        d->step = 0;
    }
}

uint32_t scu_dsp_read_reg(saturn *s, uint32_t off)
{
    scu_dsp_state *d = &s->scu_dsp;
    off &= 0xFCu;
    if (off == 0x80u) {
        uint32_t v = d->pc;
        if (d->executing && !d->paused) v |= 1u << 16;
        if (d->ended)    v |= 1u << 18;
        if (d->overflow) v |= 1u << 19;
        if (d->carry)    v |= 1u << 20;
        if (d->zero)     v |= 1u << 21;
        if (d->sign)     v |= 1u << 22;
        if (d->dma_run)  v |= 1u << 23;
        d->overflow = 0;
        d->ended = 0;
        return v;
    }
    if (off == 0x84u || off == 0x88u) return 0;
    if (off == 0x8Cu) {
        uint32_t v;
        unsigned b, a;
        if (d->executing && !d->paused) return 0xFFFFFFFFu;
        b = (d->data_addr >> 6) & 3u; a = d->data_addr & 0x3Fu;
        v = d->data[b][a]; d->data_addr++;
        return v;
    }
    return s->scu_reg[off >> 2];
}

void scu_dsp_write_reg(saturn *s, uint32_t off, uint32_t value)
{
    scu_dsp_state *d = &s->scu_dsp;
    off &= 0xFCu;
    if (off == 0x80u) {
        if (value & (1u << 15)) { d->pc = (uint8_t)value; d->next_instr = 0; }
        if (value & (1u << 25)) d->paused = 1;
        else if (value & (1u << 26)) d->paused = 0;
        else {
            d->executing = (uint8_t)((value >> 16) & 1u);
            d->step      = (uint8_t)((value >> 17) & 1u);
        }
    } else if (off == 0x84u) {
        if (!d->executing || d->paused) d->program[d->pc++] = value;
    } else if (off == 0x88u) {
        d->data_addr = (uint8_t)value;
    } else if (off == 0x8Cu) {
        if (!d->executing || d->paused) {
            unsigned b = (d->data_addr >> 6) & 3u, a = d->data_addr & 0x3Fu;
            d->data_addr++;
            d->data[b][a] = value;
        }
    }
}
