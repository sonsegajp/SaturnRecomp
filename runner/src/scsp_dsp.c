/* scsp_dsp.c -- the SCSP's DSP effect processor.
 *
 * Ported from Ymir (scsp_dsp.hpp / scsp_dsp_instr.hpp). This is a 128-step
 * microcoded DSP with its own program RAM, coefficient RAM, address registers
 * and a ring buffer in sound RAM. Every slot has TWO output paths: a direct
 * one (DISDL/DIPAN) and an effect send (EFSDL/EFPAN) that runs through here
 * and comes back via EFREG. We implemented only the direct path and hard-muted
 * any slot whose direct level was zero, so anything a game routed through the
 * DSP was silent.
 *
 * MEASURED before writing this: NiGHTS writes the whole 512-word program area
 * (MPRO 8192 writes, COEF 896, MADRS 448), so the effect path is genuinely in
 * use and not a theoretical gap.
 *
 * Register map (Ymir scsp.hpp:400-415):
 *   0x600-0x67F  MIXS   mix stack input
 *   0x700-0x77F  COEF   coefficients, 13-bit, stored << 3
 *   0x780-0x7BF  MADRS  memory address registers
 *   0x800-0xBFF  MPRO   program, 4 words per 64-bit instruction
 */
#include "saturn.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* The DSP's 16-bit wave-memory format: a 4-bit exponent and 11-bit mantissa
 * with a sign, expanded to 24-bit two's complement. */
static uint32_t dsp_f2i(uint16_t value)
{
    uint32_t sign_xor = (uint32_t)(((int32_t)((value & 0x8000u) << 16)) >> 1);
    uint32_t exp = (value >> 11) & 0xFu;
    uint32_t ret = value & 0x7FFu;
    uint32_t sh;

    if (exp < 12u) ret |= 0x800u;
    ret <<= 11u + 8u;
    ret ^= sign_xor;
    sh = 8u + (exp < 11u ? exp : 11u);
    ret = (uint32_t)(((int32_t)ret) >> sh);
    return ret & 0xFFFFFFu;
}

static int dsp_clz20(uint32_t v)
{
    /* GCC lowers this to the host CLZ/LZCNT/BSR instruction. Keep the zero
     * case explicit because __builtin_clz(0) is undefined. */
    return v ? __builtin_clz(v) : 32;
}

static uint32_t dsp_i2f(uint32_t value)
{
    uint32_t shifted = value << 8u;
    uint32_t sign_xor = (uint32_t)(((int32_t)shifted) >> 31);
    uint32_t exp = (uint32_t)dsp_clz20(((shifted ^ sign_xor) << 1) | (1u << 19));
    uint32_t shift, ret;

    if (exp > 0x1Fu) exp = 0x1Fu;
    shift = exp - (exp == 12u ? 1u : 0u);
    ret = (uint32_t)(((int32_t)shifted) >> (19u - shift));
    ret &= 0x87FFu;
    ret |= exp << 11u;
    return ret;
}

/* Sign-extend the low `bits` bits of v, DISCARDING everything above them --
 * i.e. Ymir's bit::sign_extend<N>, which extracts N bits and then extends.
 *
 * The mask is the whole point and was missing. Without it `(v ^ m) - m` only
 * sign-extends a value that is ALREADY narrow, and hands back anything wider
 * unchanged. So the SCSP's fixed-width registers never truncated: a MIXS
 * contribution that should have been clamped to 20 bits (+-524,288) arrived as
 * -1,050,608 (MEASURED), the accumulator ran to tens of millions, the DSP read
 * it as `mixs << 4` into what should be 24 bits, and every downstream stage
 * railed -- which is the "loud and staticy" effect return. It also explains why
 * clamping the accumulator with this same helper made the measured peak go UP
 * instead of binding it. */
static int32_t sext(uint32_t v, int bits)
{
    uint32_t m = 1u << (bits - 1);
    v &= (m << 1) - 1u;
    return (int32_t)((v ^ m) - m);
}

static uint16_t dsp_read_wram(saturn *s)
{
    uint32_t a = s->dsp.rw_addr * 2u;
    if (a >= SOUND_RAM_SZ) return 0;
    return (uint16_t)((s->sound_ram[a] << 8) | s->sound_ram[a + 1u]);
}

static void dsp_write_wram(saturn *s)
{
    uint32_t a = s->dsp.rw_addr * 2u;
    if (a >= SOUND_RAM_SZ) return;
    s->sound_ram[a]      = (uint8_t)(s->dsp.write_value >> 8);
    s->sound_ram[a + 1u] = (uint8_t)s->dsp.write_value;
}

void scsp_dsp_reset(saturn *s)
{
    memset(&s->dsp, 0, sizeof s->dsp);
    /* The program is aligned to operation 7, which processes slot i-6:
     * 4 steps per slot, -6*4 = -24 = 104 (0x68) modulo 128. */
    s->dsp.pc = 0x68u;
    s->dsp.mixs_null = 0xFFFFu;
    s->dsp.rbl = (0x2000u << 0) - 1u;
}

void scsp_dsp_update_rbp(saturn *s, uint16_t lead) { s->dsp.rbp = (uint32_t)lead << 12u; }
void scsp_dsp_update_rbl(saturn *s, uint16_t len)  { s->dsp.rbl = (0x2000u << len) - 1u; }

/* MIXS accumulates each slot's effect send for this sample; the buffer is
 * double-banked and swapped when the program wraps. */
void scsp_dsp_mixs_write(saturn *s, unsigned off, int32_t value)
{
    unsigned idx = (off & 0xFu) | s->dsp.mixs_gen;
    value = sext((uint32_t)value, 20);
    if (s->dsp.mixs_null & (1u << (off & 0xFu))) {
        s->dsp.mixs_null &= ~(1u << (off & 0xFu));
        s->dsp.mixs[idx] = value;
    } else {
        s->dsp.mixs[idx] += value;
    }
/* Each contribution is now genuinely 20 bits (see sext above), so the
       running sum stays in range without a separate clamp. */
}

void scsp_dsp_step(saturn *s)
{
    scsp_dsp_state *d = &s->dsp;

    if (d->pc < d->prog_len) {
        uint64_t I = d->program[d->pc];
        /* Instruction fields, Ymir scsp_dsp_instr.hpp. */
        unsigned NXADR = (unsigned)( I        & 1u);
        unsigned ADREB = (unsigned)((I >>  1) & 1u);
        unsigned MASA  = (unsigned)((I >>  2) & 0x1Fu);
        unsigned NOFL  = (unsigned)((I >>  8) & 1u);
        unsigned CRA   = (unsigned)((I >>  9) & 0x3Fu);
        unsigned BSEL  = (unsigned)((I >> 16) & 1u);
        unsigned ZERO  = (unsigned)((I >> 17) & 1u);
        unsigned NEGB  = (unsigned)((I >> 18) & 1u);
        unsigned YRL   = (unsigned)((I >> 19) & 1u);
        unsigned SHFT0 = (unsigned)((I >> 20) & 1u);
        unsigned SHFT1 = (unsigned)((I >> 21) & 1u);
        unsigned SHFT  = (unsigned)((I >> 20) & 3u);
        unsigned FRCL  = (unsigned)((I >> 22) & 1u);
        unsigned ADRL  = (unsigned)((I >> 23) & 1u);
        unsigned EWA   = (unsigned)((I >> 24) & 0xFu);
        unsigned EWT   = (unsigned)((I >> 28) & 1u);
        unsigned MRD   = (unsigned)((I >> 29) & 1u);
        unsigned MWT   = (unsigned)((I >> 30) & 1u);
        unsigned TABLE = (unsigned)((I >> 31) & 1u);
        unsigned IWA   = (unsigned)((I >> 32) & 0x1Fu);
        unsigned IWT   = (unsigned)((I >> 37) & 1u);
        unsigned IRA   = (unsigned)((I >> 38) & 0x3Fu);
        unsigned YSEL  = (unsigned)((I >> 45) & 3u);
        unsigned XSEL  = (unsigned)((I >> 47) & 1u);
        unsigned TWA   = (unsigned)((I >> 48) & 0x7Fu);
        unsigned TWT   = (unsigned)((I >> 55) & 1u);
        unsigned TRA   = (unsigned)((I >> 56) & 0x7Fu);

        unsigned tra_addr, twa_addr;
        int32_t inputs, temp, xval, sga;
        uint16_t yval;
        int32_t shifter;
        int64_t product;
        uint16_t addr;

        if (IRA <= 0x1Fu)      d->inputs = d->mems[IRA];
        else if (IRA <= 0x2Fu) d->inputs = d->mixs[((IRA & 0xFu) | d->mixs_gen) ^ 0x10u] << 4;
        else if (IRA <= 0x31u) d->inputs = (int32_t)d->exts[IRA & 1u] << 8;

        tra_addr = (TRA + d->mdec_ct) & 0x7Fu;
        twa_addr = (TWA + d->mdec_ct) & 0x7Fu;

        inputs = d->inputs;
        temp   = d->temp[tra_addr];
        xval   = XSEL ? inputs : temp;

        switch (YSEL) {
        case 0:  yval = (uint16_t)d->frc_reg; break;
        case 1:  yval = d->coef[CRA]; break;
        case 2:  yval = (uint16_t)((d->y_reg >> 11) & 0x1FFFu); break;
        default: yval = (uint16_t)((d->y_reg >> 4) & 0xFFFu); break;
        }

        if (YRL) d->y_reg = (uint32_t)inputs & 0xFFFFFFu;

        shifter = (int32_t)((uint32_t)sext(d->sft_reg, 26) << (SHFT0 ^ SHFT1));
        if (SHFT1 == 0) {
            if (shifter >  0x7FFFFF) shifter =  0x7FFFFF;
            if (shifter < -0x800000) shifter = -0x800000;
        } else {
            shifter = sext((uint32_t)shifter, 24);
        }

        if (FRCL) {
            d->frc_reg = (SHFT == 3) ? ((uint32_t)shifter & 0xFFFu)
                                     : (((uint32_t)shifter >> 11) & 0x1FFFu);
        }

        if (ZERO) {
            sga = 0;
        } else {
            sga = BSEL ? (int32_t)d->sft_reg : temp;
            if (NEGB) sga = -sga;
        }

        product = ((int64_t)sext(yval, 13) * (int64_t)xval) >> 12;
        d->sft_reg = (uint32_t)((product + sga) & 0x3FFFFFF);

        if (EWT) d->efreg[EWA] = (int16_t)(shifter >> 8);
        if (TWT) d->temp[twa_addr] = shifter;
        if (IWT) d->mems[IWA] = sext(d->read_value, 24);

        if (d->read_pending) {
            uint16_t tmp = dsp_read_wram(s);
            d->read_value = d->read_nofl ? ((uint32_t)tmp << 8) : dsp_f2i(tmp);
            {   /* what the delay line actually hands back */
                int32_t rv = (int32_t)d->read_value;
                if (rv < 0) rv = -rv;
                if (rv > s->dsp_readval_peak) s->dsp_readval_peak = rv;
            }
            d->read_pending = 0;
            d->read_nofl = 0;
        } else if (d->write_pending) {
            dsp_write_wram(s);
            d->write_pending = 0;
        }

        addr = (uint16_t)(d->madrs[MASA] + NXADR);
        if (ADREB) addr = (uint16_t)(addr + sext(d->adrs_reg, 12));
        if (!TABLE) addr = (uint16_t)((addr + d->mdec_ct) & d->rbl);
        d->rw_addr = (addr + d->rbp) & 0x7FFFFu;

        if (MRD) { d->read_pending = 1; d->read_nofl = NOFL; s->dsp_mrd++; }
        if (MWT) {
            s->dsp_mwt++;
            d->write_pending = 1;
            d->write_value = (uint16_t)(NOFL ? ((uint32_t)shifter >> 8)
                                             : dsp_i2f((uint32_t)shifter));
        }

        if (ADRL) {
            d->adrs_reg = (SHFT == 3) ? (((uint32_t)shifter >> 12) & 0xFFFu)
                                      : (((uint32_t)inputs >> 16) & 0xFFFu);
        }
    } else if (d->write_pending) {
        dsp_write_wram(s);
        d->write_pending = 0;
    }

    d->pc++;
    if (d->pc == 0x80u) {
        d->pc = 0;
        d->mixs_gen ^= 0x10u;      /* swap MIXS banks */
        d->mixs_null = 0xFFFFu;
        d->mdec_ct--;
    }
}
