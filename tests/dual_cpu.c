/* dual_cpu.c -- master/slave rendezvous, per-core on-chip banks, and the FRT.
 *
 * The blocker this pins down was diagnosed for hours against a live NiGHTS
 * boot, where you cannot tell a scheduler fault from a game-logic fault. The
 * shape of that blocker is small enough to build by hand:
 *
 *   master:  set a flag in shared RAM, then spin until the slave clears it
 *   slave:   poll FTCSR ICF (the H-Blank input capture), then clear the flag
 *
 * That is exactly NiGHTS -- master parked at 0x06032CCE, slave polling
 * 0xFFFFFE11 bit 0x80 at 0x06005F9E -- with the game taken out of it. Under
 * the old half-frame scheduler this cannot close: whichever core is running
 * runs to the end of its slice against a frozen view of the other, and ICF was
 * derived from the MASTER's cycle count, which does not advance at all while
 * the slave is the core executing.
 *
 * Encodings below are from the SH-2 manual; MOV.L @(disp,PC) resolves against
 * (PC & ~3) + 4 + disp*4, branches against PC + 4 + disp*2.
 */
#include "saturn.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static saturn S;
static int fails;

static void ck(const char *what, uint32_t got, uint32_t want)
{
    if (got != want) {
        printf("  FAIL %-42s got %08X want %08X\n", what, got, want);
        fails++;
    }
}

static void w16(uint32_t a, uint16_t v) { bus_w16(&S, a, v); }
static void w32(uint32_t a, uint32_t v) { bus_w32(&S, a, v); }

#define M_BASE   0x06040000u       /* master program        */
#define S_BASE   0x06050000u       /* slave program         */
#define FLAG     0x06041000u       /* the handshake flag    */
#define M_DONE   0x06041004u       /* master reached the far side */
#define S_DONE   0x06041008u       /* slave reached the far side  */

/* master:
 *   000  mov.l @(0x0F,pc),r1     ; r1 = &FLAG
 *   002  mov   #1,r0
 *   004  mov.l r0,@r1            ; hand the slave its work
 *   006  mov.l @r1,r0            ; loop:  <-- the park
 *   008  tst   r0,r0
 *   00A  bf    loop              ; spin while the flag is still set
 *   00C  mov.l @(0x0D,pc),r2     ; r2 = &M_DONE
 *   00E  mov   #0x5A,r0
 *   010  mov.l r0,@r2
 *   012  bra   .                 ; park for good
 *   014  nop
 *   040  .long FLAG
 *   044  .long M_DONE
 */
static void build_master(void)
{
    /* The FTI pins are wired to the MINIT/SINIT slots, so the master must
     * PULSE the slave's capture by writing to the MINIT area, 0x01000000.
     * The slot is named for who RINGS it, not who hears it: MINIT is the
     * master's doorbell and lands on the SLAVE's FRT; SINIT is the slave's
     * and lands on the MASTER's. Ymir derives this from SH7604 BCR1.MASTER
     * (sh2.cpp:399 maps 0x1000000 + IsMaster()*0x800000 to each core's own
     * TriggerFRTInputCapture) and states it in its SH-2 memory map notes.
     * We had it inverted until Sonic 3D Blast's SGL slSynch caught it. The
     * old test relied on H-Blank raising ICF, which real hardware does not
     * do. */
    w16(M_BASE + 0x00, 0xD10F);          /* mov.l @(15,pc),r1 ; r1 = &FLAG  */
    w16(M_BASE + 0x02, 0xE001);          /* mov #1,r0                       */
    w16(M_BASE + 0x04, 0x2102);          /* mov.l r0,@r1      ; FLAG = 1    */
    w16(M_BASE + 0x06, 0xD30F);          /* mov.l @(15,pc),r3 ; r3 = MINIT  */
    w16(M_BASE + 0x08, 0x2301);          /* mov.w r0,@r3      ; pulse ICF   */
    w16(M_BASE + 0x0A, 0x6012);          /* mov.l @r1,r0      ; poll FLAG   */
    w16(M_BASE + 0x0C, 0x2008);          /* tst r0,r0                       */
    w16(M_BASE + 0x0E, 0x8BFB);          /* bf 0x08: spin INCLUDING the
                                          * pulse -- the slave's reset at
                                          * start wipes a capture that landed
                                          * too early, so keep ringing the
                                          * doorbell rather than ring once */
    w16(M_BASE + 0x10, 0xD20D);          /* mov.l @(13,pc),r2 ; r2 = &DONE  */
    w16(M_BASE + 0x12, 0xE05A);          /* mov #0x5A,r0                    */
    w16(M_BASE + 0x14, 0x2202);          /* mov.l r0,@r2                    */
    w16(M_BASE + 0x16, 0xAFFE);          /* bra .                           */
    w16(M_BASE + 0x18, 0x0009);          /* nop                             */
    w32(M_BASE + 0x40, FLAG);
    w32(M_BASE + 0x44, 0x01000000u);     /* MINIT: slave FRT capture pulse  */
    w32(M_BASE + 0x48, M_DONE);
}

/* slave:
 *   000  mov.l @(0x0F,pc),r2     ; r2 = 0xFFFFFE11 (FTCSR)
 *   002  mov   #-128,r1          ; r1 = 0xFFFFFF80, so tst masks ICF
 *   004  mov.b @r2,r0            ; poll:
 *   006  tst   r1,r0
 *   008  bt    poll              ; spin while ICF is clear
 *   00A  mov.l @(0x0E,pc),r3     ; r3 = &FLAG
 *   00C  mov   #0,r0
 *   00E  mov.l r0,@r3            ; release the master
 *   010  mov.l @(0x0D,pc),r4     ; r4 = &S_DONE
 *   012  mov   #-91,r0           ; 0xA5
 *   014  mov.l r0,@r4
 *   016  bra   .
 *   018  nop
 *   040  .long 0xFFFFFE11
 *   044  .long FLAG
 *   048  .long S_DONE
 */
static void build_slave(void)
{
    w16(S_BASE + 0x00, 0xD20F);
    w16(S_BASE + 0x02, 0xE180);
    w16(S_BASE + 0x04, 0x6020);
    w16(S_BASE + 0x06, 0x2018);
    w16(S_BASE + 0x08, 0x89FC);
    w16(S_BASE + 0x0A, 0xD30E);
    w16(S_BASE + 0x0C, 0xE000);
    w16(S_BASE + 0x0E, 0x2302);
    w16(S_BASE + 0x10, 0xD40D);
    w16(S_BASE + 0x12, 0xE0A5);
    w16(S_BASE + 0x14, 0x2402);
    w16(S_BASE + 0x16, 0xAFFE);
    w16(S_BASE + 0x18, 0x0009);
    w32(S_BASE + 0x40, 0xFFFFFE11u);
    w32(S_BASE + 0x44, FLAG);
    w32(S_BASE + 0x48, S_DONE);
}

/* --------------------------------------------------------------- 1. banks */

static void test_banks(void)
{
    memset(&S, 0, sizeof S);
    sh2_reset(&S.master, &S, 0, M_BASE, 0x06100000u);
    sh2_reset(&S.slave,  &S, 1, S_BASE, 0x060F0000u);

    /* Each core owns its on-chip file. Writing the master's TIER must not be
     * visible through the slave's, or the slave's FRT poll reads the master's
     * flags -- which is what it was doing. */
    S.cur = &S.master; bus_w8(&S, 0xFFFFFE10u, 0x11);
    S.cur = &S.slave;  bus_w8(&S, 0xFFFFFE10u, 0x22);

    S.cur = &S.master; ck("master TIER after slave wrote its own", bus_r8(&S, 0xFFFFFE10u), 0x11);
    S.cur = &S.slave;  ck("slave  TIER after master wrote its own", bus_r8(&S, 0xFFFFFE10u), 0x22);

    /* A 32-bit store lands on the same bank, and FRC reads live. */
    S.cur = &S.master;
    bus_w32(&S, 0xFFFFFE10u, 0x00001234u);      /* TIER=0 FTCSR=0 FRC=0x1234 */
    ck("master FRC via long write", bus_r16(&S, 0xFFFFFE12u), 0x1234);
    S.cur = &S.slave;
    ck("slave  FRC unaffected",     bus_r16(&S, 0xFFFFFE12u), 0x0000);
    S.cur = &S.master;
}

/* ----------------------------------------------------------------- 2. FRT */

static void test_frt(void)
{
    memset(&S, 0, sizeof S);
    sh2_reset(&S.master, &S, 0, M_BASE, 0x06100000u);
    S.cur = &S.master;

    /* TCR = 0 selects phi/8, so a full scanline advances FRC by 1820/8. */
    bus_w8(&S, 0xFFFFFE16u, 0x00);
    frt_advance(&S.master, CYC_PER_LINE);
    ck("FRC after one line at phi/8", S.master.frc, CYC_PER_LINE / 8u);

    /* The prescaler remainder carries: two more lines must not lose cycles. */
    frt_advance(&S.master, CYC_PER_LINE);
    frt_advance(&S.master, CYC_PER_LINE);
    ck("FRC after three lines", S.master.frc, (CYC_PER_LINE * 3u) / 8u);

    /* phi/128 counts sixteen times slower. */
    S.master.frc = 0; S.master.frt_pre = 0;
    bus_w8(&S, 0xFFFFFE16u, 0x02);
    frt_advance(&S.master, 1280u);
    ck("FRC after 1280 cycles at phi/128", S.master.frc, 10u);

    /* Input capture: ICF latches and FICR takes the count. This is the flag
     * NiGHTS's slave polls; it used to be synthesised per read from the
     * MASTER's cycle counter, so it was neither latched nor the right core. */
    ck("ICF clear before capture", bus_r8(&S, 0xFFFFFE11u) & 0x80u, 0x00);
    frt_capture(&S.master);
    ck("ICF set by capture",       bus_r8(&S, 0xFFFFFE11u) & 0x80u, 0x80);
    ck("FICR latched FRC",         bus_r16(&S, 0xFFFFFE18u), 10u);

    /* ICF stays set until software clears it -- a level, not a window. */
    frt_advance(&S.master, CYC_PER_LINE * 4u);
    ck("ICF still set without a write", bus_r8(&S, 0xFFFFFE11u) & 0x80u, 0x80);

    /* Write-to-clear: a zero in the bit clears it, a one leaves it alone. */
    bus_w8(&S, 0xFFFFFE11u, 0x00);
    ck("ICF cleared by writing 0", bus_r8(&S, 0xFFFFFE11u) & 0x80u, 0x00);
    frt_capture(&S.master);
    bus_w8(&S, 0xFFFFFE11u, 0xFF);
    ck("ICF survives writing 1",   bus_r8(&S, 0xFFFFFE11u) & 0x80u, 0x80);
}

/* ------------------------------------------------------- 3. TVSTAT H-Blank */

static void test_tvstat(void)
{
    memset(&S, 0, sizeof S);
    sh2_reset(&S.master, &S, 0, M_BASE, 0x06100000u);
    S.cur = &S.master;

    /* Off the machine clock, not off a core's own counter. The master's
     * counter stands still for the whole of the slave's quantum, which froze
     * H-Blank for exactly the core that was trying to observe it. */
    S.clk = 0;
    ck("H-Blank clear at line start", bus_r16(&S, 0x25F80004u) & 0x0004u, 0x0000);
    S.clk = HBLANK_START;
    ck("H-Blank set in the tail",     bus_r16(&S, 0x25F80004u) & 0x0004u, 0x0004);
    S.clk = CYC_PER_LINE;
    ck("H-Blank clear on next line",  bus_r16(&S, 0x25F80004u) & 0x0004u, 0x0000);

    /* Advancing the master's own cycle count must not move it at all. */
    S.master.cycles += HBLANK_START;
    ck("H-Blank ignores master cycles", bus_r16(&S, 0x25F80004u) & 0x0004u, 0x0000);
}

/* ------------------------------------------- 4. the SCU drives master only */

static void test_scu_is_master_only(void)
{
    memset(&S, 0, sizeof S);
    sh2_reset(&S.master, &S, 0, M_BASE, 0x06100000u);
    sh2_reset(&S.slave,  &S, 1, S_BASE, 0x060F0000u);

    /* A vector table both cores could dispatch through, and a nop at the
     * handler so a core that DOES take it visibly leaves its program. */
    w32(0x06000000u + 0x40u * 4u, 0x06060000u);
    w16(0x06060000u, 0x0009);
    build_slave();

    S.scu_reg[0xA0 >> 2] = 0;              /* nothing masked */
    S.slave.sr &= ~SR_I;                   /* slave would accept it if offered */
    scu_raise(&S, 0);                      /* V-Blank-IN */

    uint32_t pend_before = S.scu_ipend;
    S.cur = &S.slave;
    for (int k = 0; k < 8; k++) sh2_step(&S.slave);

    ck("slave did not consume the SCU pending bit", S.scu_ipend, pend_before);
    ck("slave did not vector to the handler",
       (S.slave.pc >= S_BASE && S.slave.pc < S_BASE + 0x80u), 1u);

    /* The master, offered the same interrupt, must take it. */
    S.master.sr &= ~SR_I;
    S.cur = &S.master;
    sh2_step(&S.master);
    ck("master took the SCU interrupt",
       (S.master.pc >= 0x06060000u && S.master.pc <= 0x06060004u), 1u);
    ck("master consumed the pending bit", S.scu_ipend & 1u, 0u);
}

/* ------------------------------------------------------ 5. the rendezvous */

static void test_rendezvous(void)
{
    memset(&S, 0, sizeof S);
    build_master();
    build_slave();

    sh2_reset(&S.master, &S, 0, M_BASE, 0x06100000u);
    /* This harness deliberately starts the slave at its synthetic routine.
     * Retail mode instead resets through the shared BIOS vectors. */
    S.hle_active    = 1;
    S.slave_enabled = 1;
    S.slave_entry   = S_BASE;
    S.slave_sp      = 0x060F0000u;

    /* Nothing masked at the SCU, but both cores boot with SR.I = 15, so no
     * interrupt is taken: this is purely the polled handshake. */
    for (int field = 0; field < 2 && bus_r32(&S, M_DONE) != 0x5Au; field++)
        saturn_run_field(&S);

    ck("slave ran and cleared the flag", bus_r32(&S, S_DONE), 0xFFFFFFA5u);
    ck("flag was cleared",               bus_r32(&S, FLAG),   0x00u);
    ck("master got past its park",       bus_r32(&S, M_DONE), 0x5Au);
    ck("master parked at its final loop", S.master.pc, M_BASE + 0x16u);

    /* And it closed promptly -- within the first field, not eventually. The
     * slave's capture arrives the moment the master's MINIT write lands. */
    ck("closed inside one field", (S.frames <= 1), 1u);
}

/* Both cores must advance against the SAME clock. If a quantum only ever
 * advanced one of them, a poll on one core could never see the other's store. */
static void test_clock_is_shared(void)
{
    memset(&S, 0, sizeof S);
    build_master();
    build_slave();
    sh2_reset(&S.master, &S, 0, M_BASE, 0x06100000u);
    S.hle_active    = 1;
    S.slave_enabled = 1;
    S.slave_entry   = S_BASE;
    S.slave_sp      = 0x060F0000u;

    saturn_run_line(&S);
    ck("clock advanced one line", (uint32_t)S.clk, CYC_PER_LINE);
    ck("slave was started",       (uint32_t)S.slave_started, 1u);
    ck("slave advanced too",      (S.slave.cycles > 0), 1u);
    ck("master advanced too",     (S.master.cycles > 0), 1u);

    /* RunFrame stops at V-Blank-IN, when the visible field is complete.  The
     * next call resumes the blanking scanlines and runs through the following
     * visible field to the next V-Blank-IN. */
    S.clk = 0;
    saturn_run_field(&S);
    ck("first frame ends at V-Blank-IN",
       (uint32_t)S.clk, CYC_PER_LINE * LINE_VBLANK);
    ck("frame boundary line", S.line, LINE_VBLANK);
    ck("V-Blank-IN edge already asserted", S.vblank_boundary_done, 1u);
}

/* ------------------------------------------------------------- 6. DIVU */

static void test_divu(void)
{
    memset(&S, 0, sizeof S);
    sh2_reset(&S.master, &S, 0, M_BASE, 0x06100000u);
    S.cur = &S.master;

    /* 32/32: write DVSR then DVDNT; the DVDNT write starts the division.
     * -100 / 7 = -14 rem -2 (C truncation, which the hardware matches). */
    bus_w32(&S, 0xFFFFFF00u, 7u);
    bus_w32(&S, 0xFFFFFF04u, (uint32_t)-100);
    ck("divu 32 quotient",  bus_r32(&S, 0xFFFFFF04u), (uint32_t)-14);
    ck("divu 32 remainder", bus_r32(&S, 0xFFFFFF10u), (uint32_t)-2);

    /* 64/32 fixed-point idiom: (6.0 << 16) / 2.0 in 16.16 = 3.0 << 16.
     * DVDNTH:DVDNTL = 0x0006_0000:0x0000_0000 / 0x00020000. */
    bus_w32(&S, 0xFFFFFF00u, 0x00020000u);
    bus_w32(&S, 0xFFFFFF10u, 0x00000006u);
    bus_w32(&S, 0xFFFFFF14u, 0x00000000u);
    ck("divu 64 quotient",  bus_r32(&S, 0xFFFFFF14u), 0x00030000u);
    ck("divu 64 remainder", bus_r32(&S, 0xFFFFFF10u), 0x00000000u);

    /* Division by zero saturates by sign when the overflow interrupt is
     * disabled, and latches DVCR.OVF. */
    bus_w32(&S, 0xFFFFFF08u, 0u);
    bus_w32(&S, 0xFFFFFF00u, 0u);
    bus_w32(&S, 0xFFFFFF04u, 123u);
    ck("divu div0 saturates +", bus_r32(&S, 0xFFFFFF04u), 0x7FFFFFFFu);
    ck("divu div0 sets OVF",    bus_r32(&S, 0xFFFFFF08u) & 1u, 1u);

    /* The 0xFF20 block mirrors 0xFF00. */
    bus_w32(&S, 0xFFFFFF28u, 0u);           /* clear DVCR via the mirror */
    bus_w32(&S, 0xFFFFFF20u, 5u);           /* DVSR via the mirror       */
    bus_w32(&S, 0xFFFFFF04u, 17u);
    ck("divu mirror DVSR", bus_r32(&S, 0xFFFFFF04u), 3u);
    ck("divu mirror read", bus_r32(&S, 0xFFFFFF24u), 3u);
}

int main(void)
{
    /* SATURN_BENCH: pure-interpreter throughput on a tight two-instruction
     * loop, no scheduler, no video, no CD. Separates "the interpreter is
     * slow" from "the field machinery is slow". */
    if (getenv("SATURN_BENCH")) {
        memset(&S, 0, sizeof S);
        if (getenv("SATURN_BENCH2")) {
            /* memory-heavy: mov.l @r1,r2 / mov.l r2,@r3 / dt / bf */
            w16(0x06040000u + 0, 0x6212);
            w16(0x06040000u + 2, 0x2322);
            w16(0x06040000u + 4, 0x4510);
            w16(0x06040000u + 6, 0x8BFB);
            w16(0x06040000u + 8, 0xAFFE);
            w16(0x06040000u + 10, 0x0009);
            sh2_reset(&S.master, &S, 0, 0x06040000u, 0x06100000u);
            S.master.r[1] = 0x06050000u;
            S.master.r[3] = 0x06050100u;
            S.master.r[5] = 0x7FFFFFFF;
            S.cur = &S.master;
            sh2_run(&S.master, 30000000);
            printf("bench2 done\n");
            return 0;
        }
        w16(0x06040000u + 0, 0x4510);          /* dt r5        */
        w16(0x06040000u + 2, 0x8BFD);          /* bf back      */
        w16(0x06040000u + 4, 0xAFFE);          /* bra .        */
        w16(0x06040000u + 6, 0x0009);
        sh2_reset(&S.master, &S, 0, 0x06040000u, 0x06100000u);
        S.master.r[5] = 0x7FFFFFFF;
        S.cur = &S.master;
        sh2_run(&S.master, 30000000);
        printf("bench done\n");
        return 0;
    }

    printf("dual-CPU scheduler, per-core on-chip banks, FRT\n");
    test_banks();
    test_frt();
    test_tvstat();
    test_scu_is_master_only();
    test_clock_is_shared();
    test_rendezvous();
    test_divu();

    if (fails) { printf("FAILED: %d check(s)\n", fails); return 1; }
    printf("PASS: dual-CPU checks\n");
    return 0;
}
