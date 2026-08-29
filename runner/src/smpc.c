#include <stdio.h>
/* smpc.c — System Manager & Peripheral Control.
 *
 * The SMPC owns everything the SH-2s cannot do for themselves: reset, the
 * system clock, enabling the slave CPU and the sound CPU, the RTC, and all
 * controller input. The host writes a command byte to COMREG, sets SF, and
 * polls SF until the SMPC clears it; results come back in OREG0..OREG31 and,
 * for INTBACK, with a System Manager interrupt. See docs/HARDWARE.md §8.
 *
 * INTBACK is the one that matters at boot: it returns RTC, system status and
 * peripheral data, and titles poll it every frame. Answering only "not busy"
 * without ever producing data leaves a game waiting forever for input that
 * never arrives.
 */
#include "saturn.h"
#include <stdio.h>
#include <stdlib.h>

static int smpc_dbg = -1;
static int smpcdbg(void) {
    if (smpc_dbg < 0) smpc_dbg = getenv("SATURN_SMPCDBG") != NULL;
    return smpc_dbg;
}

#include <string.h>
#include <stdlib.h>

/* Register offsets within the SMPC block (odd addresses only on real hw). */
#define IREG0   0x01
#define COMREG  0x1F
#define OREG0   0x21
#define SR      0x61
#define SF      0x63

#define OREG(n) (uint32_t)(OREG0 + (n) * 2)

/* Commands */
#define CMD_MSHON     0x00
#define CMD_SSHON     0x02
#define CMD_SSHOFF    0x03
#define CMD_SNDON     0x06
#define CMD_SNDOFF    0x07
#define CMD_CDON      0x08
#define CMD_CDOFF     0x09
#define CMD_SYSRES    0x0D
#define CMD_CKCHG352  0x0E
#define CMD_CKCHG320  0x0F
#define CMD_INTBACK   0x10
#define CMD_SETTIME   0x16
#define CMD_SETSMEM   0x17
#define CMD_NMIREQ    0x18
#define CMD_RESENAB   0x19
#define CMD_RESDISA   0x1A

static void ow(saturn *s, int n, uint8_t v)
{
    s->smpc_reg[OREG(n) & 0x7F] = v;
}

static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/* IP.BIN lists every area a disc will run in, most-preferred first ("JTU" is
 * Japan+Asia+USA). The console has exactly one. Prefer the entry matching the
 * BIOS most builds here use, then fall back in order, so a multi-region disc
 * lands on a region its BIOS actually is. */
uint8_t saturn_area_from_ip(const char *area)
{
    static const struct { char c; uint8_t code; } map[] = {
        { 'J', 0x01 },  /* Japan                    */
        { 'T', 0x02 },  /* Asia NTSC                */
        { 'U', 0x04 },  /* North America            */
        { 'B', 0x05 },  /* Central/South America NTSC */
        { 'K', 0x06 },  /* Korea                    */
        { 'A', 0x0A },  /* East Asia PAL            */
        { 'E', 0x0C },  /* Europe                   */
        { 'L', 0x0D },  /* Central/South America PAL */
    };
    static const char pref[] = "UEJTBKAL";
    size_t i, k;

    if (!area) return 0x04;
    for (k = 0; k < sizeof(pref) - 1; k++)
        for (i = 0; area[i]; i++)
            if (area[i] == pref[k])
                for (size_t m = 0; m < sizeof(map)/sizeof(map[0]); m++)
                    if (map[m].c == pref[k]) return map[m].code;
    return 0x04;
}

/* Fill OREG with the INTBACK status block: RTC, cartridge/area, and the
 * system settings the BIOS wrote to backup memory. */
static void intback_status(saturn *s)
{
    /* OREG0 bit 7 is STE: "settings/time have been saved". It STARTS clear
     * (first boot -> Set Language) and is SET once the host issues SETSMEM or
     * SETTIME (Ymir smpc.cpp: SETSMEM does m_STE = true). Hardcoding it kept
     * the BIOS in setup forever: the user exited the menu, the BIOS saved via
     * SETSMEM, re-read INTBACK, still saw "never configured", and looped back
     * to Set Language. SATURN_OREG0 still overrides for path testing. */
    /* Bit 6 is RESD, the reset-disable latch. Ymir starts it set and updates
     * it on RESENAB/RESDISA. Dropping this bit made the real IPL observe a
     * different machine state from the reference during its disc handoff. */
    ow(s,  0, (uint8_t)(getenv("SATURN_OREG0")
                        ? strtoul(getenv("SATURN_OREG0"), NULL, 0)
                        : ((s->smpc_ste ? 0x80 : 0x00) |
                           (s->smpc_resd ? 0x40 : 0x00))));
    /* RTC year. Sweepable: a Saturn shipped in 1994 and its BIOS may reject a
     * year outside the range it knows, which would force the clock-setting
     * screen instead of booting. */
    {
        const char *y = getenv("SATURN_YEAR");
        int yy = y ? atoi(y) : 1998;
        ow(s, 1, bcd(yy / 100));
        ow(s, 2, bcd(yy % 100));
    }
    ow(s,  3, 0x08);                 /* day of week / month                  */
    ow(s,  4, bcd(18));              /* day                                  */
    ow(s,  5, bcd(2));               /* hour                                 */
    ow(s,  6, bcd(0));               /* minute                               */
    ow(s,  7, bcd(0));               /* second                               */
    ow(s,  8, 0x00);                 /* cartridge code                       */
    ow(s,  9, s->area_code ? s->area_code : 0x04);   /* area code (see saturn.h) */
    /* Ymir/SMPC: bit 6 is DOTSEL (352-dot clock), bit 3 is MSHNMI, and the
     * fixed base is 0x34. No NMI is asserted in the normal cold-boot path. */
    ow(s, 10, (uint8_t)(0x34 | (s->clock_352 ? 0x40 : 0x00)));
    ow(s, 11, 0x00);                 /* system status 2                      */
    for (int i = 0; i < 4; i++) ow(s, 12 + i, s->smem[i]);
}

/* Peripheral data: one digital control pad on port 1, nothing on port 2.
 * Buttons are ACTIVE LOW, so 0xFF means nothing pressed. Report format per
 * Ymir ReadPeripherals/WriteINTBACKPeripheralReport: port status, then per
 * device an ID byte and its data; unused OREGs fill with 0xFF.
 *
 * SR for a peripheral report: bit7 SET, PDL set on the FIRST report of the
 * sequence, NPE set only if more chunks follow (ours always fits in one),
 * low nibble = the port modes latched from IREG1. */
/* The pad state as of right now, after the scripted-input knobs have had their
 * say. BOTH input paths must go through this: INTBACK and direct port I/O are
 * two ways of asking the same question, and having SATURN_PADSEQ reach only
 * the INTBACK path made scripted input silently dead on every screen that
 * reads the ports directly -- which is why the BIOS setup screens ignored a
 * sequence the peripheral report showed being delivered. */
static void pad_now(saturn *s, uint8_t *lo, uint8_t *hi)
{
    static uint8_t trace_lo, trace_hi;
    static int trace_valid;
    static uint8_t live_lo, live_hi;
    static uint64_t live_read_cycle;
    static uint64_t live_until;
    static unsigned live_serial = ~0u;
    *lo = s->pad1_lo;
    *hi = s->pad1_hi;
    if (s->pad_at && s->master.cycles < s->pad_at) { *lo = 0; *hi = 0; }
    if (s->npadseq) {
        int q;
        *lo = 0; *hi = 0;
        for (q = 0; q < s->npadseq; q++)
            if (s->master.cycles >= s->padseq[q].cy) {
                *lo = s->padseq[q].lo;
                *hi = s->padseq[q].hi;
            }
    }
    /* SATURN_PADFILE is the screen-driven counterpart to SATURN_PADSEQ.
     * The headless validation harness can change a tiny text file after it
     * has observed the title/menu/level frame instead of guessing when that
     * frame will occur in master-CPU cycles.  The file contains one or two
     * hexadecimal bytes ("lo [hi]") in the same active-high layout as
     * SATURN_PAD.  Throttle host I/O to roughly once per video field; the
     * title polls PDR many times per field and opening a file on every read
     * would turn a test facility into a performance bug. */
    {
        const char *path = getenv("SATURN_PADFILE");
        if (path && *path) {
            if (!live_read_cycle || s->master.cycles < live_read_cycle ||
                s->master.cycles - live_read_cycle >= 500000u) {
                FILE *f = fopen(path, "r");
                unsigned a = live_lo, b = live_hi, serial = live_serial;
                if (f) {
                    int n = fscanf(f, "%x %x %u", &a, &b, &serial);
                    fclose(f);
                    if (n >= 3) {
                        /* A changed serial is a self-releasing button edge.
                         * Twenty million master cycles is long enough to be
                         * sampled across a video field, but short enough not
                         * to become A+B+C+Start or pause a newly loaded game. */
                        if (serial != live_serial) {
                            live_serial = serial;
                            live_lo = (uint8_t)a;
                            live_hi = (uint8_t)b;
                            live_until = s->master.cycles + 20000000u;
                        }
                    } else if (n >= 1) {
                        /* Without a serial the file describes held state,
                         * useful once a gameplay test starts moving. */
                        live_lo = (uint8_t)a;
                        if (n >= 2) live_hi = (uint8_t)b;
                        live_until = 0;
                    }
                }
                live_read_cycle = s->master.cycles;
            }
            if (live_until && s->master.cycles >= live_until) {
                live_lo = live_hi = 0;
                live_until = 0;
            }
            *lo = live_lo;
            *hi = live_hi;
        }
    }
    if (getenv("SATURN_PADTRACE") &&
        (!trace_valid || *lo != trace_lo || *hi != trace_hi)) {
        fprintf(stderr, "[padtrace] pad=%02X %02X mastercy=%llu clk=%llu\n",
                *lo, *hi, (unsigned long long)s->master.cycles,
                (unsigned long long)s->clk);
        trace_lo = *lo;
        trace_hi = *hi;
        trace_valid = 1;
    }
}

static void intback_peripheral(saturn *s)
{
    int i;
    ow(s, 0, 0xF1);                  /* port 1: direct, 1 device             */
    ow(s, 1, 0x02);                  /* peripheral ID: digital pad, 2 bytes  */
    {
        /* SATURN_PADAT delays the press until a chosen cycle, so a headless
         * run can present a press EDGE while the menu is live -- a button held
         * from power-on never transitions, and menus act on transitions. */
        uint8_t lo, hi;
        pad_now(s, &lo, &hi);
        /* Report what is actually SENT, after SATURN_PADSEQ / SATURN_PADAT
         * have had their say. Printing the raw pad fields BEFORE this
         * resolution showed "no buttons" for every scripted press and made
         * a working sequence look dead. */
        if (smpcdbg())
            printf("[smpc] PERIPHERAL report, pad=%02X %02X first=%d cy=%llu\n",
                   lo, hi, s->ib_first,
                   (unsigned long long)s->master.cycles);
        ow(s, 2, (uint8_t)~lo);
        ow(s, 3, (uint8_t)~hi);
    }
    ow(s, 4, 0xF0);                  /* port 2: direct, no device            */
    for (i = 5; i < 32; i++) ow(s, i, 0xFF);

    s->smpc_reg[SR & 0x7F] = (uint8_t)(0x80
                            | (s->ib_first ? 0x40 : 0x00)      /* PDL */
                            | (s->ib_p1md << 0)
                            | (s->ib_p2md << 2));
    s->ib_first  = 0;
    s->ib_active = 0;                /* single chunk: nothing more owed      */
}

/* IREG0 written while an INTBACK peripheral phase is owed: bit 7 is a
 * continue request (send the peripheral report and interrupt again), bit 6 a
 * break (abandon the phase, clear the sequence bits in SR). Any other write
 * is just a parameter for the next command. */
void smpc_ireg0_write(saturn *s, uint8_t v)
{
    if (smpcdbg())
        printf("[smpc] IREG0=%02X ib_active=%d cy=%llu\n", v, s->ib_active,
               (unsigned long long)s->master.cycles);
    if (!s->ib_active) return;
    if (v & 0x40) {
        s->ib_active = 0;
        s->smpc_reg[SR & 0x7F] &= (uint8_t)~0x60u;   /* clear NPE|PDL */
        return;
    }
    if (v & 0x80) {
        intback_peripheral(s);
        scu_raise(s, 7);
    }
}

static void smpc_execute_command(saturn *s, uint8_t cmd)
{
    if (getenv("SATURN_SMPC_CMDS") && cmd != CMD_INTBACK)
        printf("[smpc-cmd] cmd=%02X pc=%08X pr=%08X cy=%llu\n",
               cmd, s->master.pc, s->master.pr,
               (unsigned long long)s->master.cycles);
    if (getenv("SATURN_SMPC_CMDS") &&
        (cmd == CMD_CKCHG320 || cmd == CMD_CKCHG352)) {
        printf("[ckchg-state] vbr=%08X sp=%08X r5=%08X r6=%08X "
               "sysck=%08X warm=%08X entry=%08X\n",
               s->master.vbr, s->master.r[15], s->master.r[5],
               s->master.r[6], bus_r32(s, 0x06000328u),
               bus_r32(s, 0x06000234u), bus_r32(s, 0x06000680u));
        printf("[ckchg-entry] %08X %08X %08X %08X %08X %08X %08X %08X\n",
               bus_r32(s, 0x06000680u), bus_r32(s, 0x06000684u),
               bus_r32(s, 0x06000688u), bus_r32(s, 0x0600068Cu),
               bus_r32(s, 0x06000690u), bus_r32(s, 0x06000694u),
               bus_r32(s, 0x06000698u), bus_r32(s, 0x0600069Cu));
    }
    /* Log first sighting so the command mix a title needs is visible. */
    {
        int seen = 0;
        for (int i = 0; i < s->nsmpccmd; i++)
            if (s->smpccmd[i].cmd == cmd) { s->smpccmd[i].count++; seen = 1; break; }
        if (!seen && s->nsmpccmd < 32) {
            s->smpccmd[s->nsmpccmd].cmd   = cmd;
            s->smpccmd[s->nsmpccmd].count = 1;
            s->nsmpccmd++;
        }
    }

    switch (cmd) {
    case CMD_INTBACK: {
        /* INTBACK is a protocol (Ymir smpc.cpp INTBACK()): IREG0 bit 0 asks
         * for the status block, IREG1 bit 3 for peripheral data, IREG1 bits
         * 4-7 are the port modes echoed back through SR. When both are asked
         * for, the peripheral block is NOT sent now -- the host reads the
         * status report and then writes IREG0 bit 7 (a continue request,
         * handled in smpc_ireg0_write) to fetch it. Answering only one phase
         * was why pad input never reached the BIOS: it read status, asked to
         * continue, and no one was listening. */
        uint8_t ireg0 = s->smpc_reg[IREG0 & 0x7F];
        uint8_t ireg1 = s->smpc_reg[(IREG0 + 2) & 0x7F];

        if (s->nintback < 8) {
            s->intback_ireg[s->nintback][0] = ireg0;
            s->intback_ireg[s->nintback][1] = ireg1;
            s->nintback++;
        }

        if (s->ib_active) {
            /* A new INTBACK while a peripheral phase is owed: serve it. */
            intback_peripheral(s);
            scu_raise(s, 7);
            break;
        }

        s->ib_p1md  = (uint8_t)((ireg1 >> 4) & 3u);
        s->ib_p2md  = (uint8_t)((ireg1 >> 6) & 3u);
        s->ib_active = (ireg1 & 0x08) != 0;
        s->ib_first  = 1;

        if (smpcdbg())
            printf("[smpc] INTBACK ireg0=%02X ireg1=%02X cy=%llu\n",
                   ireg0, ireg1, (unsigned long long)s->master.cycles);
        if (ireg0 & 0x01) {
            intback_status(s);
            /* Status SR follows the latched INTBACK request exactly: bit7 is
             * clear, PDL is set, NPE is set iff a peripheral phase follows,
             * and the low nibble is the two requested port modes.  For the
             * BIOS's 01/02 request both modes are zero; forcing 0x0F here
             * reports a different protocol mode and changes the IPL path. */
            s->smpc_reg[SR & 0x7F] = (uint8_t)(0x40
                                    | (s->ib_active ? 0x20 : 0x00)
                                    | (s->ib_p1md << 0)
                                    | (s->ib_p2md << 2));
        } else if (s->ib_active) {
            intback_peripheral(s);
        }
        scu_raise(s, 7);                    /* System Manager interrupt        */
        break;
    }

    case CMD_SSHON:
        /* SSHON releases the slave SH-2 from reset. Arm it HERE, at the moment
         * the host says go, using the entry/stack the master registered -- not
         * earlier, or we start it before its code has been loaded and it faults
         * on the first instruction. */
        /* Releasing a CPU that is ALREADY running must not restart it.
         * Fighting Vipers issues SSHON a second time (cy 787M) while its slave
         * is inside SGL's command-queue loop at 0x0605B7D0 -- a loop that by
         * design never returns, because the slave is meant to sit there
         * forever draining the master's ring buffer and posting progress to
         * GBR+68. Re-arming threw it back to its dispatcher entry, where the
         * one-shot handler slot at 0x0605ABF8 had already been consumed, so it
         * never ran another command and the master spun on the ack for the
         * rest of the run. Only (re)arm a slave that is off or faulted. */
        if (!s->slave_enabled || s->slave.halted) {
            s->slave_started = 0;  /* re-arm from the registered entry */
            s->slave.halted  = 0;
        }
        s->slave_enabled = 1;
        /* On the HLE path the master registers the entry through our BIOS
         * service stub. With the REAL BIOS there is no such call: the slave's
         * entry/stack pair lives at 0x06000250/0x06000254, exactly where the
         * BIOS's own slave trampoline reads it. Without this the slave stays at
         * PC=0 and retires nothing, and the game hangs in its master/slave
         * handshake (0x06032CCA: set a flag, wait for the slave to clear it). */
        /* Re-read the registered entry EVERY time, not just the first.
         * 0x06000250/0x06000254 is a live mailbox: a game that runs several
         * phases parks a different slave routine there before each SSHON.
         * Caching the first one meant an SSHOFF/SSHON pair -- which is how a
         * game legitimately restarts the slave for a new phase -- relaunched
         * the OLD routine. Fighting Vipers does exactly that at cy 787M, and
         * the stale restart is why its slave dropped out of the SGL command
         * loop and stopped answering the master. Keep the previous value only
         * when the mailbox holds nothing usable. */
        {
            uint32_t e = bus_r32(s, 0x06000250u);
            uint32_t k = bus_r32(s, 0x06000254u);
            if (e >= 0x06000000u && e < 0x06100000u) {
                s->slave_entry = e;
                s->slave_sp    = (k >= 0x06000000u && k < 0x06100000u)
                                 ? k : 0x0607C000u;
            }
        }
        /* SSHON is "enable AND RESET", every time -- Ymir
         * SMPCOperations::EnableAndResetSlaveSH2() is unconditionally
         * `slaveSH2Enabled = true; slaveSH2.Reset(true)`.
         *
         * We were reading the new entry out of the mailbox above and then
         * never applying it: run_slave only resets when !slave_started, so
         * after the first start every later SSHON recorded an entry and left
         * the slave running whatever it was already doing. A game that does
         * SSHOFF/SSHON to hand the slave a new routine therefore kept
         * executing the OLD one -- and once the game reclaimed that memory for
         * data, the slave was executing a data table. That is exactly how
         * NiGHTS' slave ends up in the 424-byte block at 0x0600F200.
         *
         * SATURN_NOSSHRESET restores the old resume-in-place behaviour for
         * bisecting. */
        {
            static int noreset = -1;
            if (noreset < 0) noreset = getenv("SATURN_NOSSHRESET") ? 1 : 0;
            if (!noreset) {
                s->slave_started = 0;      /* run_slave re-resets at the entry */
                s->slave.halted  = 0;
            }
        }
        printf("[sshon] cy=%llu slave entry=%08X sp=%08X\n",
               (unsigned long long)s->master.cycles,
               s->slave_entry, s->slave_sp);
        ow(s, 31, cmd);
        break;
    case CMD_SSHOFF:
        s->slave_enabled = 0;
        ow(s, 31, cmd);
        break;

    case CMD_CKCHG320:
    case CMD_CKCHG352:
        /* Ymir schedules CKCHG rather than completing it on the COMREG write.
         * This delay is architecturally significant: the BIOS writes COMREG
         * at 0x52C, executes SLEEP at 0x52E, then expects the eventual NMI to
         * enter its 0x534 completion tail. Raising NMI immediately skips that
         * state transition and breaks warm-boot/autoboot callers. */
        printf("[ckchg] cmd=%02X pc=%08X pr=%08X gbr=%08X cy=%llu\n",
               cmd, s->master.pc, s->master.pr, s->master.gbr,
               (unsigned long long)s->master.cycles);
        s->pending_ckchg = 1;
        s->ckchg_cmd = cmd;
        /* The public command scheduler already imposed the long CKCHG delay.
         * Complete the reset at this scheduler boundary. */
        s->ckchg_due = s->clk;
        ow(s, 31, cmd);
        /* SF is cleared only when smpc_tick completes the command. */
        return;

    case CMD_SETTIME:
        if (smpcdbg())
            printf("[smpc] SETTIME issued at cy=%llu -- clock committed%s",
                   (unsigned long long)s->master.cycles, "\n");
        /* Sets the RTC (we do not model wall time). It does NOT touch SMEM:
         * falling through overwrote the just-saved language selection with
         * clock bytes. Both commands mark settings as saved (STE). */
        s->smpc_ste = 1;
        smpc_persist_save(s);
        ow(s, 31, cmd);
        break;

    case CMD_SETSMEM:
        s->smpc_ste = 1;

        if (smpcdbg())
            printf("[smpc] SETSMEM %02X %02X %02X %02X\n",
                   s->smpc_reg[(IREG0 + 0) & 0x7F], s->smpc_reg[(IREG0 + 2) & 0x7F],
                   s->smpc_reg[(IREG0 + 4) & 0x7F], s->smpc_reg[(IREG0 + 6) & 0x7F]);
        for (int i = 0; i < 4; i++)
            s->smem[i] = s->smpc_reg[(IREG0 + i * 2) & 0x7F];
        smpc_persist_save(s);
        ow(s, 31, cmd);
        break;

    case CMD_SNDON:
        /* Release the sound 68000. Until this arrives sound RAM holds whatever
         * the host last DMA'd there, so running the core early executes
         * garbage. */
        sound_set_on(s, 1);
        ow(s, 31, cmd);
        break;

    case CMD_SNDOFF:
        sound_set_on(s, 0);
        ow(s, 31, cmd);
        break;

    case CMD_RESENAB:
        s->smpc_resd = 0;
        ow(s, 31, cmd);
        break;
    case CMD_RESDISA:
        s->smpc_resd = 1;
        ow(s, 31, cmd);
        break;

    case CMD_MSHON:
    case CMD_CDON:   case CMD_CDOFF:  case CMD_SYSRES:
    case CMD_NMIREQ:
    default:
        ow(s, 31, cmd);              /* echo the command, the usual ack */
        break;
    }

    /* Clearing SF is what tells the host the command finished. */
    s->smpc_reg[SF & 0x7F] = 0;
}

void smpc_command(saturn *s, uint8_t cmd)
{
    /* A COMREG write schedules completion.  Completing it inside bus_w8 races
     * software that writes COMREG and only then marks its queue as waiting. */
    if (s->pending_smpc_cmd || s->pending_ckchg == 1) {
        if (getenv("SATURN_SMPC_CMDS"))
            printf("[smpc-reject] cmd=%02X pending=%02X clk=%llu\n",
                   cmd, s->smpc_cmd, (unsigned long long)s->clk);
        return;
    }

    s->smpc_reg[COMREG & 0x7F] = cmd;
    s->pending_smpc_cmd = 1;
    s->smpc_cmd = cmd;
    /* SYSRES and clock changes are long commands.  Other commands use the
     * compatibility-tested 240-cycle event delay. */
    s->smpc_cmd_due = s->clk +
        ((cmd == CMD_SYSRES || cmd == CMD_CKCHG352 || cmd == CMD_CKCHG320)
         ? 200000u : 240u);
}

/* Complete a scheduled clock change at a scheduler boundary. This follows
 * Ymir SMPC::ClockChange: soft-reset VDP/SCU/SCSP, disable the slave SH-2,
 * raise NMI on the master, then select the new clock. A soft VDP reset keeps
 * VRAM, CRAM and framebuffers; only registers and drawing state are reset. */
void smpc_tick(saturn *s)
{
    uint16_t pal;

    if (s->pending_smpc_cmd && s->clk >= s->smpc_cmd_due) {
        uint8_t cmd = s->smpc_cmd;
        s->pending_smpc_cmd = 0;
        smpc_execute_command(s, cmd);
    }

    if (s->pending_ckchg != 1 || s->clk < s->ckchg_due) return;

    vdp1_soft_reset(s);
    pal = (uint16_t)(s->vdp2_reg[0x04 >> 1] & 1u);
    memset(s->vdp2_reg, 0, sizeof s->vdp2_reg);
    s->vdp2_reg[0x04 >> 1] = pal;
    s->vdp2_vram_epoch++;
    s->cram_epoch++;

    /* SCU soft reset preserves DMA/timer registers but resets the interrupt
     * controller and the DSP's execution state. */
    s->scu_ipend = 0;
    s->scu_reg[0xA0 >> 2] = 0x0000BFFFu;
    s->scu_reg[0xA4 >> 2] = 0;
    scu_dsp_soft_reset(s);

    sound_clock_change_reset(s);
    s->slave_enabled = 0;
    s->clock_352 = (s->ckchg_cmd == CMD_CKCHG352);

    s->smpc_reg[SF & 0x7F] = 0;
    s->pending_ckchg = 2;       /* sh2_step raises the NMI next boundary */
    if (getenv("SATURN_SMPC_CMDS"))
        printf("[ckchg-complete] cmd=%02X clk=%llu pc=%08X sleeping=%d\n",
               s->ckchg_cmd, (unsigned long long)s->clk,
               s->master.pc, s->master.sleeping);
}

/* ------------------------------------------------------- direct port I/O ----
 * With IOSEL set the host drives the controller ports itself instead of using
 * INTBACK: it writes TH/TR select bits to PDR1 and reads a nibble back each
 * time, walking the pad's four button groups. NiGHTS switches to this mode
 * once it is past its own init, so PDR has to answer or the pad read spins.
 *
 * Standard digital pad, all bits ACTIVE LOW:
 *   select 0x60 -> R, X, Y, Z
 *   select 0x40 -> Start, A, C, B
 *   select 0x20 -> Right, Left, Down, Up
 *   select 0x00 -> L, and the "pad present" marker
 */
uint8_t smpc_pdr_read(saturn *s, int port)
{
    uint8_t pdr = s->smpc_reg[(port ? 0x77 : 0x75) & 0x7F];
    uint8_t ddr = s->smpc_reg[(port ? 0x7B : 0x79) & 0x7F];
    uint8_t sel = (uint8_t)(pdr & 0x60);
    uint8_t lo, hi, v;

    pad_now(s, &lo, &hi);

    /* Match Ymir ControlPad::WritePDR exactly. The DDR chooses the protocol;
     * looking only at PDR accidentally treated TH-only reads as four-phase
     * TH/TR reads. A title polling in TH mode could therefore decode Start as
     * several face buttons too -- the standard A+B+C+Start soft-reset chord. */
    if (port != 0) return 0x00;          /* NullPeripheral::WritePDR */
    switch (ddr & 0x7Fu) {
    case 0x40:                           /* TH control mode */
        return (pdr & 0x40u) ? 0x74u : 0x35u;
    case 0x60:                           /* TH/TR control mode */
        break;
    default:
        return 0xFFu;
    }

    /* Ymir ControlPad::WritePDR is the authority here, and the order is NOT
     * the one the comment above described:
     *
     *   TH/TR = 0x60  1st data:  0x70 | L<<3 | 0b100
     *   TH/TR = 0x20  2nd data:  0x30 | right left down up
     *   TH/TR = 0x40  3rd data:  0x50 | start A C B
     *   TH/TR = 0x00  4th data:  0x10 | R X Y Z
     *
     * We had the 0x60 and 0x00 phases SWAPPED, and read Start/A/C/B out of the
     * `hi` byte when they live in `lo` bits 3-0. NiGHTS drives the pad through
     * this path rather than INTBACK, so Start and A/B/C never reached it and
     * the game could not be started at all.
     *
     * Note the high nibble: Ymir returns the select bits OR 0x10, not the bare
     * select bits, and the 1st-data phase has 0b100 in the low bits. Buttons
     * are ACTIVE LOW; lo/hi hold them active-high, hence the inversions. */
    switch (sel) {
    case 0x60: v = (uint8_t)(0x70u | ((uint8_t)(~hi >> 3) & 1u) << 3 | 0x04u); break;
    case 0x20: v = (uint8_t)(0x30u | (((uint8_t)~lo >> 4) & 0x0Fu)); break;
    case 0x40: v = (uint8_t)(0x50u | ( (uint8_t)~lo       & 0x0Fu)); break;
    default:   v = (uint8_t)(0x10u | (((uint8_t)~hi >> 4) & 0x0Fu)); break;
    }
    return v;
}

/* Persistent SMPC settings, modelled on Ymir's PersistentSMPCData
 * (smpc_defs.hpp: SMEM[4] + STE + rtc). A real Saturn keeps these in
 * battery-backed memory, which is the ONLY reason it shows the Set Language /
 * Set Time screen once instead of on every power-on. We had no persistence at
 * all, so every launch looked like a factory-fresh console and the screen came
 * back every time -- the flag then had to be forced with SATURN_OREG0=0x80,
 * which is a DIFFERENT boot path (the CD player route), not the normal one.
 *
 * Ymir writes state/smpc.bin next to its profile; we write one file per game
 * beside the game's own data so two titles cannot fight over one clock. */
static void smpc_persist_path(saturn *s, char *out, size_t n)
{
    const char *e = getenv("SATURN_SMPCFILE");
    if (e) { strncpy(out, e, n - 1); out[n - 1] = 0; return; }
    snprintf(out, n, "%s", s->smpc_state_path[0] ? s->smpc_state_path
                                                 : "smpc.bin");
}

void smpc_persist_load(saturn *s)
{
    char path[512];
    FILE *f;
    uint8_t buf[8];
    smpc_persist_path(s, path, sizeof path);
    f = fopen(path, "rb");
    if (!f) return;
    if (fread(buf, 1, sizeof buf, f) == sizeof buf && buf[0] == 'S') {
        s->smem[0] = buf[1]; s->smem[1] = buf[2];
        s->smem[2] = buf[3]; s->smem[3] = buf[4];
        s->smpc_ste = buf[5] ? 1 : 0;
    }
    fclose(f);
}

void smpc_persist_save(saturn *s)
{
    char path[512];
    FILE *f;
    uint8_t buf[8];
    smpc_persist_path(s, path, sizeof path);
    buf[0] = 'S';
    buf[1] = s->smem[0]; buf[2] = s->smem[1];
    buf[3] = s->smem[2]; buf[4] = s->smem[3];
    buf[5] = (uint8_t)(s->smpc_ste ? 1 : 0);
    buf[6] = buf[7] = 0;
    f = fopen(path, "wb");
    if (!f) return;
    fwrite(buf, 1, sizeof buf, f);
    fclose(f);
}

void smpc_reset(saturn *s)
{
    memset(s->smpc_reg, 0, sizeof(s->smpc_reg));
    s->smpc_reg[SF & 0x7F] = 0;
    s->pending_smpc_cmd = 0;
    s->pending_ckchg = 0;
    /* Hard-reset register state, matching Ymir's SMPC::Reset exactly.  Do
     * not synthesize a peripheral report here: doing so overwrote SR with
     * 0xC0 and OREG31 with 0xFF before the BIOS had requested INTBACK.  Real
     * reset state is SR=0x80 and OREG31=0xF0; the first report is produced
     * only after an INTBACK command. */
    s->smpc_reg[SR & 0x7F] = 0x80;
    ow(s, 31, 0xF0);
    s->ib_active = 0;
    s->ib_first = 0;
    s->ib_p1md = 0;
    s->ib_p2md = 0;
    s->pad1_lo = 0;
    s->pad1_hi = 0;
}
