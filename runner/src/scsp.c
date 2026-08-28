/* scsp.c -- Saturn Custom Sound Processor.
 *
 * SCOPE, stated honestly: this renders the SCSP's 32 PCM slots -- address,
 * loop, pitch, envelope, level and pan -- and keeps the full register file so
 * the sound driver's reads see back what it wrote. It does NOT implement the
 * FM (modulation) path, the DSP effect processor, or the LFOs. Those matter
 * for game music; the BIOS start-up sound and the CD player's UI clicks are
 * plain PCM, which is what this is built to get right first.
 *
 * Slot register layout (per slot, 0x20 bytes), from the SCSP manual:
 *   +0x00  KYONEX KYONB SBCTL SSCTL LPCTL PCM8B  SA[19:16]
 *   +0x02  SA[15:0]
 *   +0x04  LSA        loop start, in samples
 *   +0x06  LEA        loop end, in samples
 *   +0x08  D2R D1R EGHOLD AR
 *   +0x0A  LPSLNK KRS DL RR
 *   +0x0C  STWINH SDIR TL
 *   +0x10  OCT FNS    pitch
 *   +0x16  DISDL DIPAN   direct level and pan
 */
#include "saturn.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define SCSP_REGS   (0x1000u / 2u)     /* 16-bit register file */

static uint16_t rd(saturn *s, uint32_t off)
{
    return s->scsp_reg[(off >> 1) & (SCSP_REGS - 1u)];
}

uint16_t scsp_read(saturn *s, uint32_t off)
{
    /* Slot status register (Ymir SCSP::ReadSlotStatus). The WRITTEN value's
     * bits 15-11 select a slot (MSLC); a READ returns that slot's LIVE state:
     *   bits 4-0   EG level >> 5
     *   bits 6-5   EG state (attack 0, decay1 1, decay2 2, release 3)
     *   bits 10-7  CA -- current sample offset from SA in 4 Ki-sample units
     * Returning the stored register instead pinned CA at zero, and a movie
     * player that refills its streaming audio buffer by watching CA advance
     * waited forever: Sonic 3D's TrueMotion intro froze ~12 s in, exactly
     * when its preloaded audio ran out. */
    if ((off & ~1u) == 0x408u) {
        unsigned mslc = (rd(s, 0x408u) >> 11) & 0x1Fu;
        const scsp_slot *msl = &s->scsp_slot[mslc];
        uint16_t v = (uint16_t)((msl->env >> 5) & 0x1Fu);
        v |= (uint16_t)((unsigned)msl->phase << 5);
        v |= (uint16_t)(((msl->pos >> 12) & 0xFu) << 7);
        return v;
    }
    /* Slot 0x00 bit 12 (KYONEX) always reads back zero: it is a strobe. */
    uint16_t v = rd(s, off);
    if ((off & 0x1Fu) == 0 && off < 0x400u) v &= (uint16_t)~0x1000u;
    return v;
}

/* Key-on and key-off are edge-triggered by KYONEX, and they apply to EVERY
 * slot whose KYONB says so -- not just the slot that carries the strobe. That
 * is how a driver starts a stereo pair in the same sample. */
/* Ymir SCSP::AddOutput (scsp.cpp:1076). Send level and pan are LOGARITHMIC
 * attenuations, not linear scales, and the send level field is INVERTED:
 *
 *   if (sdl == 0) return;              -- zero means -infinity dB, not "x0"
 *   out <<= 14;                        -- headroom so both shifts stay exact
 *   out >>= sdl ^ 7;                   -- 6 dB per step
 *   pan &0xF == 0xF -> silence that side, else >> (pan>>1), 0.75x if odd
 *   pan bit 4 picks WHICH side gets attenuated
 *
 * We used `(sample * sdl) >> 3` and `(sample * (15 - amt)) >> 4`, which are
 * the wrong curve in both cases -- at SDL=1 hardware gives 1/64 where we gave
 * 1/8, so quiet voices were eight times too loud relative to loud ones. */
static void add_output(int32_t *out_l, int32_t *out_r,
                       int32_t sample, unsigned sdl, unsigned pan)
{
    int32_t pan_out;
    unsigned amt = pan & 0xFu;

    if (sdl == 0) return;
    sample <<= 14;
    sample >>= (int)(sdl ^ 7u);

    if (amt == 0xFu) {
        pan_out = 0;
    } else {
        pan_out = sample >> (amt >> 1);
        if (amt & 1u) pan_out -= pan_out >> 2;
    }

    if (pan & 0x10u) {
        *out_l += sample  >> 14;
        *out_r += pan_out >> 14;
    } else {
        *out_l += pan_out >> 14;
        *out_r += sample  >> 14;
    }
}

static void key_exec(saturn *s)
{
    int i;
    for (i = 0; i < 32; i++) {
        uint16_t c = s->scsp_reg[(i * 0x20u) >> 1];
        int want = (c & 0x0800u) != 0;              /* KYONB */
        scsp_slot *sl = &s->scsp_slot[i];
        /* Ymir Slot::TriggerKey:
         *
         *     trigger = (egState == Release) == keyOnBit
         *
         * Key ON fires ONLY from the release state, key OFF only from any
         * other state. The envelope state, not `active`, is what arms it --
         * and a one-shot that runs off the end of its sample clears `active`
         * while LEAVING the envelope where it was.
         *
         * Keying on whenever `!active` was the bug behind "sound effects play
         * once, then a second chopped copy": KYONEX is a GLOBAL strobe, so
         * every later sound the driver started re-fired every finished slot
         * whose KYONB was still set. The driver has to key the slot OFF first,
         * which is exactly what this test enforces. */
        if ((sl->phase == SCSP_ENV_RELEASE) != (want != 0)) continue;
        if (want) {
            uint32_t sa = ((uint32_t)(c & 0x000Fu) << 16) |
                          s->scsp_reg[((i * 0x20u) + 2u) >> 1];
            s->scsp_keyons++;
            /* SATURN_KEYLOG: one line per key-on with the same fields Ymir's
             * SCSP-KYONEX trace prints, so the two streams can be diffed slot
             * by slot. A sound effect that never fires shows up as a slot Ymir
             * keys on and we do not; a wrong loop range shows up here too. */
            if (getenv("SATURN_KEYLOG")) {
                uint16_t r4  = s->scsp_reg[((i * 0x20u) + 4u) >> 1];   /* LSA */
                uint16_t r6  = s->scsp_reg[((i * 0x20u) + 6u) >> 1];   /* LEA */
                uint16_t r8  = s->scsp_reg[((i * 0x20u) + 8u) >> 1];   /* D2R/D1R/AR */
                uint16_t rA  = s->scsp_reg[((i * 0x20u) + 0xAu) >> 1]; /* KRS/DL/RR */
                uint16_t r10 = s->scsp_reg[((i * 0x20u) + 0x10u) >> 1];/* OCT/FNS */
                uint16_t r16 = s->scsp_reg[((i * 0x20u) + 0x16u) >> 1];
                uint16_t r18 = s->scsp_reg[((i * 0x20u) + 0x16u) >> 1];
                printf("[keyon] slot %02d addr=%05X loop=%04X-%04X "
                       "OCT=%02u FNS=%u EGa=%04X EGb=%04X "
                       "DISDL=%u EFSDL=%u cy=%llu\n",
                       i, (unsigned)sa, r4, r6,
                       (unsigned)((r10 >> 11) & 0xF), (unsigned)(r10 & 0x7FF),
                       r8, rA,
                       (unsigned)((r16 >> 13) & 7u), (unsigned)((r18 >> 5) & 7u),
                       (unsigned long long)s->master.cycles);
            }
            sl->active  = 1;
            sl->sa      = sa;
            sl->pos     = 0;
            sl->frac    = 0;
            sl->env     = 0x280;                    /* Ymir TriggerKey: attenuation, not silence */
            sl->env_prev = 0x280;
            sl->phase   = SCSP_ENV_ATTACK;
            sl->reverse = 0;
            sl->crossed = 0;
        } else {
            sl->phase = SCSP_ENV_RELEASE;
        }
    }
}

/* Interrupt bits, from Ymir scsp_defs.hpp: DMA end 4, timer A 6 (B and C
 * follow), one-per-sample 10. The level for each is bit-sliced across
 * SCILV0/1/2 -- bit i of each register supplies one bit of interrupt i's
 * 3-bit level -- and the 68000 sees the highest level currently asserted. */
#define SCSP_INTR_TIMER_A  6
#define SCSP_INTR_SAMPLE   10

#define R_SCIEB  0x41Eu
#define R_SCIPD  0x420u
#define R_SCIRE  0x422u
#define R_SCILV0 0x424u
#define R_SCILV1 0x426u
#define R_SCILV2 0x428u

static void scsp_update_irq(saturn *s)
{
    uint16_t pend, en, act;
    int best = 0, i;

    if (s->sound_cpu.halted) return;
    /* Don't re-enter the interrupt path while the 68000 is mid-instruction.
     * A write to an SCSP register from inside the handler would call back
     * into scsp_update_irq -> m68k_set_irq, and if the handler faults the
     * exception pushes more stack frames which write more SCSP regs... the
     * C-level recursion blows the host stack (Windows exit 116). */
    if (s->sound_cpu.stepping) return;

    pend = s->scsp_reg[R_SCIPD >> 1];
    en   = s->scsp_reg[R_SCIEB >> 1];
    act  = (uint16_t)(pend & en);

    for (i = 0; i < 11; i++) {
        int lvl;
        if (!(act & (1u << i))) continue;
        lvl = ((s->scsp_reg[R_SCILV0 >> 1] >> i) & 1)
            | (((s->scsp_reg[R_SCILV1 >> 1] >> i) & 1) << 1)
            | (((s->scsp_reg[R_SCILV2 >> 1] >> i) & 1) << 2);
        if (lvl > best) best = lvl;
    }
    m68k_set_irq(&s->sound_cpu, best, -1);
}

void scsp_write(saturn *s, uint32_t off, uint16_t v)
{
    uint32_t idx = (off >> 1) & (SCSP_REGS - 1u);

    if (off == R_SCIRE) {
        /* Write-to-clear: a one clears the matching pending bit. */
        s->scsp_reg[R_SCIPD >> 1] &= (uint16_t)~v;
        s->scsp_reg[idx] = v;
        scsp_update_irq(s);
        return;
    }
    if (off == R_SCIPD) {
        /* Only the manual-request bit is software-settable. */
        s->scsp_reg[idx] = (uint16_t)((s->scsp_reg[idx] & ~0x20u) | (v & 0x20u));
        scsp_update_irq(s);
        return;
    }

    s->scsp_reg[idx] = v;

    /* SATURN_DSPPROBE: is the SCSP DSP actually used by this title? Ymir's
     * register map (scsp.hpp:400-415): COEF 0x700-0x77F, MADRS 0x780-0x7BF,
     * MPRO (the DSP program) 0x800-0xBFF. Porting the DSP is a large job and a
     * wrong one adds noise rather than silence, so measure demand first. */
    /* DSP register files, Ymir scsp.hpp:400-415. MPRO is four 16-bit words per
     * 64-bit instruction; COEF is 13-bit stored shifted left by 3; MADRS and
     * MIXS are plain. Writing MPRO also extends the program length so the DSP
     * knows how many steps are live. */
    /* MEM4MB/DAC18B/VER/MVOL live at 0x400; the ring buffer's lead address and
     * length are at 0x402 (RBP bits 6-0, RBL bits 8-7 in Ymir's decode). */
    if (off == 0x402u) {
        scsp_dsp_update_rbp(s, (uint16_t)(v & 0x7Fu));
        scsp_dsp_update_rbl(s, (uint16_t)((v >> 7) & 3u));
    }

    if (off >= 0x800u && off <= 0xBFFu) {
        unsigned widx = (off - 0x800u) >> 1;      /* 0..511 */
        unsigned instr = widx >> 2;
        unsigned part  = widx & 3u;               /* 0 = most significant */
        int sh = (int)(48u - part * 16u);
        s->dsp.program[instr] &= ~((uint64_t)0xFFFFu << sh);
        s->dsp.program[instr] |= (uint64_t)v << sh;
        if (v != 0 && instr + 1u > s->dsp.prog_len) s->dsp.prog_len = instr + 1u;
    } else if (off >= 0x700u && off <= 0x77Fu) {
        s->dsp.coef[(off - 0x700u) >> 1] = (uint16_t)(v >> 3);
    } else if (off >= 0x780u && off <= 0x7BFu) {
        s->dsp.madrs[(off - 0x780u) >> 1] = v;
    }

    if (getenv("SATURN_DSPPROBE")) {
        if (off >= 0x800u && off <= 0xBFFu) s->dsp_mpro_writes++;
        else if (off >= 0x700u && off <= 0x77Fu) s->dsp_coef_writes++;
        else if (off >= 0x780u && off <= 0x7BFu) s->dsp_madrs_writes++;
    }

    if (off >= 0x418u && off <= 0x41Cu && !(off & 1u))
        s->scsp_timer_reload[(off - 0x418u) >> 1] = 1;
    /* KYONEX is a LATCH, not an immediate action. Ymir sets m_kyonex here
     * (scsp.hpp:555) and consumes it once per sample step at slot 0
     * (scsp.cpp:1113: m_kyonexExecute = m_kyonex; m_kyonex = false).
     *
     * We used to call key_exec() straight from the write. The 68000 sound
     * driver writes this register file a BYTE at a time, and m68k_w8 does a
     * read-modify-write of the whole word -- so touching the low byte of a
     * slot's register 0 re-wrote a word that still had bit 12 set and fired
     * the strobe all over again. Every re-fire re-keys slots that are already
     * playing, which is what a repeating sound effect is. Measured against
     * Ymir on the same title: 22 key-ons here versus 2 there.
     *
     * Ymir masks the address with 0x1E, so BOTH bytes of the first word arm
     * the latch, and it is the audio step -- not the write -- that acts. */
    if (off < 0x400u && (off & 0x1Eu) == 0 && (v & 0x1000u))
        s->scsp_kyonex = 1;
    if (off == R_SCIEB || off == R_SCILV0 || off == R_SCILV1 || off == R_SCILV2)
        scsp_update_irq(s);
}

static void scsp_raise(saturn *s, int bit)
{
    s->scsp_reg[R_SCIPD >> 1] |= (uint16_t)(1u << bit);
    /* Only re-derive the 68000's IPL level if this source is actually
     * enabled. The one-per-sample interrupt fires 44,100 times a second and
     * almost no driver enables it, so walking the 11-entry level table every
     * time was pure overhead -- it cost more than the whole PCM mixer. */
    if (s->scsp_reg[R_SCIEB >> 1] & (uint16_t)(1u << bit))
        scsp_update_irq(s);
}

/* Timers A/B/C live at 0x418/0x41A/0x41C: low 8 bits are the RELOAD value and
 * bits 8-10 a power-of-two prescale in samples. The counter runs up and fires
 * at 0xFF (Ymir scsp_timer.hpp). A sound driver sequences off these, so with
 * no timers the driver runs but never plays a note -- which is exactly what
 * the BIOS driver did here before this existed. */
static void scsp_timers_tick(saturn *s)
{
    int i;
    s->scsp_sample_ctr++;
    for (i = 0; i < 3; i++) {
        uint16_t reg = s->scsp_reg[(0x418u + (uint32_t)i * 2u) >> 1];
        uint32_t pre = (reg >> 8) & 7u;
        uint32_t mask = (1u << pre) - 1u;
        if ((s->scsp_sample_ctr & mask) != 0) continue;
        if (s->scsp_timer_reload[i]) {
            s->scsp_timer[i] = (uint8_t)(reg & 0xFFu);
            s->scsp_timer_reload[i] = 0;
        } else {
            s->scsp_timer[i] = (uint8_t)(s->scsp_timer[i] + 1u);
        }
        if (s->scsp_timer[i] == 0xFF) scsp_raise(s, SCSP_INTR_TIMER_A + i);
    }
    scsp_raise(s, SCSP_INTR_SAMPLE);
}

/* Pitch. FNS bit 10 is stored inverted by the SCSP. Ymir keeps the decoded
 * value as `rawFNS ^ 0x400`; at OCT=0 that value is already the phase step in
 * our 10.10 representation. Treating bit 10 as an ordinary magnitude bit
 * made 0x400 (the minimum step) play at 2x and turned affected effects into
 * short, harsh peaks. OCT is a signed 4-bit power-of-two scale. */
static uint32_t slot_step(saturn *s, int i)
{
    uint16_t p = s->scsp_reg[((uint32_t)i * 0x20u + 0x10u) >> 1];
    int oct = (int)((p >> 11) & 0xFu);
    uint32_t step = (p & 0x7FFu) ^ 0x400u;
    if (oct & 8) step >>= (16 - oct);                /* negative octave */
    else         step <<= oct;
    return step;
}

/* Envelope generator, ported from Ymir (scsp_slot.hpp IncrementEG + the three
 * lookup tables it builds). This replaces a LINEAR AMPLITUDE approximation
 * that ramped `env` up from 0 by `rate_step(ar) * 16` -- the comment on it
 * admitted it was a guess, and the `* 16` had no counterpart in hardware.
 *
 * The real chip works in ATTENUATION: env is 0x000 (loudest) to 0x3FF
 * (silent), a key-on starts it at 0x280 and the attack drives it DOWN. Rates
 * are not a simple shift either: an effective rate is derived from the 5-bit
 * register rate plus key-rate-scaling and the octave, and that indexes a
 * counter shift, a counter mask and an 8-entry increment pattern, so the
 * envelope advances only on selected samples.
 *
 * Getting this wrong changes how quickly every voice reaches full volume,
 * which is exactly where short sound effects live. */
static const uint8_t eg_shift_tab[64] = {
    12,12,12,12, 11,11,11,11, 10,10,10,10,  9, 9, 9, 9,
     8, 8, 8, 8,  7, 7, 7, 7,  6, 6, 6, 6,  5, 5, 5, 5,
     4, 4, 4, 4,  3, 3, 3, 3,  2, 2, 2, 2,  1, 1, 1, 1,
     1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1
};

static const uint8_t eg_inc_tab[64][8] = {
    { 0, 0, 0, 0, 0, 0, 0, 0 },   /* 0x00 */
    { 0, 0, 0, 0, 0, 0, 0, 0 },   /* 0x01 */
    { 0, 1, 1, 1, 0, 1, 1, 1 },   /* 0x02 */
    { 0, 1, 1, 1, 1, 1, 1, 1 },   /* 0x03 */
    { 0, 1, 0, 1, 0, 1, 0, 1 },   /* 0x04 */
    { 0, 1, 0, 1, 1, 1, 0, 1 },   /* 0x05 */
    { 0, 1, 1, 1, 0, 1, 1, 1 },   /* 0x06 */
    { 0, 1, 1, 1, 1, 1, 1, 1 },   /* 0x07 */
    { 0, 1, 0, 1, 0, 1, 0, 1 },   /* 0x08 */
    { 0, 1, 0, 1, 1, 1, 0, 1 },   /* 0x09 */
    { 0, 1, 1, 1, 0, 1, 1, 1 },   /* 0x0A */
    { 0, 1, 1, 1, 1, 1, 1, 1 },   /* 0x0B */
    { 0, 1, 0, 1, 0, 1, 0, 1 },   /* 0x0C */
    { 0, 1, 0, 1, 1, 1, 0, 1 },   /* 0x0D */
    { 0, 1, 1, 1, 0, 1, 1, 1 },   /* 0x0E */
    { 0, 1, 1, 1, 1, 1, 1, 1 },   /* 0x0F */
    { 0, 1, 0, 1, 0, 1, 0, 1 },   /* 0x10 */
    { 0, 1, 0, 1, 1, 1, 0, 1 },   /* 0x11 */
    { 0, 1, 1, 1, 0, 1, 1, 1 },   /* 0x12 */
    { 0, 1, 1, 1, 1, 1, 1, 1 },   /* 0x13 */
    { 0, 1, 0, 1, 0, 1, 0, 1 },   /* 0x14 */
    { 0, 1, 0, 1, 1, 1, 0, 1 },   /* 0x15 */
    { 0, 1, 1, 1, 0, 1, 1, 1 },   /* 0x16 */
    { 0, 1, 1, 1, 1, 1, 1, 1 },   /* 0x17 */
    { 0, 1, 0, 1, 0, 1, 0, 1 },   /* 0x18 */
    { 0, 1, 0, 1, 1, 1, 0, 1 },   /* 0x19 */
    { 0, 1, 1, 1, 0, 1, 1, 1 },   /* 0x1A */
    { 0, 1, 1, 1, 1, 1, 1, 1 },   /* 0x1B */
    { 0, 1, 0, 1, 0, 1, 0, 1 },   /* 0x1C */
    { 0, 1, 0, 1, 1, 1, 0, 1 },   /* 0x1D */
    { 0, 1, 1, 1, 0, 1, 1, 1 },   /* 0x1E */
    { 0, 1, 1, 1, 1, 1, 1, 1 },   /* 0x1F */
    { 0, 1, 0, 1, 0, 1, 0, 1 },   /* 0x20 */
    { 0, 1, 0, 1, 1, 1, 0, 1 },   /* 0x21 */
    { 0, 1, 1, 1, 0, 1, 1, 1 },   /* 0x22 */
    { 0, 1, 1, 1, 1, 1, 1, 1 },   /* 0x23 */
    { 0, 1, 0, 1, 0, 1, 0, 1 },   /* 0x24 */
    { 0, 1, 0, 1, 1, 1, 0, 1 },   /* 0x25 */
    { 0, 1, 1, 1, 0, 1, 1, 1 },   /* 0x26 */
    { 0, 1, 1, 1, 1, 1, 1, 1 },   /* 0x27 */
    { 0, 1, 0, 1, 0, 1, 0, 1 },   /* 0x28 */
    { 0, 1, 0, 1, 1, 1, 0, 1 },   /* 0x29 */
    { 0, 1, 1, 1, 0, 1, 1, 1 },   /* 0x2A */
    { 0, 1, 1, 1, 1, 1, 1, 1 },   /* 0x2B */
    { 0, 1, 0, 1, 0, 1, 0, 1 },   /* 0x2C */
    { 0, 1, 0, 1, 1, 1, 0, 1 },   /* 0x2D */
    { 0, 1, 1, 1, 0, 1, 1, 1 },   /* 0x2E */
    { 0, 1, 1, 1, 1, 1, 1, 1 },   /* 0x2F */
    { 1, 1, 1, 1, 1, 1, 1, 1 },   /* 0x30 */
    { 1, 1, 1, 2, 1, 1, 1, 2 },   /* 0x31 */
    { 2, 1, 2, 1, 2, 1, 2, 1 },   /* 0x32 */
    { 1, 2, 2, 2, 1, 2, 2, 2 },   /* 0x33 */
    { 2, 2, 2, 2, 2, 2, 2, 2 },   /* 0x34 */
    { 2, 2, 2, 4, 2, 2, 2, 4 },   /* 0x35 */
    { 4, 2, 4, 2, 4, 2, 4, 2 },   /* 0x36 */
    { 2, 4, 4, 4, 2, 4, 4, 4 },   /* 0x37 */
    { 4, 4, 4, 4, 4, 4, 4, 4 },   /* 0x38 */
    { 4, 4, 4, 8, 4, 4, 4, 8 },   /* 0x39 */
    { 8, 4, 8, 4, 8, 4, 8, 4 },   /* 0x3A */
    { 4, 8, 8, 8, 4, 8, 8, 8 },   /* 0x3B */
    { 8, 8, 8, 8, 8, 8, 8, 8 },   /* 0x3C */
    { 8, 8, 8, 8, 8, 8, 8, 8 },   /* 0x3D */
    { 8, 8, 8, 8, 8, 8, 8, 8 },   /* 0x3E */
    { 8, 8, 8, 8, 8, 8, 8, 8 },   /* 0x3F */
};

/* Ymir kEffectiveRateTable: rate + key-rate-scaling/octave correction, then
 * doubled and clamped to 6 bits. */
static unsigned eg_eff_rate(unsigned rate, int oct, unsigned krs)
{
    unsigned er = rate & 0x1Fu;
    if (krs < 0xFu) {
        int adj = (int)krs + ((oct ^ 8) - 8);
        if (adj < 0) adj = 0;
        if (adj > 0xF) adj = 0xF;
        er += (unsigned)adj;
    }
    er <<= 1;
    return er > 0x3Fu ? 0x3Fu : er;
}

/* The per-sample increment for a rate: zero unless the counter's low bits
 * clear the mask, then one of eight pattern entries. */
static unsigned eg_inc(unsigned effrate, uint64_t ctr)
{
    unsigned shift = eg_shift_tab[effrate];
    uint32_t mask  = (1u << shift) - 1u;
    if ((uint32_t)(ctr & mask) != 0) return 0;
    return eg_inc_tab[effrate][(ctr >> shift) & 7u];
}

static void env_tick(saturn *s, int i, scsp_slot *sl)
{
    uint16_t e1 = s->scsp_reg[((uint32_t)i * 0x20u + 0x08u) >> 1];
    uint16_t e2 = s->scsp_reg[((uint32_t)i * 0x20u + 0x0Au) >> 1];
    unsigned ar = e1 & 0x1Fu;
    unsigned d1r = (e1 >> 6) & 0x1Fu;
    unsigned d2r = (e1 >> 11) & 0x1Fu;
    unsigned rr = e2 & 0x1Fu;
    unsigned dl = (e2 >> 5) & 0x1Fu;

    /* Ymir IncrementEG. `curr` is sampled BEFORE the update because both the
     * attack formula and the deactivation test use the previous level. */
    {
        uint16_t p10 = s->scsp_reg[((uint32_t)i * 0x20u + 0x10u) >> 1];
        int oct = (int)((p10 >> 11) & 0xFu);
        unsigned krs = (e2 >> 10) & 0xFu;
        unsigned rate;
        unsigned effrate, inc;
        uint32_t curr = sl->env;

        switch (sl->phase) {
        case SCSP_ENV_ATTACK:  rate = ar;  break;
        case SCSP_ENV_DECAY1:  rate = d1r; break;
        case SCSP_ENV_DECAY2:  rate = d2r; break;
        default:               rate = rr;  break;
        }
        effrate = eg_eff_rate(rate, oct, krs);
        inc = eg_inc(effrate, s->scsp_sample_ctr);

        switch (sl->phase) {
        case SCSP_ENV_ATTACK:
            /* egLevel += (~curr * inc) >> 4 -- a NEGATIVE addend, so the
             * attenuation falls toward 0 (full volume) geometrically. */
            if (inc > 0 && sl->env > 0 && rate > 0) {
                int32_t d = (int32_t)(~curr * inc) >> 4;
                int32_t nv = (int32_t)sl->env + d;
                sl->env = (uint32_t)(nv < 0 ? 0 : (nv > 0x3FF ? 0x3FF : nv));
            }
            if (curr == 0) {
                sl->phase = SCSP_ENV_DECAY1;
            }
            break;
        case SCSP_ENV_DECAY1:
            if ((sl->env >> 5) == dl) sl->phase = SCSP_ENV_DECAY2;
            /* fall through: decay1, decay2 and release all climb by `inc` */
        case SCSP_ENV_DECAY2:
        case SCSP_ENV_RELEASE:
            if (rate > 0) {
                uint32_t nv = sl->env + inc;
                sl->env = nv > 0x3FFu ? 0x3FFu : nv;
            }
            break;
        default: break;
        }
        sl->env_prev = curr;
    }

    /* A slot goes SILENT -> INACTIVE in ANY phase, not just release. Ymir
     * IncrementEG ends with
     *     if (prevLevel >= 0x3C0 && !egBypass) { active = false; ... }
     * where egLevel is ATTENUATION (0 loudest, 0x3FF silent). Ours is a gain
     * running the other way, so the same threshold is env <= 0x3F000.
     *
     * We only deactivated in RELEASE, so a slot whose decay ran to silence
     * stayed active forever -- and in a looping LPCTL mode its sample kept
     * replaying underneath. That is the "sound effect repeats" bug. It also
     * blocked retriggering: key-on only fires from the release state, so a
     * slot stuck in DECAY2 could never be started again.
     *
     * EGBYPASS (bit 15 of the register at slot+0x0A) holds the envelope open
     * deliberately; honour it. */
    {
        uint16_t e2b = s->scsp_reg[((uint32_t)i * 0x20u + 0x0Au) >> 1];
        int egbypass = (e2b >> 15) & 1;
        if (!egbypass && sl->env_prev >= 0x3C0u) {
            sl->active = 0;
            sl->reverse = 0;
            sl->crossed = 0;
        }
    }
}

/* One output sample pair, 16-bit signed. */
/* ------------------------------------------------------------------ CDDA
 * The CD block calls this with one raw 2352-byte audio sector. Ymir's
 * SCSP::ReceiveCDDA (scsp.cpp:242) returns how full the ring is in thirds so
 * the drive can pace itself; we return the same so cdb_tick can do likewise.
 *
 * Audio-track bytes are little-endian 16-bit stereo -- the one place on this
 * big-endian machine where that is true, because it is CD-DA data, not
 * something the SH-2 ever formatted. */
uint32_t scsp_cdda_push(saturn *s, const uint8_t *sector2352)
{
    uint32_t n, len;

    /* REFUSE the sector rather than lapping the read pointer. The ring is a
     * byte ring whose fill is measured as `(wp - rp) mod size`, so once the
     * writer passes the reader that measure WRAPS to a small number: the
     * overrun is invisible, ~100 ms of audio is destroyed, and the music is
     * heard to jump forwards. Ymir never hits this because ReceiveCDDA is
     * only ever called when the drive has decided there is room; dropping the
     * sector here is the same contract stated defensively. */
    len = (s->cdda_wp + CDDA_RING - s->cdda_rp) % CDDA_RING;
    if (len + 2352u >= CDDA_RING) {
        s->cdda_over++;
        return 3u;                  /* "full": tell the drive to hold off */
    }

    /* SATURN_CDDADUMP=path: the exact byte stream handed to the mixer. Diffing
     * it against the track file on disc separates "the drive delivered the
     * wrong sectors" from "the mixer consumed them at the wrong rate", which
     * no amount of listening to the output can. */
    {
        static FILE *dump; static int tried;
        if (!tried) {
            const char *p = getenv("SATURN_CDDADUMP");
            tried = 1;
            if (p) dump = fopen(p, "wb");
        }
        if (dump) fwrite(sector2352, 1, 2352, dump);
    }

    for (n = 0; n < 2352u; n++) {
        s->cdda[s->cdda_wp] = sector2352[n];
        s->cdda_wp = (s->cdda_wp + 1u) % CDDA_RING;
    }
    s->cdda_pushed++;
    len = (s->cdda_wp + CDDA_RING - s->cdda_rp) % CDDA_RING;
    /* Do not start draining until several sectors are in hand: starting on
     * the first one guarantees an underrun on the very next sample, which
     * clears cdda_ready again and produces a stutter instead of music. */
    if (len >= 2352u * 4u) s->cdda_ready = 1;
    return (len * 3u) / CDDA_RING;
}

static uint32_t mix_slot_peak[32];

void scsp_render(saturn *s, int16_t *left, int16_t *right)
{
    int32_t l = 0, r = 0;
    int32_t dsp_slot_out[32] = {0};
    int32_t dsp_prev_out[32];
    int16_t next_ext_l = 0, next_ext_r = 0;
    static int mixdbg = -1;
    static int32_t mix_pre_peak;
    static uint32_t mix_clip, mix_n;
    static int nodsp = -1;
    if (mixdbg < 0) mixdbg = getenv("SATURN_MIXDBG") != NULL;
    if (nodsp < 0) {
        /* The DSP is accurate enough to run by default. SATURN_NODSP remains
         * a diagnostic bypass for comparing the dry signal. */
        nodsp = getenv("SATURN_NODSP") != NULL;
    }
    scsp_timers_tick(s);

    /* CD audio first, so a track keeps playing even when no PCM slot is
     * keyed on -- which is the normal state during a CDDA music track or an
     * FMV whose audio comes off an audio track. */
    if (s->cdda_ready) {
        if (s->cdda_rp != s->cdda_wp) {
            next_ext_l = (int16_t)(s->cdda[s->cdda_rp] |
                         (s->cdda[(s->cdda_rp + 1u) % CDDA_RING] << 8));
            next_ext_r = (int16_t)(s->cdda[(s->cdda_rp + 2u) % CDDA_RING] |
                         (s->cdda[(s->cdda_rp + 3u) % CDDA_RING] << 8));
            s->cdda_rp = (s->cdda_rp + 4u) % CDDA_RING;
            s->cdda_drained++;
        } else {
            s->cdda_ready = 0;        /* underrun: rebuffer before resuming */
            s->cdda_under++;
        }
    }
    int i;
    uint16_t mvol_reg = s->scsp_reg[0x400u >> 1];
    uint32_t mvol = mvol_reg & 0xFu;

    for (i = 0; i < 32; i++) dsp_prev_out[i] = s->scsp_slot[i].output;

    /* The DSP is stepped INSIDE the slot loop below, four steps per slot, so
     * that a step can read the MIXS entry a slot wrote earlier in this same
     * sample -- which is what Ymir does (SCSP::SlotProcess interleaves
     * m_dsp.Step() between the slot pipeline stages).
     *
     * Running all 128 steps as a BLOCK up here, before any slot had written
     * MIXS, meant the program read the wrong generation of its own inputs:
     * the effect return came out at full scale and was heard as loud static
     * rather than reverb. That is why the DSP was left opt-in. */
    if (!nodsp && s->dsp.prog_len != 0) {
        /* stepping happens per slot; nothing to do here */
    } else {
        /* An empty program still completes one 128-step DSP cycle. Preserve
         * its externally visible counters without making 5.6 million empty
         * function calls per emulated second. */
        s->dsp.mixs_gen ^= 0x10u;
        s->dsp.mixs_null = 0xFFFFu;
        s->dsp.mdec_ct--;
    }

    /* Ymir consumes the KYONEX latch at slot 0 of each sample step and clears
     * it, so a burst of writes between two steps produces exactly ONE strobe. */
    if (s->scsp_kyonex) {
        s->scsp_kyonex = 0;
        key_exec(s);
    }

    for (i = 0; i < 32; i++) {
        scsp_slot *sl = &s->scsp_slot[i];
        uint16_t c, lev;
        uint32_t lsa, lea, tl, disdl, dipan;
        int lpctl, pcm8;
        int32_t sample;

        if (!sl->active) {
            sl->output = 0;
            continue;
        }

        c    = s->scsp_reg[((uint32_t)i * 0x20u) >> 1];
        lpctl = (c >> 5) & 3;
        pcm8  = (c & 0x0010u) != 0;
        lsa  = s->scsp_reg[((uint32_t)i * 0x20u + 0x04u) >> 1];
        lea  = s->scsp_reg[((uint32_t)i * 0x20u + 0x06u) >> 1];
        lev  = s->scsp_reg[((uint32_t)i * 0x20u + 0x0Cu) >> 1];
        tl   = lev & 0xFFu;
        disdl = (s->scsp_reg[((uint32_t)i * 0x20u + 0x16u) >> 1] >> 13) & 7u;
        dipan = (s->scsp_reg[((uint32_t)i * 0x20u + 0x16u) >> 1] >> 8) & 0x1Fu;

        /* Fetch the current sample out of sound RAM. While running backwards
         * the counter holds the complement of the index (Ymir:
         * `currSmp = reverse ? ~currSample : currSample`). */
        {
            uint32_t idx  = sl->reverse ? (uint16_t)~sl->pos : sl->pos;
            uint32_t addr = sl->sa + (pcm8 ? idx : idx * 2u);
            addr &= (SOUND_RAM_SZ - 1u);
            if (pcm8) sample = (int32_t)(int8_t)s->sound_ram[addr] << 8;
            else      sample = (int32_t)(int16_t)((s->sound_ram[addr] << 8) |
                                                   s->sound_ram[(addr + 1u) &
                                                   (SOUND_RAM_SZ - 1u)]);
        }

        /* Ymir SlotProcessStep5_4 / Step6_4: envelope and total level are the
         * SAME attenuation domain and are ADDED, then one logarithmic
         * conversion turns the result into a gain:
         *
         *   finalLevel = min(envLevel + (TL << 2), 0x3FF)
         *   out = (out * ((finalLevel & 0x3F) ^ 0x7F)) >> ((finalLevel >> 6) + 7)
         *
         * We used to apply a linear envelope gain and THEN a separate linear
         * (255 - TL) multiply, which is neither the right curve nor the right
         * combination. */
        {
            uint32_t final_lvl = sl->env + (tl << 2);
            if (final_lvl > 0x3FFu) final_lvl = 0x3FFu;
            sample = (sample * (int32_t)((final_lvl & 0x3Fu) ^ 0x7Fu))
                     >> ((final_lvl >> 6) + 7u);
        }
        /* SATURN_MIXDBG: once a second, name the slots that dominate the mix
         * with their programmed levels. "The output clips" cannot separate
         * "one slot is wrongly at full scale" from "eight correct slots sum
         * hot"; this can. */
        if (mixdbg) {
            int32_t a = sample < 0 ? -sample : sample;
            if ((uint32_t)a > mix_slot_peak[i]) mix_slot_peak[i] = (uint32_t)a;
        }
        /* Save the slot output for operation 7 of the seven-stage pipeline.
         * DSP stepping and MIXS writes happen after all slot waveforms have
         * been produced, in hardware slot order below. */
        dsp_slot_out[i] = sample;
        sl->output = sample;
        {
            uint16_t r14 = s->scsp_reg[((uint32_t)i * 0x20u + 0x14u) >> 1];
            if (r14) s->dsp_reg14_nonzero++;
        }

        add_output(&l, &r, sample, disdl, dipan);

        /* Advance the phase, then handle looping -- Ymir
         * Slot::IncrementSampleCounter, transcribed.
         *
         * Every mode plays SA..LSA forwards ONCE before the loop segment
         * engages; `crossed` latches that. Only then does LEA mean anything,
         * which is why the old `if (lea && pos >= lea)` was wrong twice over:
         * it armed the end test before LSA had been passed, and a slot with
         * LEA == 0 never ended at all and ran on into whatever sample came
         * next in sound RAM.
         *   0 off (stop at LEA)   1 forward   2 reverse   3 alternating */
        sl->frac += slot_step(s, i);
        while (sl->frac >= 1024u) {
            sl->frac -= 1024u;
            sl->pos++;
            if (!sl->crossed) {
                if ((uint16_t)(sl->pos + 1u) > lsa) {
                    /* DIAGNOSTIC: a loop taken while the envelope is already
                     * at (or past) silence is the `sound effect repeats with a
                     * chopped second copy'' signature -- the slot should have
                     * been deactivated before reaching the loop point. */
                    if (sl->env >= 0x3C0u) s->scsp_silent_loops++;   /* attenuation: high = silent */
                    sl->crossed = 1;
                    if (lpctl == 2) {           /* reverse: jump to LEA */
                        sl->pos = (uint16_t)(sl->pos - (lsa + lea));
                        sl->reverse = 1;
                    }
                }
            } else {
                uint16_t nxt = (uint16_t)((sl->reverse ? (uint16_t)~sl->pos
                                                       : sl->pos) + 1u);
                uint16_t pt = (sl->reverse && (lpctl == 2 || lpctl == 3))
                            ? (uint16_t)lsa : (uint16_t)lea;
                int hit = nxt > pt;
                if ((sl->reverse != 0) != hit) {
                    switch (lpctl) {
                    case 0:
                        sl->active = 0; sl->reverse = 0; sl->crossed = 0;
                        break;
                    case 1:
                        sl->pos = (uint16_t)(sl->pos + (sl->reverse
                                  ? (uint16_t)(lea - lsa)
                                  : (uint16_t)(lsa - lea)));
                        break;
                    case 2:
                        sl->pos = (uint16_t)(sl->pos + (uint16_t)(lsa - lea));
                        break;
                    default:
                        sl->reverse ^= 1;
                        sl->pos = sl->reverse
                                ? (uint16_t)(sl->pos - (uint16_t)(lea * 2u))
                                : (uint16_t)(sl->pos + (uint16_t)(lsa * 2u));
                        break;
                    }
                }
                if (!sl->active) break;
            }
        }
        env_tick(s, i, sl);
    }

    if (!nodsp && s->dsp.prog_len != 0) {
        /* Ymir ProcessSlots(i): operation 7 belongs to slot i-6.  The DSP
         * starts at PC 0x68, advances once, receives that slot's MIXS value,
         * advances three more times, then exposes the slot's EFREG return.
         *
         * The old loop sent slot i BEFORE four DSP steps.  It therefore fed
         * slot 0 to program step 104 instead of step 0 and sampled every
         * effect return 24 instructions out of phase -- the cause of the
         * full-scale static that forced DSP off by default. */
        int pipe;
        for (pipe = 0; pipe < 32; pipe++) {
            unsigned slot = ((unsigned)pipe - 6u) & 31u;
            uint16_t r14 = s->scsp_reg[(slot * 0x20u + 0x14u) >> 1];
            unsigned isel = (r14 >> 3) & 0xFu;
            unsigned imxl = r14 & 0x7u;

            scsp_dsp_step(s);
            if (imxl) {
                s->dsp_sends++;
                scsp_dsp_mixs_write(s, isel,
                                    (((pipe < 6) ? dsp_prev_out[slot]
                                                 : dsp_slot_out[slot]) * 16)
                                    >> (imxl ^ 7u));
            } else {
                scsp_dsp_mixs_write(s, isel, 0);
            }
            scsp_dsp_step(s);
            scsp_dsp_step(s);
            scsp_dsp_step(s);

            if (slot < 16u) {
                uint16_t emix = s->scsp_reg[(slot * 0x20u + 0x16u) >> 1];
                unsigned efsdl = (emix >> 5) & 7u;
                if (efsdl > s->dsp_efsdl_max) s->dsp_efsdl_max = efsdl;
                if (efsdl) s->dsp_efret_slots |= 1u << slot;
                add_output(&l, &r, (int32_t)s->dsp.efreg[slot],
                           efsdl, emix & 0x1Fu);
            }
        }
    }

    if (getenv("SATURN_DSPPROBE")) {
        int q;
        for (q = 0; q < 16; q++) {
            int32_t a = s->dsp.efreg[q] < 0 ? -s->dsp.efreg[q] : s->dsp.efreg[q];
            if (a > s->dsp_efreg_peak) s->dsp_efreg_peak = a;
        }
        for (q = 0; q < 32; q++) {
            int32_t a = s->dsp.mixs[q] < 0 ? -s->dsp.mixs[q] : s->dsp.mixs[q];
            if (a > s->dsp_mixs_peak) s->dsp_mixs_peak = a;
        }
    }

    /* Ymir accumulates EFREG 0-15 and EXTS 0-1 according to the register
     * fields in slots 0-17, independently of PCM slot activity. CDDA is EXTS,
     * not an unattenuated signal added directly to the final accumulator. */
    /* EFREG is mixed per slot inside the loop above. */
    for (i = 16; i < 18; i++) {
        uint16_t mix = s->scsp_reg[((uint32_t)i * 0x20u + 0x16u) >> 1];
        add_output(&l, &r, (int32_t)s->dsp.exts[i & 1],
                   (mix >> 5) & 7u, mix & 0x1Fu);
    }
    /* Ymir copies the newly drained CDDA frame into EXTS after finishing the
     * current sample, so the external input has exactly a one-sample delay. */
    s->dsp.exts[0] = next_ext_l;
    s->dsp.exts[1] = next_ext_r;

    /* Master volume is 4-bit attenuation, 15 = full. */
    /* Master volume. Ymir (scsp.cpp:1008): the field is INVERTED and applied
     * as a logarithmic attenuation in 3 dB steps -- half a bit per step, with
     * an extra 0.75x for the odd half-step -- not as a linear scale.
     *
     *     mv = MVOL ^ 0xF;  out = (out << 8) >> (mv >> 1);
     *     if (mv & 1) out -= out >> 2;   out >>= 8;
     *
     * We multiplied by MVOL/16 instead, which at mid volume (MVOL=8) is 0.5x
     * where hardware gives about 0.094x -- over five times too loud, and the
     * reason the mix sat on the clipping rails. */
    {
        int32_t mv = (int32_t)(mvol ^ 0xFu);
        if (mvol == 0) {
            l = r = 0;
        } else {
            l <<= 8; l >>= (mv >> 1); if (mv & 1) l -= l >> 2; l >>= 8;
            r <<= 8; r >>= (mv >> 1); if (mv & 1) r -= r >> 2; r >>= 8;
        }
    }
    if (mixdbg) {
        int32_t al = l < 0 ? -l : l, ar3 = r < 0 ? -r : r;
        int32_t pk = al > ar3 ? al : ar3;
        if (pk > mix_pre_peak) mix_pre_peak = pk;
        if (l > 32767 || l < -32768 || r > 32767 || r < -32768) mix_clip++;
        if (++mix_n >= 44100u) {
            int q;
            printf("[mix] cy=%llu preclip_peak=%d clip=%u/44100 MVOL=%X",
                   (unsigned long long)s->master.cycles, mix_pre_peak,
                   mix_clip, s->scsp_reg[0x400u >> 1] & 0xFu);
            for (q = 0; q < 32; q++) {
                if (mix_slot_peak[q] < 6000u) continue;
                {
                    uint16_t rC  = s->scsp_reg[((uint32_t)q * 0x20u + 0x0Cu) >> 1];
                    uint16_t r16 = s->scsp_reg[((uint32_t)q * 0x20u + 0x16u) >> 1];
                    printf(" | s%02d pk=%u env=%03X TL=%02X DISDL=%u PAN=%02X",
                           q, mix_slot_peak[q], s->scsp_slot[q].env,
                           rC & 0xFFu, (r16 >> 13) & 7u, (r16 >> 8) & 0x1Fu);
                }
            }
            printf("\n");
            mix_pre_peak = 0; mix_clip = 0; mix_n = 0;
            memset(mix_slot_peak, 0, sizeof(mix_slot_peak));
        }
    }
    if (l >  32767) l =  32767;
    if (l < -32768) l = -32768;
    if (r >  32767) r =  32767;
    if (r < -32768) r = -32768;
    *left  = (int16_t)l;
    *right = (int16_t)r;

    /* Output level telemetry. A silent mixer and a working one are otherwise
     * indistinguishable in a headless run, which is how an envelope change
     * could pass every test and still produce nothing audible. */
    {
        int32_t al = l < 0 ? -l : l, ar2 = r < 0 ? -r : r;
        uint32_t pk = (uint32_t)(al > ar2 ? al : ar2);
        if (pk > s->scsp_peak) s->scsp_peak = pk;
        if (pk) s->scsp_nonsilent++;
        /* Peak and non-silent saturate almost immediately, so neither can show
         * whether an ADDITIVE path (like the DSP effect return) contributes.
         * Total energy can. */
        s->scsp_energy += (uint64_t)al + (uint64_t)ar2;
    }
}

void scsp_reset(saturn *s)
{
    scsp_dsp_reset(s);
    int i;
    memset(s->scsp_reg, 0, sizeof(s->scsp_reg));
    memset(s->scsp_slot, 0, sizeof(s->scsp_slot));
    /* Slots come up in RELEASE, not ATTACK (Ymir Slot::Reset). With the
     * Ymir key rule -- key-on only fires from release -- a slot left in the
     * zeroed ATTACK state could never be started at all. */
    for (i = 0; i < 32; i++) s->scsp_slot[i].phase = SCSP_ENV_RELEASE;
    memset(s->scsp_timer, 0, sizeof(s->scsp_timer));
    memset(s->scsp_timer_reload, 0, sizeof(s->scsp_timer_reload));
    s->scsp_sample_ctr = 0;
    /* Master volume comes up at full: a driver that never writes MVOL still
     * gets heard, which is what the BIOS relies on. */
    s->scsp_reg[0x400u >> 1] = 0x000Fu;
}
