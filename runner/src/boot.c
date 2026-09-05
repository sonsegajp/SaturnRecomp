/* boot.c — bring a Saturn title up under the interpreter.
 *
 * Reproduces what the IPL does: read IP.BIN, load the 1st-read file to the
 * address it declares, and jump there. No BIOS image is required or used.
 *
 * The point of this stage is not to render a frame -- it is to find out, from
 * the game's own code, exactly which hardware it touches and in what order.
 * That trace is the work list for the runner.
 */
#include "saturn.h"
#include "disc.h"
#include "game_config.h"
#include "../../external/sh2-recomp-core/common/sh2_isa.h"
#include <stdio.h>
#include <x86intrin.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#ifdef _WIN32
#include <windows.h>
#endif

void bios_hle_install(saturn *s);
int  bios_rom_load(saturn *s, const char *path, char *err, size_t errsz);

void bios_reset_vector(saturn *s, uint32_t *pc, uint32_t *sp);

static saturn g_sys;

static void crash_handler(int sig)
{
    saturn *s = &g_sys;
    fprintf(stderr, "\n*** CRASH (signal %d) ***\n"
            "master PC=%08X PR=%08X cy=%llu\n"
            "slave  PC=%08X halted=%d\n"
            "68k    PC=%06X halted=%d\n",
            sig, s->master.pc, s->master.pr,
            (unsigned long long)s->master.cycles,
            s->slave.pc, s->slave.halted,
            s->sound_cpu.pc, s->sound_cpu.halted);
    fflush(stderr);
    _exit(99);
}

static void load_into_wram(saturn *s, uint32_t addr, const uint8_t *data, size_t n)
{
    for (size_t i = 0; i < n; i++)
        bus_w8(s, addr + (uint32_t)i, data[i]);
}

static void dump_state(sh2 *c, FILE *out)
{
    fprintf(out, "  PC=%08X  PR=%08X  SR=%08X  GBR=%08X  VBR=%08X\n",
            c->pc, c->pr, c->sr, c->gbr, c->vbr);
    fprintf(out, "  MACH=%08X MACL=%08X  cycles=%llu\n",
            c->mach, c->macl, (unsigned long long)c->cycles);
    for (int i = 0; i < 16; i += 4)
        fprintf(out, "  R%-2d=%08X R%-2d=%08X R%-2d=%08X R%-2d=%08X\n",
                i, c->r[i], i+1, c->r[i+1], i+2, c->r[i+2], i+3, c->r[i+3]);
}

/* Disassemble a span of WRAM. Used to inspect what the BIOS builds in the
 * work area -- the interrupt trampolines in particular. */
static void dump_region(saturn *s, uint32_t lo, uint32_t hi, FILE *out)
{
    fprintf(out, "\n  region 0x%08X-0x%08X:\n", lo, hi);
    for (uint32_t a = lo; a < hi; a += 2) {
        uint16_t op = bus_r16(s, a);
        char txt[64];
        if (sh2_format(op, a, txt)) fprintf(out, "  %08X  %04X  %s\n", a, op, txt);
        else                        fprintf(out, "  %08X  %04X  .word\n", a, op);
    }
}

static void dump_around(saturn *s, uint32_t pc, FILE *out)
{
    fprintf(out, "  disassembly around PC:\n");
    for (int k = -4; k <= 4; k++) {
        uint32_t a = pc + (uint32_t)(k * 2);
        uint16_t op = bus_r16(s, a);
        char txt[64];
        const char *mark = (k == 0) ? "->" : "  ";
        if (sh2_format(op, a, txt))
            fprintf(out, "  %s %08X  %04X  %s\n", mark, a, op, txt);
        else
            fprintf(out, "  %s %08X  %04X  .word\n", mark, a, op);
    }
}

int main(int argc, char **argv)
{
    game_config g;
    disc d;
    iso_fs fs;
    saturn_ip ip;
    saturn *s = &g_sys;
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    uint64_t budget = 20000000;
    const char *toml;
    const iso_entry *first_read = NULL;
    void *image = NULL;
    size_t image_size = 0;
    uint32_t load_addr, entry, sp;
    int bios_boot = 0;
    int no_bios = 0;
    (void)bios_boot;

    if (argc < 2) {
        fprintf(stderr, "usage: saturnboot <games/<name>/game.toml> [max-instructions]\n");
        return 2;
    }
    toml = argv[1];
    if (argc > 2) budget = strtoull(argv[2], NULL, 0);
    for (int k = 1; k < argc; k++)
        if (!strcmp(argv[k], "nobios")) no_bios = 1;

    if (gc_load(&g, toml) != 0) { fprintf(stderr, "error: %s\n", g.err); return 1; }
    if (disc_open(&d, g.disc) != 0) { fprintf(stderr, "error: %s\n", d.err); return 1; }
    if (ip_read(&d, &ip) != 0) { fprintf(stderr, "error: not a Saturn disc\n"); return 1; }
    if (iso_read(&d, &fs) != 0) { fprintf(stderr, "error: %s\n", d.err); return 1; }

    printf("=== %s ===\n", g.name);
    printf("product      %s %s   area %s\n", ip.product_no, ip.version, ip.area);
    printf("1st-read     0x%08X\n", ip.first_read_addr);

    /* Find the module the config marks as the IPL-loaded one. */
    for (int i = 0; i < g.nmodules; i++) {
        if (!g.modules[i].is_first_read) continue;
        for (int j = 0; j < fs.nentries; j++)
            if (!fs.entries[j].is_dir &&
                strcmp(fs.entries[j].path, g.modules[i].file) == 0) {
                first_read = &fs.entries[j];
                break;
            }
        if (first_read) {
            load_addr = g.modules[i].load_addr ? g.modules[i].load_addr
                                               : ip.first_read_addr;
            entry     = g.modules[i].entry ? g.modules[i].entry : load_addr;
            break;
        }
    }
    if (!first_read) {
        fprintf(stderr, "error: no module marked first_read = true was found on the disc\n");
        return 1;
    }

    image = iso_extract(&d, first_read, &image_size);
    if (!image) { fprintf(stderr, "error: could not read %s\n", first_read->path); return 1; }
    printf("loading      %s (%zu bytes) -> 0x%08X\n",
           first_read->path, image_size, load_addr);

    if (getenv("SATURN_LSFILES")) {
        int n = 0;
        for (int i = 0; i < fs.nentries; i++) {
            if (fs.entries[i].is_dir) { printf("  [DIR] %s\n", fs.entries[i].path); continue; }
            printf("  file id %3d  FAD %6u  %8u B  %s\n", n + 2,
                   fs.entries[i].lba + 150u, (unsigned)fs.entries[i].size,
                   fs.entries[i].path);
            n++;
            if (n >= 200) break;
        }
    }
    saturn_init(s);
    /* saturn_init clears the whole machine, including the console-area latch.
     * Select the region afterwards so the BIOS sees the disc's real region. */
    s->area_code = getenv("SATURN_AREA")
                 ? (uint8_t)strtoul(getenv("SATURN_AREA"), NULL, 0)
                 : saturn_area_from_ip(ip.area);
    s->irq_clobber_reg = -1;
    cdb_init(s, &d, &fs);
    if (getenv("SATURN_DTRFILL")) s->dtr_fill = 1;
    if (getenv("SATURN_CDMAP")) s->cd_map = atoi(getenv("SATURN_CDMAP"));
    if (getenv("SATURN_CDDELAY")) s->cd.boot_delay = atoi(getenv("SATURN_CDDELAY"));
    if (getenv("SATURN_WWATCH"))
        s->wwatch_addr = (uint32_t)strtoul(getenv("SATURN_WWATCH"), NULL, 0);
    if (getenv("SATURN_RWATCH"))
        s->rwatch_addr = (uint32_t)strtoul(getenv("SATURN_RWATCH"), NULL, 0);
    if (getenv("SATURN_BAL_A"))
        s->bal_a = (uint32_t)strtoul(getenv("SATURN_BAL_A"), NULL, 0);
    if (getenv("SATURN_BAL_B"))
        s->bal_b = (uint32_t)strtoul(getenv("SATURN_BAL_B"), NULL, 0);
    frt_irq_init();
    if (getenv("SATURN_RING"))
        s->ring_trig_pc = (uint32_t)strtoul(getenv("SATURN_RING"), NULL, 0);
    /* Hold a pad button for the whole run. Saturn digital pad, byte 1:
     * b7 Right b6 Left b5 Down b4 Up b3 Start b2 A b1 C b0 B.
     * Start = 0x08. Lets us test whether the BIOS screen is waiting for
     * input rather than having rejected the disc. */
    /* SATURN_SMEM=aa,bb,cc,dd -- preload the SMPC's four battery-backed
     * settings bytes and mark them saved (STE), standing in for the NVRAM a
     * real console persists between boots. With valid contents the BIOS
     * skips first-boot setup the honest way, rather than via the OREG0
     * override that left SMEM blank and the machine inconsistent. */
    /* Persistent SMPC settings live beside the game's own game.toml, so each
     * title keeps its own clock/language exactly as a real console keeps one
     * in battery-backed memory. Loaded BEFORE the SATURN_SMEM override so an
     * explicit override still wins. Without this the BIOS shows Set Language
     * on every single launch. */
    {
        const char *slash = strrchr(toml, '/');
        const char *bslash = strrchr(toml, 92);   /* backslash */
        const char *cut = slash > bslash ? slash : bslash;
        size_t dirlen = cut ? (size_t)(cut - toml) + 1 : 0;
        if (dirlen && dirlen < sizeof(s->smpc_state_path) - 16) {
            memcpy(s->smpc_state_path, toml, dirlen);
            strcpy(s->smpc_state_path + dirlen, "smpc.bin");
        } else {
            strcpy(s->smpc_state_path, "smpc.bin");
        }
        smpc_persist_load(s);
    }

    if (getenv("SATURN_SMEM")) {
        unsigned a0, b0, c0, d0;
        if (sscanf(getenv("SATURN_SMEM"), "%x,%x,%x,%x", &a0, &b0, &c0, &d0) == 4) {
            s->smem[0] = (uint8_t)a0; s->smem[1] = (uint8_t)b0;
            s->smem[2] = (uint8_t)c0; s->smem[3] = (uint8_t)d0;
            s->smpc_ste = 1;
        }
    }
    /* SATURN_POKE=addr:value:cycle[,addr:value:cycle...] -- write a 32-bit
     * value into guest memory at a given master cycle. This is a HYPOTHESIS
     * TESTER, not a fix: if forcing one word makes a stuck title proceed, that
     * word is the missing piece and the real question becomes why the game
     * never wrote it. Cheaper and far more decisive than another round of
     * static analysis. */
    if (getenv("SATURN_POKE")) {
        char pb[256];
        char *tk;
        strncpy(pb, getenv("SATURN_POKE"), sizeof(pb) - 1);
        pb[sizeof(pb) - 1] = 0;
        tk = strtok(pb, ",");
        while (tk && s->npoke < 8) {
            unsigned long a = 0, v = 0; unsigned long long cy = 0, per = 0;
            int n = sscanf(tk, "%lx:%lx:%llu:%llu", &a, &v, &cy, &per);
            if (n >= 3) {
                s->poke[s->npoke].addr   = (uint32_t)a;
                s->poke[s->npoke].val    = (uint32_t)v;
                s->poke[s->npoke].cy     = cy;
                s->poke[s->npoke].period = (n >= 4) ? per : 0;
                s->npoke++;
            }
            tk = strtok(NULL, ",");
        }
    }

    smpc_padseq_load_env(s);
    if (getenv("SATURN_PADAT"))
        s->pad_at = strtoull(getenv("SATURN_PADAT"), NULL, 0);
    if (getenv("SATURN_PAD"))
        s->pad1_lo = (uint8_t)strtoul(getenv("SATURN_PAD"), NULL, 0);
    if (getenv("SATURN_RING_ANY")) s->ring_any = 1;
    if (getenv("SATURN_RING_PR"))
        s->ring_trig_pr = (uint32_t)strtoul(getenv("SATURN_RING_PR"), NULL, 0);
    if (getenv("SATURN_SNAP"))
        s->snap_addr = (uint32_t)strtoul(getenv("SATURN_SNAP"), NULL, 0);
    if (getenv("SATURN_RING_SKIP"))
        s->ring_skip = strtoull(getenv("SATURN_RING_SKIP"), NULL, 0);
    if (getenv("SATURN_REGAT"))
        s->regat_pc = (uint32_t)strtoul(getenv("SATURN_REGAT"), NULL, 0);
    if (getenv("SATURN_WRANGE")) {
        char wb[64]; strncpy(wb, getenv("SATURN_WRANGE"), sizeof(wb)-1);
        wb[sizeof(wb)-1] = 0;
        /* Base 16, NOT strtoul's base-0 autodetect: a natural-looking
         * "05F80028" has a leading zero, so base 0 reads it as OCTAL, stops at
         * the 'F' and yields 5. The watch then silently covered address 5 and
         * reported nothing, which reads exactly like "the game never writes
         * this register". 0x-prefixed values still parse fine in base 16. */
        char *dash = strchr(wb, '-');
        if (dash) { *dash = 0; s->wrange_hi = (uint32_t)strtoul(dash+1, NULL, 16); }
        s->wrange_lo = (uint32_t)strtoul(wb, NULL, 16);
    }
    if (getenv("SATURN_RRANGE")) {
        char rb[64]; strncpy(rb, getenv("SATURN_RRANGE"), sizeof(rb)-1);
        rb[sizeof(rb)-1] = 0;
        char *dash = strchr(rb, '-');
        if (dash) { *dash = 0; s->rrange_hi = (uint32_t)strtoul(dash+1, NULL, 16); }
        s->rrange_lo = (uint32_t)strtoul(rb, NULL, 16);
    }
    /* Optional producer/consumer disambiguation for a watched read range.
     * SDK bookkeeping often touches every byte before title code consumes the
     * buffer, filling the bounded log with irrelevant memcpy PCs. */
    if (getenv("SATURN_RRPC")) {
        char rb[64]; strncpy(rb, getenv("SATURN_RRPC"), sizeof(rb)-1);
        rb[sizeof(rb)-1] = 0;
        char *dash = strchr(rb, '-');
        if (dash) { *dash = 0; s->rrpc_hi = (uint32_t)strtoul(dash+1, NULL, 16); }
        s->rrpc_lo = (uint32_t)strtoul(rb, NULL, 16);
    }
    if (getenv("SATURN_PCLAST"))
        s->pclast_pc = (uint32_t)strtoul(getenv("SATURN_PCLAST"), NULL, 0);
    if (getenv("SATURN_DUMPAT"))
        s->dumpat_pc = (uint32_t)strtoul(getenv("SATURN_DUMPAT"), NULL, 0);
    s->dumpat_n = getenv("SATURN_DUMPAT_N") ? atoi(getenv("SATURN_DUMPAT_N")) : 1;
    if (s->dumpat_n < 1) s->dumpat_n = 1;
    if (getenv("SATURN_WATCH"))
        s->watch_pc = (uint32_t)strtoul(getenv("SATURN_WATCH"), NULL, 0);

    /* Authentic path: run the real IPL from its reset vector. The BIOS then
     * builds the vector table and BIOS call table itself, which is exactly the
     * thing our stubs could only approximate. */
    if (g.bios[0] && !no_bios) {
        char err[256];
        if (bios_rom_load(s, g.bios, err, sizeof(err)) != 0) {
            fprintf(stderr, "error: %s\n", err);
            return 1;
        }
        bios_reset_vector(s, &entry, &sp);
        printf("BIOS         %s\n", g.bios);
        printf("reset vector PC 0x%08X   SP 0x%08X\n", entry, sp);
        bios_boot = 1;
    } else {
        bios_hle_install(s);
        sp = ip.stack_m ? ip.stack_m : 0x06100000u;
        printf("BIOS         (none configured - using HLE stubs)\n");
    }

    /* On the HLE path we still stand in for the IPL and place the 1st-read
     * image ourselves. On the real-BIOS path we must NOT: the IPL fetches it
     * over the CD block, and pre-loading 447KB at 0x06004000 scribbles over
     * the work area the IPL is using for its own code and literals. */
    if (!bios_boot)
        load_into_wram(s, load_addr, (const uint8_t *)image, image_size);
    else {
        printf("1st-read     left to the IPL (CD block serves it)\n");
        /* SATURN_PROTECT: guard the 1st-read image from the BIOS's own WRAM
         * fill loop on the REAL-BIOS path too.
         *
         * prot_lo/prot_hi were only ever armed on the SATURN_HANDOFF path, so
         * on a real-BIOS boot prot_block() returned 0 immediately and the
         * guard did nothing. Measured on NiGHTS: the fill at 0x000002B0 runs a
         * SECOND time at clk 160,872,644 -- long after the game owns the
         * machine -- and zeroes 0x0600F200 onward, which is live game code.
         * The game then calls 0x0600F208 (a legitimate callback it enqueues
         * itself) and executes the hole. Opt-in because blocking BIOS writes
         * wholesale has broken other titles before; see prot_block(). */
        if (getenv("SATURN_PROTECT")) {
            s->prot_lo = load_addr & 0x0FFFFFFFu;
            s->prot_hi = s->prot_lo + (uint32_t)image_size;
            printf("protect      0x%08X-0x%08X from the BIOS fill loop\n",
                   s->prot_lo, s->prot_hi);
        }
    }

    sh2_reset(&s->master, s, 0, entry, sp);
    if (!bios_boot) {
        /* The real IPL hands control to the game with interrupts ENABLED; the
         * SH-2 reset value of SR.I = 15 masks everything. Leaving it at reset
         * on the HLE path means the game's CD driver never receives its
         * completion interrupt and waits forever on a request it did issue. */
        /* The IPL also leaves the SCU with V-Blank enabled. The game never
         * writes the SCU mask itself (measured: zero accesses to 0x05FE0000),
         * so whatever we initialise it to is what it runs with -- and masking
         * V-Blank there starves its main loop. */
        s->scu_reg[0xA0 >> 2] &= ~0x3u;
        const char *m = getenv("SATURN_IMASK");
        unsigned lvl = m ? (unsigned)strtoul(m, NULL, 0) : 0u;
        s->master.sr = (s->master.sr & ~SR_I) | ((lvl & 0xF) << 4);
    }

    /* Hybrid boot: let the REAL BIOS run from reset until it has finished
     * building its work area -- vector table, service-pointer table, CD driver
     * -- then take over as the IPL: load the first-read program and jump to it.
     * The game's BIOS service calls then land on genuine BIOS routines instead
     * of our stubs (it calls slot 0x0600026C, the "HCDM" CD service, which we
     * only ever stubbed out). */
    if (bios_boot && getenv("SATURN_HANDOFF")) {
        uint32_t stop = (uint32_t)strtoul(getenv("SATURN_HANDOFF"), NULL, 0);
        uint64_t guard = 0;
        sh2 *c = &s->master;
        /* Same scheduler as the game runs under: the BIOS spins on TVSTAT
         * H-Blank in its clock-change tail, so the run-up to the handoff needs
         * scanline timing as much as the game does. */
        while (guard < (uint64_t)stop && !c->halted)
            guard += saturn_run_field(s);
        printf("handoff      BIOS at 0x%08X after %llu instructions\n",
               c->pc, (unsigned long long)guard);
        load_into_wram(s, load_addr, (const uint8_t *)image, image_size);
        /* Hand control to the GAME. `entry` still holds the BIOS reset
         * vector at this point, so jumping to it restarted the whole
         * BIOS -- which cleared WRAM straight back over the image we
         * just loaded (measured: 0x06004000 read back as zero) and then
         * booted the CD player again. */
        printf("handoff load  0x%08X = %08X  (0x06060000 = %08X)\n",
               load_addr, bus_r32(s, load_addr), bus_r32(s, 0x06060000u));
        /* Opt-in: blocking the BIOS's second CD-player copy stops the
         * image being overwritten, but the BIOS routes the game's own
         * service calls through structures that assume the CD player is
         * resident, so the machine ends at PC=0x00000002 either way. */
        if (!getenv("SATURN_NOPROTECT")) {
            s->prot_lo = load_addr & 0x0FFFFFFFu;
            s->prot_hi = s->prot_lo + (uint32_t)image_size;
        }
        entry = load_addr;
        sp    = ip.stack_m ? ip.stack_m : 0x06100000u;
        /* Take over completely, the way the IPL does. Leaving the BIOS's own
         * handlers installed means its main loop resumes through RTE on the
         * next V-Blank and re-fills WRAM straight over the game we just
         * loaded (measured: the fill at 0x000002B0 ran twice). Clear the user
         * vector table and mask interrupts; the game installs its own handlers
         * and re-enables them during init. */
        /* Neutralise the BIOS's own handlers without breaking dispatch. A null
         * user-vector entry faults (the trampolines jump through it
         * unconditionally), and leaving the BIOS handlers in place lets its
         * main loop resume through RTE and re-fill WRAM straight over the game
         * (measured: the fill at 0x000002B0 ran twice). Point every entry at a
         * bare `rts` instead; the game overwrites the ones it wants. */
        /* Stubbing the user vectors was only ever needed because the handoff
         * jumped back to the BIOS reset vector and its main loop resumed. Now
         * that we jump to the game, the BIOS handlers are exactly what the
         * game expects to still be there. */
        /* The BIOS dispatcher at 0x06000924 calls callback[vector], read from
         * *(0x06000900 + vector*4) -- so vector 0x40 (V-Blank IN) lives at
         * 0x06000A00. Those slots still point into the CD player the BIOS just
         * loaded, and we have now loaded the game straight over it, so the
         * first V-Blank calls a stale address and dies in a data table
         * (measured: illegal slot at 0x0600CFDA via 0x06028E20).
         *
         * Stub only the SCU range 0x40..0x5F. The previous code stubbed 128
         * entries from 0x06000A00, which ran off the end of the table into
         * other BIOS state and wedged the sleep/wake path instead. The stub
         * sits at 0x06000F00, below the 0x06004000 load address, so the game
         * image does not overwrite it. */
        if (!getenv("SATURN_NOSTUBVEC")) {
            const uint32_t stub = 0x06000F00u;
            bus_w16(s, stub + 0, 0x000B);   /* rts */
            bus_w16(s, stub + 2, 0x0009);   /* nop */
            for (uint32_t v = 0x40; v <= 0x5F; v++)
                bus_w32(s, 0x06000900u + v * 4u, stub);
        }
        s->scu_ipend = 0;
        /* Hand over the way the IPL does: V-Blank ENABLED and SR.I low. The
         * game's first act is a wait-for-change loop on its V-Blank counter
         * (0x060402E4: mov.l @(576,gbr),r0 / cmp/eq / bt), so handing over with
         * interrupts masked deadlocks it immediately. */
        s->scu_reg[0xA0 >> 2] = 0xFFFFFFFFu & ~0x3u;
        c->sr &= ~SR_I;
        c->pc = entry;
        c->r[15] = sp;
    }

    printf("entry        0x%08X   sp 0x%08X\n", entry, sp);
    printf("running      up to %llu instructions\n\n", (unsigned long long)budget);

    /* SATURN_PNGLIVE: keep the last field rendered while TVMD's DISP bit was
     * actually set. The end-of-run capture renders whatever registers survive
     * the budget cut-off, and boot code turns the display off and reprograms
     * VDP2 once it has finished drawing -- so the shot you get is of the
     * teardown, not of the screen. Same rule as every other diagnostic here:
     * snapshot at the moment, never at the end. */
    static uint32_t live_fr[704 * 512];
    int live_w = 0, live_h = 0;
    const int want_live = getenv("SATURN_PNGLIVE") != NULL;
    /* SATURN_PNGAFTER=N: only fields >= N compete for the live capture --
     * lets a late screen (CD player) win over a busier early one (logo). */
    const long long live_after = getenv("SATURN_PNGAFTER") ? atoll(getenv("SATURN_PNGAFTER")) : 0;
    uint64_t live_field = 0;
    int live_score = -1;
    static uint16_t live_reg[256];
    static uint16_t live_fbs[6];
    static uint8_t  live_cram2[256];   /* CRAM bytes 0x400.. for palette-bank dumps */
    static uint16_t live_cram[16];

    {
        /* Scanline-driven: saturn_run_field walks LINES_TOTAL scanlines, raising
         * V-Blank in/out on the lines they occur on and interleaving master and
         * slave within each line. Sprite Draw End is not raised here -- VDP1
         * asserts it when it finishes a command list (vdp1.c does that) --
         * and Timer 0 only fires when the SCU timer is actually unmasked. */
        uint64_t retired = 0;
        sh2 *c = &s->master;

        int noirq = getenv("SATURN_NOIRQ") != NULL;
        const unsigned throttle_ms = getenv("SATURN_THROTTLE_MS")
                                   ? (unsigned)strtoul(getenv("SATURN_THROTTLE_MS"), NULL, 0)
                                   : 0u;
        while (retired < budget && !c->halted) {
            if (noirq) {
                /* Bisect: run the same boot with no interrupt sources at all.
                 * If a hang survives this, interrupts are not involved. */
                cdb_tick(s);
                s->cur = c;
                retired += sh2_run(c, CYC_PER_LINE * LINES_TOTAL);
                /* Still advance the machine clock: TVSTAT H-Blank comes off it,
                 * and a bisect that also freezes the raster is testing two
                 * changes at once. */
                s->clk += CYC_PER_LINE * LINES_TOTAL;
                continue;
            }
            retired += saturn_run_field(s);
#ifdef _WIN32
            /* Interactive headless validation needs time to inspect a PNG and
             * issue the next SATURN_PADFILE edge.  Unthrottled execution can
             * consume an entire title/menu window during one host-tool round
             * trip, recreating the flaky absolute-cycle automation this hook
             * was designed to replace.  Opt in; benchmark runs stay fast. */
            if (throttle_ms) Sleep(throttle_ms);
#else
            (void)throttle_ms;
#endif
            /* SATURN_POKEAT=cy:addr:val -- one 32-bit write once the master
             * clock passes cy. Lets a hypothesis about a state variable be
             * tested directly ("if the player wrote 2 here, would the BIOS
             * boot the disc?") without first finding the UI path to it. */
            {
                static int poked = 0;
                const char *pk = getenv("SATURN_POKEAT");
                const char *pkb = getenv("SATURN_POKEB");
                static int pokedb = 0;
                if (pkb && !pokedb) {
                    uint64_t pcy; uint32_t pa, pv;
                    if (sscanf(pkb, "%llu:%x:%x", (unsigned long long *)&pcy, &pa, &pv) == 3 &&
                        s->master.cycles >= pcy) {
                        bus_w8(s, pa, (uint8_t)pv);
                        pokedb = 1;
                    }
                }
                if (pk && !poked) {
                    uint64_t pcy; uint32_t pa, pv;
                    if (sscanf(pk, "%llu:%x:%x", (unsigned long long *)&pcy, &pa, &pv) == 3 &&
                        s->master.cycles >= pcy) {
                        bus_w32(s, pa, pv);
                        poked = 1;
                        printf("poked       0x%08X = 0x%08X at cy %llu\n", pa, pv,
                               (unsigned long long)s->master.cycles);
                    }
                }
            }
            /* SATURN_SHOTS=prefix:interval -- write a PNG of the composited
             * screen every `interval` master cycles. One run then yields a
             * filmstrip, which is the only affordable way to hunt for the
             * cycle at which a scripted input sequence reaches gameplay when a
             * single run costs minutes. */
            {
                static const char *shot_pfx = NULL;
                static unsigned long long shot_ival = 0, shot_next = 0;
                static int shot_init = 0;
                if (!shot_init) {
                    const char *e = getenv("SATURN_SHOTS");
                    shot_init = 1;
                    if (e) {
                        static char pb[192];
                        const char *col = strrchr(e, ':');
                        if (col) {
                            size_t n = (size_t)(col - e);
                            if (n > sizeof(pb) - 1) n = sizeof(pb) - 1;
                            memcpy(pb, e, n); pb[n] = 0;
                            shot_pfx  = pb;
                            shot_ival = strtoull(col + 1, NULL, 0);
                            shot_next = shot_ival;
                        }
                    }
                }
                if (shot_pfx && shot_ival && s->master.cycles >= shot_next) {
                    static uint32_t sfr[704 * 512];
                    char path[256];
                    int sw, sh;
                    vdp2_display_size(s, &sw, &sh);
                    if (sw < 1 || sw > 704) sw = 320;
                    if (sh < 1 || sh > 512) sh = 224;
                    vdp2_render(s, sfr, sw, sh, 1);
                    snprintf(path, sizeof path, "%s%llu.png", shot_pfx,
                             (unsigned long long)shot_next);
                    if (png_write(path, sfr, sw, sh) == 0)
                        printf("[shot] %s (%dx%d) frame %llu\n", path, sw, sh,
                               (unsigned long long)s->frames);
                    fflush(stdout);
                    while (shot_next <= s->master.cycles) shot_next += shot_ival;
                }
            }
            /* SATURN_FRAME_SHOTS=prefix -- capture every completed emulated
             * field, indexed exactly like Ymir's RunFrame reference driver.
             * This is intentionally separate from SATURN_SHOTS: cycle-spaced
             * samples can skip the short IPL shard animation and cannot be
             * compared frame-for-frame across emulators. */
            {
                const char *frame_pfx = getenv("SATURN_FRAME_SHOTS");
                if (frame_pfx) {
                    static uint32_t ffr[704 * 512];
                    char path[256];
                    int fw, fh;
                    vdp2_display_size(s, &fw, &fh);
                    if (fw < 1 || fw > 704) fw = 320;
                    if (fh < 1 || fh > 512) fh = 224;
                    vdp2_render(s, ffr, fw, fh, 1);
                    snprintf(path, sizeof path, "%s%04llu.png", frame_pfx,
                             (unsigned long long)s->frames);
                    png_write(path, ffr, fw, fh);
                }
            }
            /* SATURN_LAYERDUMP=field with SATURN_LAYERDUMP_OUT=prefix writes
             * all six VDP2 inputs from one completed field.  Rendering them
             * from the same machine state is essential: replaying the title
             * once per layer can land on different command lists and makes a
             * compositing fault look like a rasterisation fault. */
            {
                static long long layer_field = -2;
                static int layer_done;
                if (layer_field == -2) {
                    const char *e = getenv("SATURN_LAYERDUMP");
                    layer_field = e ? atoll(e) : -1;
                }
                if (!layer_done && layer_field >= 0 &&
                    (long long)s->frames >= layer_field) {
                    static const char *name[6] = {
                        "nbg0", "nbg1", "nbg2", "nbg3", "rbg0", "sprites"
                    };
                    static uint32_t lfr[704 * 512];
                    const char *prefix = getenv("SATURN_LAYERDUMP_OUT");
                    unsigned saved_mask = s->layer_mask;
                    int saved_lock = s->layer_lock;
                    int lw, lh;
                    char path[320];

                    if (!prefix || !*prefix) prefix = "layerdump";
                    /* Paired with tools/render_state: preserve the exact inputs
                     * so renderer changes can be compared without replaying CPUs. */
                    if (getenv("SATURN_LAYERSTATE")) {
                        FILE *state_file = fopen(getenv("SATURN_LAYERSTATE"), "wb");
                        if (state_file) {
                            if (fwrite(s, sizeof(*s), 1, state_file) != 1)
                                fprintf(stderr, "failed to write layer state\n");
                            fclose(state_file);
                        }
                    }
                    vdp2_display_size(s, &lw, &lh);
                    if (lw < 1 || lw > 704) lw = 320;
                    if (lh < 1 || lh > 512) lh = 224;
                    s->layer_lock = 1;
                    printf("[layerdump] VDP2 SCRCTL=%04X VCSTA=%04X:%04X "
                           "CYCA0=%04X:%04X CYCA1=%04X:%04X "
                           "CYCB0=%04X:%04X CYCB1=%04X:%04X\n",
                           s->vdp2_reg[0x9A >> 1], s->vdp2_reg[0x9C >> 1],
                           s->vdp2_reg[0x9E >> 1], s->vdp2_reg[0x10 >> 1],
                           s->vdp2_reg[0x12 >> 1], s->vdp2_reg[0x14 >> 1],
                           s->vdp2_reg[0x16 >> 1], s->vdp2_reg[0x18 >> 1],
                           s->vdp2_reg[0x1A >> 1], s->vdp2_reg[0x1C >> 1],
                           s->vdp2_reg[0x1E >> 1]);
                    printf("[layerdump] BGON=%04X CHCTLA=%04X PNCN0=%04X "
                           "PLSZ=%04X MPOFN=%04X PRINA=%04X SPCTL=%04X "
                           "CCCTL=%04X\n",
                           s->vdp2_reg[0x20 >> 1], s->vdp2_reg[0x28 >> 1],
                           s->vdp2_reg[0x30 >> 1], s->vdp2_reg[0x3A >> 1],
                           s->vdp2_reg[0x3C >> 1], s->vdp2_reg[0xF8 >> 1],
                           s->vdp2_reg[0xE0 >> 1], s->vdp2_reg[0xEC >> 1]);
                    printf("[layerdump] WCTLA=%04X WCTLB=%04X WCTLC=%04X "
                           "WCTLD=%04X W0=%04X,%04X-%04X,%04X "
                           "W1=%04X,%04X-%04X,%04X\n",
                           s->vdp2_reg[0xD0 >> 1], s->vdp2_reg[0xD2 >> 1],
                           s->vdp2_reg[0xD4 >> 1], s->vdp2_reg[0xD6 >> 1],
                           s->vdp2_reg[0xC0 >> 1], s->vdp2_reg[0xC2 >> 1],
                           s->vdp2_reg[0xC4 >> 1], s->vdp2_reg[0xC6 >> 1],
                           s->vdp2_reg[0xC8 >> 1], s->vdp2_reg[0xCA >> 1],
                           s->vdp2_reg[0xCC >> 1], s->vdp2_reg[0xCE >> 1]);
                    printf("[layerdump] LWTA0=%04X:%04X LWTA1=%04X:%04X "
                           "VDP1 TVMR=%04X FBCR=%04X PTMR=%04X "
                           "draw=%u display=%u\n",
                           s->vdp2_reg[0xD8 >> 1], s->vdp2_reg[0xDA >> 1],
                           s->vdp2_reg[0xDC >> 1], s->vdp2_reg[0xDE >> 1],
                           s->vdp1_reg[0], s->vdp1_reg[1], s->vdp1_reg[2],
                           (unsigned)s->fb_draw, (unsigned)(s->fb_draw ^ 1));
                    for (int layer = 0; layer < 6; layer++) {
                        s->layer_mask = 1u << layer;
                        vdp2_render(s, lfr, lw, lh, 1);
                        snprintf(path, sizeof path, "%s.%s.png", prefix, name[layer]);
                        if (png_write(path, lfr, lw, lh) == 0)
                            printf("[layerdump] field %llu %s\n",
                                   (unsigned long long)s->frames, path);
                    }
                    s->layer_mask = 0x3Fu;
                    vdp2_render(s, lfr, lw, lh, 1);
                    snprintf(path, sizeof path, "%s.full.png", prefix);
                    if (png_write(path, lfr, lw, lh) == 0)
                        printf("[layerdump] field %llu %s\n",
                               (unsigned long long)s->frames, path);
                    s->layer_mask = saved_mask;
                    s->layer_lock = saved_lock;
                    layer_done = 1;
                    fflush(stdout);
                }
            }
            if (want_live) {
                int lw, lh;
                vdp2_display_size(s, &lw, &lh);
                if (lw < 1) lw = 320;
                if (lh < 1) lh = 224;
                if (lw > 704) lw = 704;
                if (lh > 512) lh = 512;
                /* Keep the field that actually draws the most, rather than
                 * the one where DISP happens to be set: this BIOS never sets
                 * DISP at all, so gating on it captured nothing and fell back
                 * to the end state -- the very thing this exists to avoid.
                 * Scoring by non-back pixels needs no guess about which field
                 * to look at. */
                {
                    static uint32_t tmp[704 * 512];
                    uint32_t back;
                    int score = 0;
                    vdp2_render(s, tmp, lw, lh, 1);
                    back = tmp[0];
                    for (int q = 0; q < lw * lh; q++)
                        if (tmp[q] != back) score++;
                    if (score > live_score && (long long)s->frames >= live_after) {
                        live_score = score;
                        memcpy(live_fr, tmp, (size_t)lw * lh * sizeof tmp[0]);
                        live_w = lw; live_h = lh; live_field = s->frames;
                /* The registers AS THEY WERE for this frame. Reading them
                 * after the run reports the teardown configuration. */
                memcpy(live_reg, s->vdp2_reg, sizeof live_reg);
                {   /* framebuffer probes: the word under these screen points
                     * at the captured moment. */
                    static const int fpx[6][2] = {{60,40},{300,60},{160,10},
                                                  {20,200},{300,200},{160,112}};
                    const uint8_t *fbp = s->vdp1_fb[s->fb_draw ^ 1];
                    for (int q = 0; q < 6; q++) {
                        uint32_t o = (uint32_t)(fpx[q][1] * 512 + fpx[q][0]) * 2u;
                        live_fbs[q] = (uint16_t)((fbp[o] << 8) | fbp[o + 1]);
                    }
                }
                for (int q = 0; q < 16; q++)
                    live_cram[q] = (uint16_t)((s->cram[q*2] << 8) | s->cram[q*2+1]);
                memcpy(live_cram2, s->cram + 0x400, 256);
                    }
                }
            }
        }

        /* SATURN_FINDL=hexvalue: scan WRAM-H for a big-endian long. Finds the
         * literal pools that reference an address, i.e. the code that touches
         * it. */
        if (getenv("SATURN_FINDL")) {
            uint32_t want = (uint32_t)strtoul(getenv("SATURN_FINDL"), NULL, 0);
            int hits = 0;
            for (uint32_t o = 0; o + 3 < WRAM_H_SIZE && hits < 24; o += 2) {
                uint32_t v = ((uint32_t)s->wram_h[o] << 24) |
                             ((uint32_t)s->wram_h[o+1] << 16) |
                             ((uint32_t)s->wram_h[o+2] << 8) | s->wram_h[o+3];
                if (v == want) {
                    printf("findl: %08X at 0x06%06X\n", want, o);
                    hits++;
                }
            }
        }
        if (getenv("SATURN_PROF")) {
            printf("fastpath %llu  slowpath %llu (%.1f%% fast)\n",
                   (unsigned long long)s->fastpath_hits,
                   (unsigned long long)s->slowpath_hits,
                   100.0 * s->fastpath_hits /
                   (double)(s->fastpath_hits + s->slowpath_hits + 1));
            double tot = (double)(s->prof_master + s->prof_slave +
                                  s->prof_video + s->prof_other);
            if (tot < 1) tot = 1;
            printf("prof: master %.1f%% slave %.1f%% video %.1f%% sound/cd %.1f%% "
                   "(ticks %llu/%llu/%llu/%llu)\n",
                   100.0 * s->prof_master / tot, 100.0 * s->prof_slave / tot,
                   100.0 * s->prof_video / tot, 100.0 * s->prof_other / tot,
                   (unsigned long long)s->prof_master,
                   (unsigned long long)s->prof_slave,
                   (unsigned long long)s->prof_video,
                   (unsigned long long)s->prof_other);
        }
        if (getenv("SATURN_RENDERBENCH")) {
            static uint32_t bfr[704 * 512];
            int bw, bh, it = atoi(getenv("SATURN_RENDERBENCH"));
            uint64_t t0;
            vdp2_display_size(s, &bw, &bh);
            if (bw < 1) { bw = 320; bh = 224; }
            t0 = __rdtsc();
            for (int q = 0; q < it; q++) vdp2_render(s, bfr, bw, bh, 1);
            printf("renderbench: %d frames, %llu ticks/frame (%dx%d)\n",
                   it, (unsigned long long)((__rdtsc() - t0) / (uint32_t)(it ? it : 1)), bw, bh);
        }
        printf("  cd: boot_delay=%d first_cmd=%d processing=%d peri_count=%llu "
               "last_peri=%llu status=%02X init_busy=%d CR1=%04X HIRQ=%04X\n",
               s->cd.boot_delay, s->cd.first_cmd_seen,
               s->cd.processing_cmd,
               (unsigned long long)s->cd.periodic_count,
               (unsigned long long)s->cd.last_peri_cy,
               s->cd.status,
               s->cd.init_busy, s->cdb_reg[0x18 >> 1], s->cdb_reg[0x08 >> 1]);
        printf("  scu ipend=%08X mask=%08X\n", s->scu_ipend,
               s->scu_reg[0xA0 >> 2]);
        for (int v = 0; v < 32; v++)
            if (s->scu_raise_hist[v])
                printf("  scu_raise(%d) called %llu times\n", v,
                       (unsigned long long)s->scu_raise_hist[v]);
        for (int v = 0; v < 128; v++)
            if (s->irqvec_hist[v])
                printf("  vector %02X taken %llu times\n", v,
                       (unsigned long long)s->irqvec_hist[v]);
        printf("retired      %llu instructions, %llu interrupts taken, "
               "%llu DMA transfers (%llu bytes)\n",
               (unsigned long long)retired, (unsigned long long)s->irqs_taken,
               (unsigned long long)s->dma_transfers,
               (unsigned long long)s->dma_bytes);
        printf("SCU DMA      %llu transfers, %llu bytes\n",
               (unsigned long long)s->scu_dma_transfers,
               (unsigned long long)s->scu_dma_bytes);
        printf("VDP1         %llu lists, %llu commands, %llu pixels drawn\n",
               (unsigned long long)s->vdp1_lists,
               (unsigned long long)s->vdp1_commands,
               (unsigned long long)s->vdp1_pixels);
        printf("lowest SR.I seen = %u at PC 0x%08X\n", s->min_imask, s->min_imask_pc);
        {
            int idx[12]; int nidx = 0;
            for (int k = 0; k < HOTPC_SLOTS; k++) {
                if (!s->hotpc[k].n) continue;
                int at = nidx;
                while (at > 0 && s->hotpc[idx[at-1]].n < s->hotpc[k].n) at--;
                if (at < 12) {
                    for (int m = (nidx < 12 ? nidx : 11); m > at; m--) idx[m] = idx[m-1];
                    idx[at] = k; if (nidx < 12) nidx++;
                }
            }
            printf("\nhottest PCs:\n");
            for (int k = 0; k < nidx; k++) {
                uint32_t a = s->hotpc[idx[k]].pc; char t[64];
                uint16_t op = bus_r16(s, a);
                if (!sh2_format(op, a, t)) snprintf(t, sizeof(t), ".word");
                printf("  %08X  %10llu  %s\n", a,
                       (unsigned long long)s->hotpc[idx[k]].n, t);
            }
        }
        printf("\nSR loads (PC -> installed I mask, count):\n");
        for (int k = 0; k < s->nldcsr; k++)
            printf("  %08X  I=%-2u  %llu\n", s->ldcsr[k].pc,
                   s->ldcsr[k].imask, (unsigned long long)s->ldcsr[k].n);
        printf("last SR.I=0 at PC 0x%08X (cycle %llu); re-masked at PC 0x%08X; %u maskings\n",
               s->last_open_pc, (unsigned long long)s->last_open_cy,
               s->first_mask_pc, s->nmaskings);
        printf("SCU int mask 0x%08X   pending 0x%08X   SR.I=%u\n",
               s->scu_reg[0xA0 >> 2], s->scu_ipend,
               (unsigned)((c->sr >> 4) & 0xF));
        printf("interrupt vectors (VBR=0x%08X); 0x00000200 = our unhandled-rte stub\n",
               c->vbr);
        {
            static const struct { uint8_t v; const char *name; } vs[] = {
                { 0x40, "V-Blank-IN"  }, { 0x41, "V-Blank-OUT" },
                { 0x42, "H-Blank-IN"  }, { 0x43, "Timer 0"     },
                { 0x46, "Sound Req"   }, { 0x47, "Sys Manager" },
                { 0x4D, "Sprite End"  }, { 0x00, NULL }
            };
            for (int k = 0; vs[k].name; k++) {
                uint32_t h = bus_r32(s, c->vbr + (uint32_t)vs[k].v * 4);
                printf("  vec 0x%02X %-12s -> 0x%08X%s\n", vs[k].v, vs[k].name, h,
                       h == 0x00000200u ? "   (still our stub - game never hooked it)"
                                        : "   <- game handler");
            }
        }
        if (c->halted) {
            printf("HALTED       %s at PC=0x%08X\n", c->fault, c->fault_pc);
            if (c == &s->master) {
                /* The last PCs the master retired. A runaway into unmapped
                 * space only makes sense with the path that got it there. */
                uint32_t n = s->mring_head < 48u ? s->mring_head : 48u;
                uint32_t k;
                printf("  master PC ring (oldest first):\n");
                for (k = n; k > 0; k--) {
                    uint32_t a = s->mring[(s->mring_head - k) & 255u];
                    char txt[64];
                    if (!sh2_format(bus_r16(s, a), a, txt)) txt[0] = 0;
                    printf("    master %08X  %s\n", a, txt);
                }
            }
        } else if (c->sleeping) {
            printf("SLEEPING     at PC=0x%08X (waiting on an interrupt)\n", c->pc);
        } else {
            printf("BUDGET       exhausted, still running at PC=0x%08X\n", c->pc);
        }
        printf("\nfinal CPU state\n");
        dump_state(c, stdout);
        printf("\n");
        dump_around(s, c->halted ? c->fault_pc : c->pc, stdout);
        cdb_report(s, stdout);
        /* Raw 32-bit word dump with offsets -- for reading a whole struct at once
     * instead of probing addresses one at a time. SATURN_WORDS=base[,count] */
    if (getenv("SATURN_WORDS")) {
        char buf[128]; strncpy(buf, getenv("SATURN_WORDS"), sizeof(buf)-1);
        buf[sizeof(buf)-1] = 0;
        char *comma = strchr(buf, ',');
        int n = comma ? atoi(comma + 1) : 32;
        if (comma) *comma = 0;
        uint32_t base = (uint32_t)strtoul(buf, NULL, 0);
        printf("\nwords at 0x%08X:\n", base);
        for (int k = 0; k < n; k++) {
            uint32_t a = base + (uint32_t)k * 4;
            uint32_t v = bus_r32(s, a);
            printf("  +%-4d 0x%08X = %08X%s\n", k*4, a, v,
                   (v & 0x40) ? "   <bit6 SET>" : "");
        }
    }
    /* SATURN_MEMDUMP=addr,len,file -- write `len` bytes of guest memory to a
     * host file at the end of the run. Game code is loaded and relocated at
     * runtime, so a routine at 0x0606xxxx exists in no file on the disc;
     * without this there is no way to disassemble an overlay that is spinning.
     * Pair it with tools/sh2_disasm.py --base to read the result. */
    /* One implementation, in bus.c, shared with the SATURN_DUMPAT hit capture.
     * This used to be a second copy that printed the same "memdump:" line, so
     * an exit dump and a hit dump were indistinguishable in the log. */
    mem_dump_at(s, 0);

    if (getenv("SATURN_COVER")) {
        const char *e = getenv("SATURN_COVER");
        char cb[128]; char *cc; uint32_t lo=0, hi=0;
        strncpy(cb, e, sizeof(cb)-1); cb[sizeof(cb)-1]=0;
        cc = strchr(cb, ',');
        if (cc) { *cc=0; lo=(uint32_t)strtoul(cb,NULL,0); hi=(uint32_t)strtoul(cc+1,NULL,0); }
        if (hi) {
            uint32_t blk, n=(hi-lo)>>5, run_lo=0; int in_run=0, executed=0;
            printf("coverage %08X-%08X (32-byte blocks that EXECUTED):\n", lo, hi);
            for (blk=0; blk<=n && blk<COVER_BLOCKS; blk++) {
                int hit = (blk<n) && (s->cover[blk>>3] >> (blk&7)) & 1;
                if (hit) executed++;
                if (hit && !in_run) { run_lo=blk; in_run=1; }
                else if (!hit && in_run) {
                    printf("  %08X-%08X\n", lo+(run_lo<<5), lo+(blk<<5)-1);
                    in_run=0;
                }
            }
            printf("  %u of %u blocks executed (%.1f%%)\n", executed, n, n?100.0*executed/n:0.0);
        }
    }

    if (getenv("SATURN_DUMP")) {
            /* Comma-separated list: each entry is a start address, optionally
             * ":len". One run of this harness costs ~100s, so being able to
             * disassemble several routines per run matters. */
            const char *p = getenv("SATURN_DUMP");
            while (*p) {
                char *end;
                unsigned long lo = strtoul(p, &end, 0);
                unsigned long len = 0x80;
                if (*end == ':') len = strtoul(end + 1, &end, 0);
                dump_region(s, (uint32_t)lo, (uint32_t)(lo + len), stdout);
                while (*end == ',' || *end == ' ') end++;
                if (end == p) break;
                p = end;
            }
        }
        {
            static const struct { unsigned off; const char *n; } vr[] = {
                {0x00,"TVMD"},{0x0E,"RAMCTL"},{0x20,"BGON"},{0x28,"CHCTLA"},
                {0x2A,"CHCTLB"},{0x30,"PNCN0"},{0x32,"PNCN1"},{0x3A,"PLSZ"},
                {0x3C,"MPOFN"},{0x40,"MPABN0"},{0x42,"MPCDN0"},{0x44,"MPABN1"},
                {0x70,"SCXIN0"},{0x74,"SCYIN0"},{0x78,"ZMXIN0"},{0xE0,"PRINA"},
                {0xE2,"PRINB"},{0xAC,"BKTAU"},{0xAE,"BKTAL"},{0,NULL}
            };
            printf("\nVDP2 layer state\n");
            for (int k = 0; vr[k].n; k++)
                printf("  %-7s 0x%04X\n", vr[k].n, s->vdp2_reg[vr[k].off >> 1]);
            printf("  CRAM[0..7] ");
            for (int k = 0; k < 8; k++)
                printf("%04X ", (s->cram[k*2] << 8) | s->cram[k*2+1]);
            printf("\n  VDP2 VRAM non-zero bytes: ");
            { unsigned long nz = 0;
              for (unsigned k = 0; k < VDP2_VRAM_SZ; k++) if (s->vdp2_vram[k]) nz++;
              printf("%lu of %u\n", nz, (unsigned)VDP2_VRAM_SZ); }
            {
                uint32_t first = 0xFFFFFFFFu;
                for (uint32_t b = 0; b < VDP2_VRAM_SZ / 32768u; b++) {
                    uint32_t cnt = 0;
                    for (uint32_t k = 0; k < 32768u; k++)
                        if (s->vdp2_vram[b * 32768u + k]) {
                            cnt++;
                            if (first == 0xFFFFFFFFu) first = b*32768u + k;
                        }
                    if (cnt) printf("    bank %2u (0x%05X): %u non-zero\n",
                                    b, b * 32768u, cnt);
                }
                printf("    first non-zero at 0x%05X\n", first);
            }
        }
        for (int k = 0; k < s->ncr2log; k++)
            printf("CR2 read = %04X  from PC %08X\n", s->cr2log[k].val, s->cr2log[k].pc);
        if (s->bal_ok)
            printf("stack balance: %llu checked, %llu MISMATCH (first got %08X want %08X)\n",
                   (unsigned long long)s->bal_ok, (unsigned long long)s->bal_bad,
                   s->bal_bad_sp, s->bal_bad_exp);
        for (int k = 0; k < s->nltrace; k++)
            printf("  hit %2d  r0=%08X r4=%08X  from %08X\n",
                   k, s->trace_r0[k], s->trace_r4[k], s->trace_prev[k]);
        for (int k = 0; k < s->nr0chg; k++)
            printf("r0 0x10000->0 done by PC %08X, next PC %08X\n",
                   s->r0chg_pc[k], s->r0chg_next[k]);
        if (s->pclast_pc && s->pclast_head) {
            uint32_t n = s->pclast_head < 16u ? s->pclast_head : 16u;
            printf("last %u visits to %08X (most recent last):\n",
                   n, s->pclast_pc);
            for (uint32_t k = n; k > 0; k--) {
                uint32_t i = (s->pclast_head - k) & 15u;
                printf("  pr=%08X cy=%llu\n   ", s->pclast[i].pr,
                       (unsigned long long)s->pclast[i].cy);
                for (int q = 0; q < 16; q++)
                    printf(" r%d=%08X", q, s->pclast[i].r[q]);
                printf("\n");
            }
        }
        for (int k = 0; k < s->nirqpc; k++)
            printf("IRQ inside routine at PC %08X with r0=%08X\n",
                   s->irqpc[k], s->irqpc_r0[k]);
        if (s->snap_taken) {
            printf("--- snapshot of 0x%08X at the trigger ---\n", s->snap_addr);
            for (int q = 0; q < 128; q++) {
                char txt[64];
                uint32_t a = s->snap_addr + q * 2u;
                if (!sh2_format(s->snap[q], a, txt)) txt[0] = 0;
                printf("  %08X  %04X  %s\n", a, s->snap[q], txt);
            }
        }
        if (s->ring_frozen) {
            uint32_t n = s->ring_head < RING_N ? s->ring_head : RING_N;
            uint32_t start = s->ring_head - n;
            printf("--- last %u instructions before the trigger ---\n", n);
            for (uint32_t k = (n > 40 ? n - 40 : 0); k < n; k++) {
                uint32_t idx = (start + k) & (RING_N - 1);
                char txt[64];
                uint16_t op = bus_r16(s, s->ring_pc[idx]);
                if (!sh2_format(op, s->ring_pc[idx], txt)) txt[0] = 0;
                printf("  %08X  r0=%08X r4=%08X  %s\n",
                       s->ring_pc[idx], s->ring_r0[idx], s->ring_r4[idx], txt);
            }
        }
        s->irq_clobber_reg = s->irq_clobber_reg;
        for (int k = 0; k < 7; k++)
            if (s->irq_clobber[k])
                printf("r%d CLOBBERED across an interrupt %llu times\n",
                       8 + k, (unsigned long long)s->irq_clobber[k]);
        if (s->irq_clobber_reg > 0)
            printf("first clobber: r%d %08X -> %08X, interrupted PC %08X\n",
                   s->irq_clobber_reg, s->irq_clobber_old,
                   s->irq_clobber_new, s->irq_clobber_pc);
        for (int k = 0; k < s->nr13w; k++)
            printf("r13 changed by PC %08X : %08X -> %08X\n",
                   s->r13w_pc[k], s->r13w_old[k], s->r13w_new[k]);
        for (int k = 0; k < s->nintback; k++)
            printf("INTBACK request IREG0=%02X IREG1=%02X\n",
                   s->intback_ireg[k][0], s->intback_ireg[k][1]);
        printf("periodic status reports sent: %llu (boot_delay now %d)\n",
               (unsigned long long)s->cd.periodic_count, s->cd.boot_delay);
        for (int k = 0; k < s->nbadcr; k++)
            printf("bare-status CR1=%04X written while CPU at PC %08X\n",
                   s->badcr_val[k], s->badcr_pc[k]);
        printf("END: cdb_reg CR1=%04X CR2=%04X CR3=%04X CR4=%04X ; bus_r16=%04X\n",
               s->cdb_reg[0x18>>1], s->cdb_reg[0x1C>>1],
               s->cdb_reg[0x20>>1], s->cdb_reg[0x24>>1],
               bus_r16(s, 0x25890018u));
        printf("irq nesting: max depth %d (outer level %d, inner %d, at PC %08X), depth now %d\n",
               s->irq_depth_max, s->irq_nest_outer, s->irq_nest_inner,
               s->irq_nest_pc, s->irq_depth);
        printf("hits with R0==0: %llu, first such entered from %08X\n",
               (unsigned long long)s->watch_zero, s->watch_zero_prev);
        printf("first-hit R0=%08X R4=%08X R5=%08X R6=%08X\n",
               s->watch_first[0], s->watch_first[4],
               s->watch_first[5], s->watch_first[6]);
        for (int k = 0; k < s->npred; k++)
            printf("entered from PC %08X : %llu times\n",
                   s->pred[k].pc, (unsigned long long)s->pred[k].n);
        printf("CR block changed between reads: %llu times\n",
               (unsigned long long)s->cr_changes);
        for (int k = 0; k < s->nrwatch; k++)
            printf("cdread from PC %08X : %llu times\n",
                   s->rwatch[k].pc, (unsigned long long)s->rwatch[k].count);
        for (int k = 0; k < s->nxferlog; k++) {
            printf("cdxfer %u bytes, head:", s->xferlog[k].bytes);
            for (int m = 0; m < 12; m++) printf(" %02X", s->xferlog[k].head[m]);
            printf("  \"");
            for (int m = 0; m < 12; m++) { int ch = s->xferlog[k].head[m];
                putchar(ch >= 32 && ch < 127 ? ch : 46); }
            printf("\"  next2recs:");
            for (int m = 0; m < 24; m++) printf(" %02X", s->xferlog[k].more[m]);
            printf("\n");
        }
        for (int k = 0; k < s->ncdresp; k++)
            printf("cdresp op 0x%02X -> CR1 %04X CR2 %04X CR3 %04X CR4 %04X  (len=%u words)\n",
                   s->cdresp[k].op, s->cdresp[k].cr1, s->cdresp[k].cr2,
                   s->cdresp[k].cr3, s->cdresp[k].cr4,
                   ((s->cdresp[k].cr1 << 16) | s->cdresp[k].cr2) & 0xFFFFFF);
        for (int k = 0; k < s->ncdwlog; k++)
            printf("cdwrite 0x%08X = 0x%04X  from PC 0x%08X\n",
                   s->cdwlog[k].addr, s->cdwlog[k].val, s->cdwlog[k].pc);
        {
            static uint32_t fr[704*512];
            int dw, dh, nb = 0;
            uint32_t first = 0;
            vdp2_display_size(s, &dw, &dh);
            if (dw > 704) dw = 704;
            if (dh > 512) dh = 512;
            vdp2_render(s, fr, dw, dh, 1);
            for (int i = 0; i < dw*dh; i++)
                if ((fr[i] & 0x00FFFFFF) != 0) { if (!nb) first = fr[i]; nb++; }
            /* Render the composite as ASCII so the screen can be READ from a
             * headless run -- the cheapest diagnostic now that anything is
             * actually being drawn: menu, error message, or game? */
            if (getenv("SATURN_ASCII")) {
                const char *ramp = " .:-=+*#%@";
                int cols = 100, rows = 46;
                for (int ry = 0; ry < rows; ry++) {
                    for (int rx = 0; rx < cols; rx++) {
                        long sum = 0; int n = 0;
                        int x0 = rx * dw / cols, x1 = (rx+1) * dw / cols;
                        int y0 = ry * dh / rows, y1 = (ry+1) * dh / rows;
                        if (x1 <= x0) x1 = x0 + 1;
                        if (y1 <= y0) y1 = y0 + 1;
                        for (int y = y0; y < y1 && y < dh; y++)
                            for (int x = x0; x < x1 && x < dw; x++) {
                                uint32_t c = fr[y*dw + x];
                                sum += ((c>>16 & 0xFF) + (c>>8 & 0xFF) + (c & 0xFF)) / 3;
                                n++;
                            }
                        int v = n ? (int)(sum / n) : 0;
                        putchar(ramp[v * 9 / 255]);
                    }
                    putchar(10);
                }
            }
            /* part_sectors/part_bytes are ARRAYS -- passing them bare printed the
             * pointer, so this line reported things like
             * "part_sectors=159967632" and read as a wildly corrupt
             * partition when nothing was wrong. Print each populated one. */
            printf("CD partition: playing=%d xfer %u/%u\n",
                   s->cd.playing, s->cd.xfer_pos, s->cd.xfer_size);
            for (int k = 0; k < CD_NUM_PARTS; k++)
                if (s->cd.part_sectors[k])
                    printf("  part[%d] sectors=%u bytes=%u\n", k,
                           s->cd.part_sectors[k], s->cd.part_bytes[k]);
            for (int k = 0; k < s->nstaged && k < 12; k++)
                printf("  xfer#%d staged %u; previous was %u read %u\n", k,
                       s->stagelog[k].size, s->stagelog[k].prev_size,
                       s->stagelog[k].prev_read);
            for (int k = 0; k < s->nsdlog; k++)
                printf("  GetSectorData off=%u want=%u have=%u\n",
                       s->sdlog[k].off, s->sdlog[k].want, s->sdlog[k].have);
            printf("staged: %d transfers, %llu bytes total; host read %llu bytes\n",
                   s->nstaged, (unsigned long long)s->staged_total,
                   (unsigned long long)((s->dtr_ok + s->dtr_dry) * 2));
            for (int k = 0; k < s->nsmpccmd; k++)
                printf("  SMPC cmd 0x%02X x%u\n",
                       s->smpccmd[k].cmd, s->smpccmd[k].count);
            printf("slave: enabled=%d pc=%08X halted=%d sleeping=%d cycles=%llu r0=%08X r1=%08X r6=%08X r15=%08X sr=%08X\n",
                   s->slave_enabled, s->slave.pc, s->slave.halted,
                   s->slave.sleeping, (unsigned long long)s->slave.cycles,
                   s->slave.r[0], s->slave.r[1], s->slave.r[6], s->slave.r[15],
                   s->slave.sr);
            /* Per-core on-chip state. The FRT is how the two cores pace
             * against the raster, so when a handshake does not close this is
             * the first thing to look at -- and it is per core, so printing
             * one bank tells you nothing about the other. */
            for (int w = 0; w < 2; w++) {
                sh2 *cc = w ? &s->slave : &s->master;
                printf("%s on-chip: TIER=%02X FTCSR=%02X FRC=%04X TCR=%02X TOCR=%02X FICR=%04X IPRB=%04X VCRC=%04X VCRD=%04X\n",
                       w ? "slave " : "master",
                       cc->onchip[0x10], cc->onchip[0x11], cc->frc,
                       cc->onchip[0x16], cc->onchip[0x17],
                       (unsigned)((cc->onchip[0x18] << 8) | cc->onchip[0x19]),
                       (unsigned)((cc->onchip[0x60] << 8) | cc->onchip[0x61]),
                       (unsigned)((cc->onchip[0x66] << 8) | cc->onchip[0x67]),
                       (unsigned)((cc->onchip[0x68] << 8) | cc->onchip[0x69]));
            }
            printf("FRT-sourced interrupts: %llu   fields: %llu   clk: %llu\n",
                   (unsigned long long)s->frt_irqs,
                   (unsigned long long)s->frames,
                   (unsigned long long)s->clk);
            {
                extern uint64_t g_capture_count[2];
                printf("FRT input capture (inter-CPU doorbell): master %llu, slave %llu\n",
                       (unsigned long long)g_capture_count[0],
                       (unsigned long long)g_capture_count[1]);
            }
            if (getenv("SATURN_CACHELOG")) {
                for (unsigned core = 0; core < 2; core++) {
                    sh2 *cc = core ? &s->slave : &s->master;
                    unsigned valid = 0;
                    for (unsigned set = 0; set < 64; set++)
                        for (unsigned way = 0; way < 4; way++)
                            valid += cc->cache_valid[set][way] != 0;
                    printf("%s instruction cache: CCR=%02X, %u valid lines\n",
                           core ? "slave" : "master", cc->onchip[0x92], valid);
                    for (unsigned set = 0; set < 64; set++) {
                        for (unsigned way = 0; way < 4; way++) {
                            uint32_t a;
                            uint8_t *line;
                            if (!cc->cache_valid[set][way]) continue;
                            a = (cc->cache_tag[set][way] << 10) | (set << 4);
                            if (a < 0x06060000u || a >= 0x06080000u) continue;
                            line = &cc->cache_data[(way << 10) | (set << 4)];
                            printf("  %08X way%u:", a, way);
                            for (unsigned k = 0; k < 16; k++) printf(" %02X", line[k]);
                            putchar('\n');
                        }
                    }
                }
            }
            printf("DTR reads: %llu with data, %llu DRY (returned 0)\n",
                   (unsigned long long)s->dtr_ok, (unsigned long long)s->dtr_dry);
            /* CD audio rate accounting: one sector is 588 stereo frames, so
             * pushed*588 and drained should track each other. A shortfall in
             * `drained` means the mixer never consumed what the drive
             * delivered -- the music plays fast because sectors were dropped. */
            if (s->cdda_pushed || s->cdda_drained)
                printf("SCSP: %llu loop(s) taken by an already-silent slot" "\n",
               (unsigned long long)s->scsp_silent_loops);
            printf("SCSP: %llu key-on(s), peak output %u, %llu non-silent sample(s)\n",
                   (unsigned long long)s->scsp_keyons,
                   s->scsp_peak, (unsigned long long)s->scsp_nonsilent);
            printf("SCSP: total energy %llu\n",
                   (unsigned long long)s->scsp_energy);
            if (getenv("SATURN_DSPPROBE")) {
                /* Peaks accumulated across the WHOLE run (scsp.c), not the
                 * value that happened to be in the register at exit. */
                int32_t emax = s->dsp_efreg_peak, mmax = s->dsp_mixs_peak;
                printf("SCSP DSP: MPRO %llu, COEF %llu, MADRS %llu writes; "
                       "prog_len %u, |EFREG| max %d, |MIXS| max %d\n",
                       (unsigned long long)s->dsp_mpro_writes,
                       (unsigned long long)s->dsp_coef_writes,
                       (unsigned long long)s->dsp_madrs_writes,
                       (unsigned)s->dsp.prog_len, (int)emax, (int)mmax);
                printf("SCSP DSP: effect RETURN max EFSDL=%u, slots returning=%04X\n",
                       s->dsp_efsdl_max, s->dsp_efret_slots);
                printf("SCSP DSP: ring RBP=%05X RBL=%05X | MRD=%llu MWT=%llu | delay read peak=%d\n",
                       s->dsp.rbp, s->dsp.rbl,
                       (unsigned long long)s->dsp_mrd,
                       (unsigned long long)s->dsp_mwt,
                       (int)s->dsp_readval_peak);
                printf("SCSP DSP: %llu send(s) with IMXL>0, "
                       "reg14 non-zero %llu time(s)\n",
                       (unsigned long long)s->dsp_sends,
                       (unsigned long long)s->dsp_reg14_nonzero);
            }
        printf("CDDA: %llu sectors pushed (= %llu frames), %llu frames "
                       "drained (%.2f%%), %llu overruns, %llu underruns\n",
                       (unsigned long long)s->cdda_pushed,
                       (unsigned long long)(s->cdda_pushed * 588ull),
                       (unsigned long long)s->cdda_drained,
                       s->cdda_pushed ? 100.0 * (double)s->cdda_drained /
                                        (double)(s->cdda_pushed * 588ull) : 0.0,
                       (unsigned long long)s->cdda_over,
                       (unsigned long long)s->cdda_under);
            for (int k = 0; k < s->nwrlog; k++)
                printf("wr %08X = %08X (sz%d) from PC %08X cy=%llu\n",
                       s->wrlog[k].addr, s->wrlog[k].val,
                       s->wrlog[k].sz, s->wrlog[k].pc,
                       (unsigned long long)s->wrlog[k].cy);
            for (int k = 0; k < s->nrrlog; k++)
                printf("rd %08X from PC %08X : %llu times %s\n",
                       s->rrlog[k].addr, s->rrlog[k].pc,
                       (unsigned long long)s->rrlog[k].n,
                       s->rrlog[k].slave ? "slave" : "master");
            printf("composite non-black pixels: %d of %d (%dx%d) first 0x%08X\n",
                   nb, dw*dh, dw, dh, first);
            if (getenv("SATURN_ROW")) {
                int ry = atoi(getenv("SATURN_ROW"));
                printf("row %d:", ry);
                for (int q = 0; q < 12 && q < dw; q++)
                    printf(" %06X", fr[ry * dw + q] & 0xFFFFFF);
                printf("\n");
            }
            if (getenv("SATURN_CELLDBG")) {
                int L = 2, cx = 8, cy = 150;
                sscanf(getenv("SATURN_CELLDBG"), "%d,%d,%d", &L, &cx, &cy);
                vdp2_cell_debug(s, L, cx, cy);
            }
            if (getenv("SATURN_PNG")) {
                const uint32_t *out = fr;
                int ow = dw, oh = dh;
                if (live_w) {
                    out = live_fr; ow = live_w; oh = live_h;
                    printf("capture     field %llu, best-drawn (not end state)\n",
                           (unsigned long long)live_field);
                } else if (want_live) {
                    printf("capture     nothing rendered in any field\n");
                }
                if (live_w) {
                    static const struct { unsigned o; const char *n; } lr[] = {
                        {0x00,"TVMD"},{0x06,"VRSIZE"},{0x0E,"RAMCTL"},{0x20,"BGON"},
                        {0x28,"CHCTLA"},{0x2A,"CHCTLB"},{0x30,"PNCN0"},{0x3A,"PLSZ"},
                        {0x3C,"MPOFN"},{0x40,"MPABN0"},{0xE0,"SPCTL"},{0xE6,"CRAOFB"},{0xF0,"PRISA"},{0xF8,"PRINA"},{0xFA,"PRINB"},
                        {0xAC,"BKTAU"},{0xAE,"BKTAL"},
                        {0xB0,"RPMD"},{0xB2,"RPRCTL"},{0xB4,"KTCTL"},{0xB6,"KTAOF"},
                        {0xBC,"RPTAU"},{0xBE,"RPTAL"},{0xC0,"WPSX0"},{0xC2,"WPSY0"},
                        {0xC4,"WPEX0"},{0xC6,"WPEY0"},{0xD0,"WCTLA"},{0xD2,"WCTLB"},
                        {0xD4,"WCTLC"},{0xD6,"WCTLD"},{0xEC,"CCCTL"},{0x108,"CCRNA"},
                        {0xFC,"PRIR"},{0,NULL}};
                    printf("live VDP2 (field %llu, %d px drawn):",
                           (unsigned long long)live_field, live_score);
                    for (int q = 0; lr[q].n; q++)
                        printf(" %s=%04X", lr[q].n, live_reg[lr[q].o >> 1]);
                    printf("\n");
                    printf("live FB probes (60,40)(300,60)(160,10)(20,200)(300,200)(160,112):");
                    for (int q = 0; q < 6; q++) printf(" %04X", live_fbs[q]);
                    printf("\n");
                    printf("live CRAM mode-2 bank 0x100 (RGB888, first 32 entries):");
                    for (int q = 0; q < 32; q++)
                        printf(" %02X%02X%02X", live_cram2[q*4+3], live_cram2[q*4+2], live_cram2[q*4+1]);
                    printf("%s", "\n");
                    printf("live CRAM[0..15]:");
                    for (int q = 0; q < 16; q++) printf(" %04X", live_cram[q]);
                    printf("\n");
                }
                if (png_write(getenv("SATURN_PNG"), out, ow, oh) == 0)
                    printf("wrote %s (%dx%d)\n", getenv("SATURN_PNG"), ow, oh);
            }
        }
        if (s->cd_read_pc) {
            printf("first CD register read from PC 0x%08X addr 0x%08X\n", s->cd_read_pc, s->cd_read_addr);
            dump_region(s, s->cd_read_pc - 0x30, s->cd_read_pc + 0x20, stdout);
        }
        {
            extern uint64_t g_dma_dst_bytes[128];
            extern uint32_t g_dma_dst_count[128];
            for (int k = 0; k < 128; k++)
                if (g_dma_dst_count[k])
                    printf("SH2 DMA dst %02X00000-%02XFFFFF : %u transfers, %llu bytes\n",
                           k, k, g_dma_dst_count[k],
                           (unsigned long long)g_dma_dst_bytes[k]);
        }
        for (int k = 0; k < s->nocdmalog; k++)
            printf("SH2 DMA src 0x%08X -> dst 0x%08X  %u bytes  chcr 0x%08X\n",
                   s->ocdmalog[k].src, s->ocdmalog[k].dst,
                   s->ocdmalog[k].cnt, s->ocdmalog[k].chcr);
        if (s->ndmalog) {
            printf("first SCU DMA transfers:\n");
            for (int k = 0; k < s->ndmalog; k++)
                printf("  src 0x%08X -> dst 0x%08X  %u bytes  md 0x%08X ad 0x%08X\n",
                       s->dmalog[k].src, s->dmalog[k].dst, s->dmalog[k].cnt,
                       s->dmalog[k].md, s->dmalog[k].ad);
        }
        if (s->wwatch_addr) {
            for (int k = 0; k < s->nwlog; k++)
                printf("  write#%d cy=%llu %s pc=%08X val=%08X %s\n", k,
                       (unsigned long long)s->wlog[k].cy,
                       s->wlog[k].slave ? "SLAVE " : "master", s->wlog[k].pc,
                       s->wlog[k].val, s->wlog[k].sz ? "byte" : "wide");
            printf("  first write at cycle %llu, last at %llu; first IRQ at cycle %llu\n",
                   (unsigned long long)s->wwatch_first_cy,
                   (unsigned long long)s->wwatch_last_cy,
                   (unsigned long long)s->first_irq_cy);
            printf("  last writer PC %08X\n", s->wwatch_pc);
            printf("byte writes to 0x%08X: %llu, last value 0x%02X\n",
                   s->wwatch_addr, (unsigned long long)s->wwatch_hits,
                   s->wwatch_last & 0xFF);
            printf("  first write from PC 0x%08X\n", s->wwatch_pc);
        }
        if (s->watch_pc) {
            printf("watch first hit at cycle %llu\n", (unsigned long long)s->watch_cy);
            printf("watch 0x%08X executed %llu times\n",
                   s->watch_pc, (unsigned long long)s->watch_hits);
            printf("    PR=%08X\n", s->watch_pr);
            for (int wi = 0; wi < 16; wi += 4)
                printf("    R%-2d=%08X R%-2d=%08X R%-2d=%08X R%-2d=%08X\n",
                       wi, s->watch_regs[wi], wi+1, s->watch_regs[wi+1],
                       wi+2, s->watch_regs[wi+2], wi+3, s->watch_regs[wi+3]);
        }
        if (s->nbioscall) {
            printf("\nBIOS services called (arguments at first call)\n");
            printf("%-12s %-10s %-10s %-10s %-10s %s\n",
                   "SLOT", "r4", "r5", "r6", "r7", "CALLS");
            for (int k = 0; k < s->nbioscall; k++)
                printf("0x%08X   0x%08X 0x%08X 0x%08X 0x%08X %u\n",
                       s->bioscall[k].slot, s->bioscall[k].r4, s->bioscall[k].r5,
                       s->bioscall[k].r6, s->bioscall[k].r7, s->bioscall[k].count);
        }
        saturn_report_trace(s, stdout);
    }

    free(image);
    iso_free(&fs);
    disc_close(&d);
    return 0;
}
