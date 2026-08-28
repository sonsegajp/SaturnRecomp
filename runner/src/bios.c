/* bios.c — minimal HLE of the Saturn BIOS presence.
 *
 * The IPL leaves two things behind that every game depends on and that are NOT
 * part of the game's own image:
 *
 *   1. A block of BIOS work area / system-call vectors in WRAM-H below the
 *      1st-read load address (0x06000000-0x06003FFF). Games call BIOS routines
 *      indirectly through it, e.g.  mov.l @r2,r3 ; jsr @r3  with r2 = 0x06000320.
 *   2. Sane initial values in a handful of hardware registers.
 *
 * With nothing there, the first indirect BIOS call jumps to zero -- which is
 * exactly where NiGHTS dies at instruction 580,526.
 *
 * This installs a stub: every vector slot points at a tiny `rts` thunk in the
 * boot ROM region, so BIOS calls return harmlessly instead of crashing. That is
 * deliberately NOT an implementation of the BIOS -- it is scaffolding that lets
 * boot proceed so we can see which call actually needs real behaviour next.
 * Each thunk is distinct, so a trace tells us precisely which vector was used.
 */
#include "saturn.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Where the stub thunks live in the boot ROM image we synthesise. */
#define STUB_BASE   0x00000400u
#define STUB_STRIDE 4u
#define NUM_STUBS   256u

/* Two kinds of thunk, because this region serves two purposes:
 *   - Exception/interrupt VECTORS (VBR + vector*4) must return with RTE, which
 *     pops the PC and SR the exception pushed. Returning with RTS there leaves
 *     two words of garbage on the stack and corrupts SR.
 *   - BIOS FUNCTION POINTERS are reached by `jsr @rN` and must return with RTS.
 *
 * Sega's BIOS puts its callable entry points in the upper, unused part of the
 * vector table (vectors 0xC0-0xFF => 0x06000300-0x060003FF), which is exactly
 * where NiGHTS calls through: r2 = 0x06000320. */
#define RTE_STUB    0x00000200u

/* Per-vector interrupt trampolines we author, and the user handler table they
 * dispatch through. 0x06000A00 is where the real BIOS puts that table, so a
 * game's existing handler-registration code works unmodified. */
#define IRQ_THUNK_BASE  0x00008000u
#define USER_VEC_TABLE  0x06000A00u

/* The BIOS work area / vector region, below the 1st-read load address. */
#define VEC_LO      0x06000000u
/* SH-2 exception/interrupt vectors occupy entries 0x00-0x7F, i.e. up to
 * 0x060001FF. Everything above that is free, and the BIOS puts its callable
 * entry points there -- NiGHTS calls through 0x0600026C, which a boundary of
 * 0x06000300 would wrongly treat as an interrupt vector and answer with an
 * `rte` thunk, unwinding the caller into garbage. */
#define VEC_FUNCS   0x06000200u
#define VEC_HI      0x06004000u

void bios_hle_install(saturn *s)
{
    s->hle_active = 1;
    /* Shared `rte` thunk for any interrupt the game has not hooked yet. */
    s->bios[RTE_STUB + 0] = 0x00;  s->bios[RTE_STUB + 1] = 0x2B;   /* rte */
    s->bios[RTE_STUB + 2] = 0x00;  s->bios[RTE_STUB + 3] = 0x09;   /* nop */

    /* Each function thunk is `rts` + `nop`, at a distinct address so a PC
     * trace identifies exactly which BIOS entry point the game called. */
    for (uint32_t i = 0; i < NUM_STUBS; i++) {
        uint32_t a = STUB_BASE + i * STUB_STRIDE;
        s->bios[a + 0] = 0x00;  s->bios[a + 1] = 0x0B;   /* rts */
        s->bios[a + 2] = 0x00;  s->bios[a + 3] = 0x09;   /* nop */
    }

    /* Interrupt vectors do NOT point straight at handlers on a real Saturn.
     * The BIOS installs a trampoline per vector which looks the handler up in
     * a user table and calls it, so a game can install a handler simply by
     * storing a pointer -- which is exactly what NiGHTS does, and why leaving
     * bare `rte` stubs here left every vector unhooked forever.
     *
     * Confirmed by disassembling the real BIOS's own work area: its per-vector
     * trampolines at 0x06000840 push r0, load the vector number, and branch to
     * a common dispatcher at 0x060008F4 that does `mov.l @(r0,r3),r6; jsr @r6`
     * with r3 = 0x06000A00.
     *
     * We reproduce the *mechanism*, not the code. Each vector gets its own
     * 16-byte thunk we author here:
     *
     *     mov.l @(20,pc),r0   ; r0 = &user_table[vector]
     *     mov.l @r0,r0        ; r0 = handler
     *     tst   r0,r0
     *     bt    .none         ; unregistered -> return from the exception
     *     sts.l pr,@-r15      ; the handler is CALLED, not jumped to...
     *     jsr   @r0
     *     nop
     *     lds.l @r15+,pr      ; ...so its rts lands here
     *     nop
     * .none:
     *     rte                 ; only an rte restores the pushed PC and SR
     *     nop
     *     .long user_table + vector*4
     *
     * Calling matters: the handler ends in `rts`, so jumping to it would send
     * that `rts` to a stale PR and leave the exception's PC and SR stranded on
     * the stack. Only the trailing `rte` unwinds the exception correctly.
     */
    /* Only entries 0x00-0x7F are real exception/interrupt vectors; writing
     * thunks above that would stamp over the BIOS function pointers. */
    for (uint32_t v = 0; v < 128; v++) {
        uint32_t a = IRQ_THUNK_BASE + v * 32u;      /* 32-byte stride, aligned */
        uint32_t slot = USER_VEC_TABLE + v * 4u;
        static const uint8_t body[24] = {
            0xD0, 0x05,   /* mov.l @(20,pc),r0  -> the .long at a+24 */
            0x60, 0x02,   /* mov.l @r0,r0                            */
            0x20, 0x08,   /* tst   r0,r0                             */
            0x89, 0x04,   /* bt    a+18 (.none)                      */
            0x4F, 0x22,   /* sts.l pr,@-r15                          */
            0x40, 0x0B,   /* jsr   @r0                               */
            0x00, 0x09,   /* nop   (delay slot)                      */
            0x4F, 0x26,   /* lds.l @r15+,pr                          */
            0x00, 0x09,   /* nop                                     */
            0x00, 0x2B,   /* rte                                     */
            0x00, 0x09,   /* nop   (delay slot)                      */
            0x00, 0x09,   /* nop   (pad, keeps the literal aligned)  */
        };
        if (a + 28 > BIOS_SIZE) break;
        memcpy(&s->bios[a], body, sizeof(body));
        s->bios[a + 24] = (uint8_t)(slot >> 24);
        s->bios[a + 25] = (uint8_t)(slot >> 16);
        s->bios[a + 26] = (uint8_t)(slot >> 8);
        s->bios[a + 27] = (uint8_t)slot;

        bus_w32(s, VEC_LO + v * 4u, a);
    }

    /* BIOS function pointers -> rts thunks.
     *
     * Must skip the user interrupt vector table: it lives inside this range,
     * and filling it with rts-thunk pointers makes every interrupt dispatch
     * into an `rts` that returns to whatever PR happened to hold, instead of
     * the `rte` the exception needs. */
    {
        uint32_t idx = 0;
        for (uint32_t a = VEC_FUNCS; a < VEC_HI; a += 4, idx++) {
            if (a >= USER_VEC_TABLE && a < USER_VEC_TABLE + 256 * 4u) continue;
            bus_w32(s, a, STUB_BASE + (idx % NUM_STUBS) * STUB_STRIDE);
        }
    }

    /* Clear the user handler table last, so nothing above can clobber it. */
    for (uint32_t v = 0; v < 256; v++)
        bus_w32(s, USER_VEC_TABLE + v * 4u, 0);

    /* Replace the plain rts thunks in the service region with HLE traps. */
    bios_hle_traps_install(s);

    /* SMPC: not busy, so the standard "poll SF, then issue command" handshake
     * completes rather than spinning forever. */
    s->smpc_reg[0x63 & 0x7F] = 0x00;   /* SF  */
    s->smpc_reg[0x61 & 0x7F] = 0x00;   /* SR  */

    /* SMPC OREG0..: report a standard digital control pad on port 1 and
     * nothing on port 2, with all buttons released (active low). */
    s->smpc_reg[0x21 & 0x7F] = 0xF1;   /* port 1 status: 1 device        */
    s->smpc_reg[0x23 & 0x7F] = 0x02;   /* peripheral ID: digital, 2 bytes*/
    s->smpc_reg[0x25 & 0x7F] = 0xFF;
    s->smpc_reg[0x27 & 0x7F] = 0xFF;
    s->smpc_reg[0x29 & 0x7F] = 0xF0;   /* port 2: no device              */
}

/* Which BIOS vector slot a stub address corresponds to, or -1. */
int bios_stub_vector(uint32_t pc)
{
    if (pc < STUB_BASE || pc >= STUB_BASE + NUM_STUBS * STUB_STRIDE) return -1;
    return (int)((pc - STUB_BASE) / STUB_STRIDE);
}

/* --------------------------------------------------------- real BIOS ----
 * Loading a real 512 KB Saturn BIOS replaces every stub above. The IPL then
 * does what it does on hardware: initialise the peripherals, build the
 * interrupt vector table and BIOS call table in WRAM-H, authenticate the disc,
 * load the 1st-read file and jump to it.
 *
 * The ROM is a user-supplied input, exactly like the disc image. Nothing from
 * it is redistributed and nothing from it is compiled in.
 */
int bios_rom_load(saturn *s, const char *path, char *err, size_t errsz)
{
    FILE *f = fopen(path, "rb");
    size_t n;

    if (!f) { snprintf(err, errsz, "cannot open BIOS: %.200s", path); return -1; }
    n = fread(s->bios, 1, BIOS_SIZE, f);
    fclose(f);
    if (n != BIOS_SIZE) {
        snprintf(err, errsz, "BIOS must be %u bytes, got %zu", (unsigned)BIOS_SIZE, n);
        return -1;
    }
    return 0;
}

/* SH-2 reset: PC and SP come from the first two longwords of the vector table
 * at address 0, which for the Saturn lives at the start of the boot ROM. */
void bios_reset_vector(saturn *s, uint32_t *pc, uint32_t *sp)
{
    *pc = ((uint32_t)s->bios[0] << 24) | ((uint32_t)s->bios[1] << 16) |
          ((uint32_t)s->bios[2] << 8)  |  (uint32_t)s->bios[3];
    *sp = ((uint32_t)s->bios[4] << 24) | ((uint32_t)s->bios[5] << 16) |
          ((uint32_t)s->bios[6] << 8)  |  (uint32_t)s->bios[7];
}

/* ------------------------------------------------------- HLE call traps ----
 * Games reach BIOS services through function pointers in the vector table's
 * upper region. To implement those services we point each slot at a distinct
 * sentinel address; sh2_step recognises the range, calls bios_hle_call() and
 * then returns as if the routine had executed `rts`.
 *
 * Every call is logged with its arguments, so the set of services a title
 * actually needs is discovered from the title rather than guessed.
 */
void bios_hle_traps_install(saturn *s)
{
    for (uint32_t v = 0; v < BIOS_HLE_COUNT; v++) {
        uint32_t slot = VEC_FUNCS + v * 4u;
        /* 0x06000250/54 is the SLAVE SH-2 entry/stack pair, not a BIOS service
         * pointer. Installing a trap sentinel there means the slave can never
         * be given a start address. */
        if (slot == 0x06000250u || slot == 0x06000254u) continue;
        bus_w32(s, slot, BIOS_HLE_BASE + v * 4u);
    }
}

int bios_hle_is_trap(saturn *s, uint32_t pc)
{
    /* The sentinel range 0x0000C000-0x0000C1FF lies INSIDE the BIOS ROM. With a
     * real BIOS loaded, treating it as a trap hijacks genuine ROM code: we ran
     * an HLE stub, overwrote r0 and jumped to pr. That is what left r0 = 0 in
     * the BIOS's normalisation loop at 0x06012C88, where it should have held
     * 0x00010000 -- 0 >= 0 is true forever, so the boot spun there. Only honour
     * the sentinels when we are actually standing in for the BIOS. */
    if (!s->hle_active) return 0;
    return pc >= BIOS_HLE_BASE && pc < BIOS_HLE_BASE + BIOS_HLE_COUNT * 4u;
}

/* Returns non-zero if the call was serviced. r0 carries any return value. */
int bios_hle_call(saturn *s, uint32_t pc, uint32_t *r)
{
    uint32_t id   = (pc - BIOS_HLE_BASE) / 4u;
    uint32_t slot = VEC_FUNCS + id * 4u;

    /* Log first sighting of each service, with arguments. */
    {
        int seen = 0;
        for (int i = 0; i < s->nbioscall; i++)
            if (s->bioscall[i].slot == slot) { s->bioscall[i].count++; seen = 1; break; }
        if (!seen && s->nbioscall < BIOSCALL_SLOTS) {
            s->bioscall[s->nbioscall].slot  = slot;
            s->bioscall[s->nbioscall].r4    = r[4];
            s->bioscall[s->nbioscall].r5    = r[5];
            s->bioscall[s->nbioscall].r6    = r[6];
            s->bioscall[s->nbioscall].r7    = r[7];
            s->bioscall[s->nbioscall].count = 1;
            s->nbioscall++;
        }
    }

    switch (slot) {
    case 0x06000300:
    case 0x06000310:
        /* Set interrupt handler: r4 = vector number, r5 = handler address.
         *
         * Identified from NiGHTS itself rather than guessed -- the call log
         * showed slot 0x06000300 invoked four times with r4 = 0x40 (V-Blank-IN)
         * and r5 = 0x0600461A, an address inside the game's own image. Without
         * this the title never hooks VBlank and spins forever on a flag its
         * handler would have set. */
        if (r[4] < 256u) bus_w32(s, USER_VEC_TABLE + r[4] * 4u, r[5]);
        r[0] = 0;
        return 1;

    case 0x06000344:
        /* Set SCU interrupt mask: r4 = new mask (a SET bit masks the source).
         * NiGHTS passes 0xFFFFDFF4, clearing bits 0, 1, 3 and 13 -- V-Blank-IN,
         * V-Blank-OUT, Timer 0 and Sprite Draw End. */
        /* [TEST] SATURN_MASKINV: treat r4 as a set of interrupts to ENABLE
         * rather than to mask. If the service's convention is inverted from
         * ours we have been masking exactly what the game asked to receive. */
        s->scu_reg[0xA0 >> 2] = getenv("SATURN_MASKINV") ? ~r[4] : r[4];
        r[0] = 0;
        return 1;

    case 0x06000358:
        /* Slave SH-2 start. NiGHTS calls this with r4 = 0x0607C000,
         * r5 = 0x06062CC4, r6 = 0x06001FE0 -- an entry point and two stacks,
         * all in WRAM-H. Park the entry where run_slave() picks it up. The
         * game also issues SMPC SSHON, which is what actually releases it. */
        if (r[5] >= 0x06000000u && r[5] < 0x06100000u) {
            /* Keep this in machine state, not guest memory: anything we park
             * at 0x06000250 gets overwritten before SSHON arrives. */
            s->slave_entry = r[5];
            s->slave_sp    = (r[4] >= 0x06000000u && r[4] < 0x06100000u)
                             ? r[4] : 0x0607C000u;
        }
        r[0] = 0;
        return 1;

    case 0x0600026C: {
        /* The "HCDM" CD service (real BIOS: ROM 0x0000186C -> 0x00000424).
         * NiGHTS calls it with r4 = -1, r5 = a destination buffer, r6 = a
         * length. [EXPERIMENT, off unless SATURN_HCDM] Serve it as "fill that
         * buffer from the disc" to see whether the loader advances. The real
         * routine is a system-level path we have not decoded, so this is a
         * probe, not a claim about its semantics. */
        uint32_t dst = r[5], words = r[6] & 0xFFFFFFu;
        if (getenv("SATURN_HCDM") && dst >= 0x06000000u && words && words < 0x40000u) {
            for (uint32_t i = 0; i < words; i++)
                bus_w16(s, dst + i * 2u, cdb_read_dtr(s));
        }
        r[0] = 0;
        return 1;
    }

    default:
        /* Unimplemented service: return 0 and keep going. It shows up in the
         * call log, which is the work list. */
        r[0] = 0;
        return 1;
    }
}
