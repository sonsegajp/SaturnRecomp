/* m68k_core.c -- MC68000 semantics, against the 68000 programmer's manual.
 *
 * The sound CPU is the one processor in this machine whose output you cannot
 * see. A wrong flag in the SH-2 shows up as a corrupted screen within seconds;
 * a wrong flag here shows up as silence, or as a driver that wanders off into
 * sound RAM, with nothing on screen to say so. So the arithmetic and the flags
 * get checked here, per-instruction, before any of it is trusted.
 *
 * Encodings are assembled by hand from the manual's tables.
 */
#include "saturn.h"
#include "m68k.h"
#include <stdio.h>
#include <string.h>

static saturn S;
static int fails;

static void ck(const char *what, uint32_t got, uint32_t want)
{
    if (got != want) {
        printf("  FAIL %-46s got %08X want %08X\n", what, got, want);
        fails++;
    }
}

static void w16(uint32_t a, uint16_t v)
{
    S.sound_ram[a & (SOUND_RAM_SZ - 1)]       = (uint8_t)(v >> 8);
    S.sound_ram[(a + 1) & (SOUND_RAM_SZ - 1)] = (uint8_t)v;
}

static void w32(uint32_t a, uint32_t v) { w16(a, (uint16_t)(v >> 16)); w16(a + 2, (uint16_t)v); }

/* Assemble a program at 0x1000 and run it for one instruction at a time. */
static m68k *boot(const uint16_t *prog, int n)
{
    int i;
    memset(&S, 0, sizeof S);
    w32(0, 0x00010000u);          /* initial SSP */
    w32(4, 0x00001000u);          /* initial PC  */
    for (i = 0; i < n; i++) w16(0x1000u + (uint32_t)i * 2u, prog[i]);
    m68k_reset(&S.sound_cpu, &S);
    return &S.sound_cpu;
}

static void step_n(m68k *m, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        uint64_t before = m->cycles;
        while (m->cycles == before) m68k_run(m, 1);
    }
}

int main(void)
{
    printf("MC68000 semantics\n");

    /* ---- 1. reset loads SSP and PC from the vector table --------------- */
    {
        static const uint16_t p[] = { 0x4E71 };          /* NOP */
        m68k *m = boot(p, 1);
        ck("reset SSP", m->a[7], 0x00010000u);
        ck("reset PC",  m->pc,   0x00001000u);
        ck("reset supervisor", (m->sr & M68K_SR_S) != 0, 1);
    }

    /* ---- 2. MOVEQ sign-extends and sets N ------------------------------ */
    {
        static const uint16_t p[] = { 0x70FF };          /* moveq #-1,d0 */
        m68k *m = boot(p, 1);
        step_n(m, 1);
        ck("moveq #-1 value", m->d[0], 0xFFFFFFFFu);
        ck("moveq sets N", (m->sr & M68K_SR_N) != 0, 1);
        ck("moveq clears Z", (m->sr & M68K_SR_Z) != 0, 0);
        ck("moveq clears V/C", (m->sr & (M68K_SR_V|M68K_SR_C)), 0);
    }

    /* ---- 3. ADD.B carry and overflow are independent ------------------- */
    {
        /* moveq #127,d0 ; moveq #1,d1 ; add.b d1,d0 */
        static const uint16_t p[] = { 0x707F, 0x7201, 0xD001 };
        m68k *m = boot(p, 3);
        step_n(m, 3);
        ck("add.b 127+1 = 0x80", m->d[0] & 0xFF, 0x80u);
        ck("add.b sets V (signed overflow)", (m->sr & M68K_SR_V) != 0, 1);
        ck("add.b clears C (no unsigned carry)", (m->sr & M68K_SR_C) != 0, 0);
        ck("add.b sets N", (m->sr & M68K_SR_N) != 0, 1);
    }
    {
        /* moveq #-1,d0 ; moveq #1,d1 ; add.b d1,d0  -> 0xFF + 1 = 0x00 */
        static const uint16_t p[] = { 0x70FF, 0x7201, 0xD001 };
        m68k *m = boot(p, 3);
        step_n(m, 3);
        ck("add.b 0xFF+1 = 0", m->d[0] & 0xFF, 0x00u);
        ck("add.b sets C", (m->sr & M68K_SR_C) != 0, 1);
        ck("add.b sets X", (m->sr & M68K_SR_X) != 0, 1);
        ck("add.b sets Z", (m->sr & M68K_SR_Z) != 0, 1);
        ck("add.b clears V", (m->sr & M68K_SR_V) != 0, 0);
        /* The upper bytes of D0 must be untouched by a byte operation. */
        ck("add.b preserves upper bytes", m->d[0] >> 8, 0xFFFFFFu);
    }

    /* ---- 4. SUB sets C as a BORROW ------------------------------------ */
    {
        /* moveq #1,d0 ; moveq #2,d1 ; sub.l d1,d0   -> 1 - 2 */
        static const uint16_t p[] = { 0x7001, 0x7202, 0x9081 };
        m68k *m = boot(p, 3);
        step_n(m, 3);
        ck("sub.l 1-2", m->d[0], 0xFFFFFFFFu);
        ck("sub.l sets C (borrow)", (m->sr & M68K_SR_C) != 0, 1);
        ck("sub.l sets N", (m->sr & M68K_SR_N) != 0, 1);
        ck("sub.l clears V", (m->sr & M68K_SR_V) != 0, 0);
    }

    /* ---- 5. CMP must NOT disturb X ------------------------------------ */
    {
        /* moveq #-1,d0; moveq #1,d1; add.b d1,d0 (sets X); moveq #5,d2;
           moveq #9,d3; cmp.l d3,d2 */
        static const uint16_t p[] = { 0x70FF, 0x7201, 0xD001, 0x7405, 0x7609, 0xB483 };
        m68k *m = boot(p, 6);
        step_n(m, 6);
        ck("cmp leaves X set", (m->sr & M68K_SR_X) != 0, 1);
        ck("cmp 5-9 sets N", (m->sr & M68K_SR_N) != 0, 1);
        ck("cmp 5-9 sets C", (m->sr & M68K_SR_C) != 0, 1);
        ck("cmp does not write d2", m->d[2], 5u);
    }

    /* ---- 6. ADDX chains the X flag and ANDs Z -------------------------- */
    {
        /* moveq #0,d0 ; moveq #0,d1 ; (X clear, Z set by moveq #0)
           addx.l d1,d0  -> 0 + 0 + 0, Z must stay set */
        static const uint16_t p[] = { 0x7000, 0x7200, 0xD181 };
        m68k *m = boot(p, 3);
        step_n(m, 3);
        ck("addx 0+0 keeps Z", (m->sr & M68K_SR_Z) != 0, 1);
        ck("addx result", m->d[0], 0u);
    }

    /* ---- 7. LSR/ASR differ on the sign bit ----------------------------- */
    {
        /* moveq #-16,d0 ; lsr.l #1,d0 */
        static const uint16_t p[] = { 0x70F0, 0xE288 };
        m68k *m = boot(p, 2);
        step_n(m, 2);
        ck("lsr.l shifts in zero", m->d[0], 0x7FFFFFF8u);
    }
    {
        /* moveq #-16,d0 ; asr.l #1,d0 */
        static const uint16_t p[] = { 0x70F0, 0xE280 };
        m68k *m = boot(p, 2);
        step_n(m, 2);
        ck("asr.l preserves sign", m->d[0], 0xFFFFFFF8u);
        ck("asr.l sets N", (m->sr & M68K_SR_N) != 0, 1);
    }

    /* ---- 8. addressing modes: (An)+ and -(An) ------------------------- */
    {
        /* movea.l #0x2000,a0 ; move.l #0x11223344,(a0)+ ; move.l -(a0),d0 */
        static const uint16_t p[] = {
            0x207C, 0x0000, 0x2000,          /* movea.l #$2000,a0 */
            0x20FC, 0x1122, 0x3344,          /* move.l #$11223344,(a0)+ */
            0x2020                            /* move.l -(a0),d0 */
        };
        m68k *m = boot(p, 7);
        step_n(m, 3);
        ck("(An)+ stored value", m->d[0], 0x11223344u);
        ck("-(An) restored A0", m->a[0], 0x2000u);
    }

    /* ---- 9. A7 keeps word alignment on byte access -------------------- */
    {
        /* move.b #1,-(a7) : A7 must drop by 2, not 1 */
        static const uint16_t p[] = { 0x1EFC, 0x0001 };   /* move.b #1,(a7)+ */
        m68k *m = boot(p, 2);
        {
            uint32_t before = m->a[7];
            step_n(m, 1);
            ck("A7 byte access steps by 2", m->a[7] - before, 2u);
        }
    }

    /* ---- 10. DBcc counts through -1 ----------------------------------- */
    {
        /* moveq #2,d0 ; dbra d0,-2  (loops back onto itself) */
        static const uint16_t p[] = { 0x7002, 0x51C8, 0xFFFE };
        m68k *m = boot(p, 3);
        step_n(m, 1);
        step_n(m, 3);                       /* three DBRA executions */
        ck("dbra counted down", m->d[0] & 0xFFFF, 0xFFFFu);
    }

    /* ---- 11. MOVEM round-trips registers ------------------------------ */
    {
        /* moveq #0x11,d0 ; moveq #0x22,d1 ; movea.l #0x3000,a0 ;
           movem.l d0-d1,(a0) ; moveq #0,d0 ; moveq #0,d1 ;
           movem.l (a0),d0-d1 */
        static const uint16_t p[] = {
            0x7011, 0x7222,
            0x207C, 0x0000, 0x3000,
            0x4890, 0x0003,
            0x7000, 0x7200,
            0x4C90, 0x0003
        };
        m68k *m = boot(p, 11);
        step_n(m, 7);
        ck("movem restored d0", m->d[0], 0x11u);
        ck("movem restored d1", m->d[1], 0x22u);
    }

    /* ---- 12. exceptions push PC and SR, RTE restores ------------------- */
    {
        /* TRAP #0 -> vector 32 at 0x80, pointing at an RTE. */
        static const uint16_t p[] = { 0x4E40, 0x4E71 };   /* trap #0 ; nop */
        m68k *m = boot(p, 2);
        w32(32u * 4u, 0x00002000u);
        w16(0x2000u, 0x4E73);                            /* rte */
        step_n(m, 1);
        ck("trap vectored", m->pc, 0x00002000u);
        step_n(m, 1);
        ck("rte returned", m->pc, 0x00001002u);
    }

    /* ---- 13. an interrupt is taken and masked afterwards -------------- */
    {
        static const uint16_t p[] = { 0x4E71, 0x4E71, 0x4E71 };
        m68k *m = boot(p, 3);
        w32(26u * 4u, 0x00003000u);            /* autovector for level 2 */
        m->sr = (uint16_t)(m->sr & ~M68K_SR_I); /* unmask */
        m68k_set_irq(m, 2, -1);
        step_n(m, 1);
        ck("irq vectored", m->pc, 0x00003000u);
        ck("irq raised the mask", (m->sr & M68K_SR_I) >> 8, 2u);
    }

    /* ---- 14. SCSP key-on starts a slot, key-off releases it ------------ */
    {
        memset(&S, 0, sizeof S);
        scsp_reset(&S);
        /* Slot 0: KYONB | KYONEX, start address 0x100. */
        scsp_write(&S, 0x02u, 0x0100u);
        scsp_write(&S, 0x00u, 0x1800u);
        /* KYONEX is a LATCH: Ymir arms m_kyonex on the write and consumes it
         * at slot 0 of the NEXT sample step (scsp.cpp:1113), so the key-on has
         * not happened yet at this point. This test used to check immediately,
         * which encoded our old execute-on-write behaviour. Render one sample
         * to let the strobe fire. */
        {
            int16_t sl_, sr_;
            ck("key-on is still pending before a sample step",
               (uint32_t)S.scsp_slot[0].active, 0u);
            scsp_render(&S, &sl_, &sr_);
        }
        ck("key-on activated slot 0", (uint32_t)S.scsp_slot[0].active, 1u);
        ck("key-on latched start address", S.scsp_slot[0].sa, 0x100u);
        ck("KYONEX reads back clear", scsp_read(&S, 0) & 0x1000u, 0u);
        scsp_write(&S, 0x00u, 0x1000u);        /* KYONEX with KYONB clear */
        { int16_t sl_, sr_; scsp_render(&S, &sl_, &sr_); }  /* consume strobe */
        ck("key-off entered release", (uint32_t)S.scsp_slot[0].phase,
           (uint32_t)SCSP_ENV_RELEASE);
    }

    /* ---- 15. the SCSP renders silence when nothing is keyed on -------- */
    {
        int16_t l = 1, r = 1;
        memset(&S, 0, sizeof S);
        scsp_reset(&S);
        scsp_render(&S, &l, &r);
        ck("idle SCSP is silent (L)", (uint32_t)(uint16_t)l, 0u);
        ck("idle SCSP is silent (R)", (uint32_t)(uint16_t)r, 0u);
    }

    /* ---- 16. FNS bit 10 is encoded, not a magnitude bit --------------- */
    {
        int i;
        int16_t l, r;
        memset(&S, 0, sizeof S);
        scsp_reset(&S);
        scsp_write(&S, 0x06u, 0xFFFFu);       /* keep the test slot alive */
        scsp_write(&S, 0x10u, 0x0400u);       /* decoded FNS = 0 */
        scsp_write(&S, 0x00u, 0x1800u);       /* KYONB | KYONEX */
        for (i = 0; i < 8; i++) scsp_render(&S, &l, &r);
        ck("FNS 0x400 decodes to zero phase step", S.scsp_slot[0].pos, 0u);

        scsp_write(&S, 0x10u, 0x0000u);       /* decoded FNS = 0x400: 1x */
        for (i = 0; i < 8; i++) scsp_render(&S, &l, &r);
        ck("FNS 0x000 advances at unity pitch", S.scsp_slot[0].pos, 8u);
    }

    /* ---- 17. scheduler preserves fractional and overshoot clocks ------ */
    {
        int i;
        memset(&S, 0, sizeof S);
        w32(0, 0x00010000u);
        w32(4, 0x00001000u);
        w16(0x1000u, 0x60FEu);                  /* bra.s $1000 */
        sound_init(&S);
        sound_set_on(&S, 1);
        for (i = 0; i < 1000; i++) sound_run(&S, 128u);
        ck("68000 cumulative target keeps fractional clocks",
           (uint32_t)S.m68k_target, 50462u);
        ck("68000 instruction overshoot is carried forward",
           (uint32_t)(S.sound_cpu.cycles - S.m68k_target) < 10u, 1u);
    }

    if (fails) { printf("FAILED: %d check(s)\n", fails); return 1; }
    printf("PASS: MC68000 + SCSP checks\n");
    return 0;
}
