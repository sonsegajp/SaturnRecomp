/* cd_bus.c -- CD/CS2 address decoding and peripheral-bus access semantics.
 *
 * The CD register file is a 64-byte device mirrored through a 4 KiB block;
 * that block repeats every 32 KiB across A-Bus CS2. These checks keep the CPU,
 * SCU DMA, diagnostics, and FIFO side effects on that one hardware mapping. */
#include "saturn.h"
#include "disc.h"
#include <stdio.h>
#include <string.h>

static saturn S;
static int fails, checks;

static void ck(const char *what, uint32_t got, uint32_t want)
{
    checks++;
    if (got != want) {
        printf("  FAIL %-47s got %08X want %08X\n", what, got, want);
        fails++;
    }
}

static void reset_state(void)
{
    memset(&S, 0, sizeof S);
    /* Keep periodic-report generation dormant while testing register I/O. */
    S.cd.boot_delay = 1;
}

static int report_contains(const char *needle)
{
    FILE *f = tmpfile();
    char text[4096];
    size_t n;
    if (!f) return 0;
    saturn_report_trace(&S, f);
    rewind(f);
    n = fread(text, 1, sizeof text - 1, f);
    text[n] = 0;
    fclose(f);
    return strstr(text, needle) != NULL;
}

int main(void)
{
    uint8_t fifo[8];
    uint64_t before;

    reset_state();
    S.cdb_reg[0x08 >> 1] = 0xABCD;
    ck("canonical block register read", bus_r16(&S, 0x05818008), 0xABCD);
    ck("32 KiB block mirror read", bus_r16(&S, 0x05890008), 0xABCD);
    ck("64-byte register mirror read", bus_r16(&S, 0x05818048), 0xABCD);
    ck("cache-through mirror read", bus_r16(&S, 0x25818008), 0xABCD);

    before = S.unmapped_reads;
    (void)bus_r16(&S, 0x05800FFE);       /* last word in a mapped 4 KiB block */
    ck("mapped block boundary not unmapped", (uint32_t)S.unmapped_reads,
       (uint32_t)before);
    (void)bus_r16(&S, 0x05801000);       /* hole before the next 32 KiB block */
    ck("CS2 hole counted unmapped", (uint32_t)S.unmapped_reads,
       (uint32_t)(before + 1));

    S.cdb_reg[0x0C >> 1] = 0;
    bus_w16(&S, 0x058F800C, 0x55AA);     /* final mapped block */
    ck("final CS2 block is mapped", S.cdb_reg[0x0C >> 1], 0x55AA);
    before = S.unmapped_writes;
    bus_w16(&S, 0x058F900C, 0x1234);     /* hole after final mapped block */
    ck("final CS2 hole counted unmapped", (uint32_t)S.unmapped_writes,
       (uint32_t)(before + 1));

    reset_state();
    memset(fifo, 0, sizeof fifo);
    fifo[0] = 0x11; fifo[1] = 0x22; fifo[2] = 0x33; fifo[3] = 0x44;
    S.cd.xfer = fifo; S.cd.xfer_size = 4; S.cd.xfer_pos = 0;
    ck("32-bit DATATRNS splits into two FIFO reads",
       bus_r32(&S, 0x05818000), 0x11223344);
    ck("32-bit DATATRNS advances four bytes", S.cd.xfer_pos, 4);

    reset_state();
    fifo[0] = 0x11; fifo[1] = 0x22;
    S.cd.xfer = fifo; S.cd.xfer_size = 2; S.cd.xfer_type = 2;
    (void)bus_r16(&S, 0x05818000);
    ck("non-sector FIFO exhaustion does not raise EHST",
       S.cdb_reg[0x08 >> 1] & 0x0080u, 0);

    reset_state();
    fifo[0] = 0x11; fifo[1] = 0x22;
    S.cd.xfer = fifo; S.cd.xfer_size = 2; S.cd.xfer_type = 1;
    (void)bus_r16(&S, 0x05818000);
    ck("sector FIFO exhaustion raises EHST",
       S.cdb_reg[0x08 >> 1] & 0x0080u, 0x0080u);

    S.cd.xfer_pos = 0;
    ck("odd DATATRNS byte offset is unhandled", bus_r8(&S, 0x05890001), 0);
    ck("odd DATATRNS byte does not consume FIFO", S.cd.xfer_pos, 0);
    ck("even DATATRNS byte uses direct low byte", bus_r8(&S, 0x05890000), 0x22);
    ck("even DATATRNS byte consumes one word", S.cd.xfer_pos, 2);

    reset_state();
    S.cdb_reg[0x0C >> 1] = 0xFFFF;
    bus_w8(&S, 0x0581800D, 0xA5);
    ck("odd byte write is not a register RMW", S.cdb_reg[0x0C >> 1], 0xFFFF);
    bus_w8(&S, 0x0581800C, 0x5A);
    ck("even byte write dispatches directly", S.cdb_reg[0x0C >> 1], 0x005A);
    bus_w32(&S, 0x0581800C, 0x12345678);
    ck("32-bit register write splits in bus order", S.cdb_reg[0x0C >> 1], 0x1234);

    S.cdb_reg[0x08 >> 1] = 0xFFFF;
    bus_w16(&S, 0x05890048, 0x0F0F);
    ck("mirrored HIRQ write clears bits", S.cdb_reg[0x08 >> 1], 0x0F0F);

    memset(fifo, 0, sizeof fifo);
    S.cd.xfer = fifo; S.cd.xfer_type = 3; S.cd.xfer_size = 4; S.cd.xfer_pos = 0;
    bus_w8(&S, 0x05818000, 0x12);
    ck("DATATRNS byte write advances one bus word", S.cd.xfer_pos, 2);
    ck("DATATRNS byte write stores zero-extended word",
       ((uint32_t)fifo[0] << 8) | fifo[1], 0x0012);
    bus_w8(&S, 0x05818001, 0xFF);
    ck("odd DATATRNS write does not advance", S.cd.xfer_pos, 2);
    bus_w16(&S, 0x05818002, 0x3456);
    ck("DATATRNS word mirror stores next FIFO word",
       ((uint32_t)fifo[2] << 8) | fifo[3], 0x3456);

    reset_state();
    S.cd.xfer = fifo; S.cd.xfer_size = S.cd.xfer_pos = 4;
    S.cd.xfer_type = 2;
    S.cd.cmd_stage[0] = 0x0600;            /* EndDataTransfer */
    cdb_execute(&S);
    ck("ending a metadata transfer does not raise EHST",
       S.cdb_reg[0x08 >> 1] & 0x0080u, 0);
    ck("EndDataTransfer clears transfer type", S.cd.xfer_type, 0);

    reset_state();
    S.cd.xfer = fifo; S.cd.xfer_size = S.cd.xfer_pos = 4;
    S.cd.xfer_type = 1;
    S.cd.cmd_stage[0] = 0x0600;
    cdb_execute(&S);
    ck("ending a sector transfer raises EHST",
       S.cdb_reg[0x08 >> 1] & 0x0080u, 0x0080u);

    reset_state();
    S.clk = 100;
    S.smpc_resd = 1;
    S.smpc_reg[0x63] = 1;                  /* SF busy */
    bus_w8(&S, 0x0010001Fu, 0x19);        /* RESENAB */
    ck("COMREG schedules SMPC command", S.pending_smpc_cmd, 1);
    ck("scheduled SMPC command leaves SF busy", S.smpc_reg[0x63], 1);
    bus_w8(&S, 0x0010001Fu, 0x1A);        /* rejected while RESENAB pending */
    ck("busy SMPC retains pending command", S.smpc_cmd, 0x19);
    ck("busy SMPC retains COMREG", S.smpc_reg[0x1F], 0x19);
    S.clk = 339;
    smpc_tick(&S);
    ck("SMPC command does not complete early", S.pending_smpc_cmd, 1);
    ck("SMPC side effect does not happen early", S.smpc_resd, 1);
    S.clk = 340;
    smpc_tick(&S);
    ck("SMPC command completes at due boundary", S.pending_smpc_cmd, 0);
    ck("SMPC completion applies command", S.smpc_resd, 0);
    ck("SMPC completion clears SF", S.smpc_reg[0x63], 0);

    reset_state();
    S.trace_enabled = 1;
    S.cdb_reg[0x08 >> 1] = 0xCAFE;
    (void)bus_r16(&S, 0x05818008);
    (void)bus_r16(&S, 0x05801008);
    ck("trace uses real decoder for mapped CD block",
       report_contains("0x05818008 R   2   1         CD block"), 1);
    ck("trace agrees that CS2 hole is unmapped",
       report_contains("0x05801008 R   2   1         unmapped"), 1);

    /* Reading status must retain the drive's exact report clock and must
     * never overwrite a command response while the host is picking it up. */
    reset_state();
    S.cd.boot_delay = 0;
    S.cd.first_cmd_seen = 1;
    S.cd.status = 1;
    S.cd.last_peri_cy = 100;
    S.master.cycles = 100 + 381818 - 1;
    cdb_periodic_maybe(&S);
    ck("periodic report is not early", (uint32_t)S.cd.periodic_count, 0);
    S.master.cycles++;
    cdb_periodic_maybe(&S);
    ck("periodic report at exact deadline", (uint32_t)S.cd.periodic_count, 1);
    ck("periodic report asserts SCDQ", S.cdb_reg[0x08 >> 1] & 0x0400u, 0x0400u);
    S.cd.processing_cmd = 1;
    S.master.cycles += 381818;
    cdb_periodic_maybe(&S);
    ck("periodic report cannot replace command reply", (uint32_t)S.cd.periodic_count, 1);
    S.cd.processing_cmd = 0;
    S.cd.resp_fresh = 1;
    cdb_periodic_maybe(&S);
    ck("reply pickup starts fresh report period", (uint32_t)S.cd.last_peri_cy, (uint32_t)S.master.cycles);
    ck("reply pickup does not emit report", (uint32_t)S.cd.periodic_count, 1);
    S.master.cycles += 381817;
    cdb_periodic_maybe(&S);
    ck("fresh reply remains through period", (uint32_t)S.cd.periodic_count, 1);
    S.master.cycles++;
    cdb_periodic_maybe(&S);
    ck("fresh reply expires at exact deadline", (uint32_t)S.cd.periodic_count, 2);

    /* File ids number the records of the CURRENT directory (0 ".", 1 "..",
     * files and subdirectories from 2) and ChangeDirectory moves it:
     * 0xFFFFFF = root, 1 = parent, otherwise a subdirectory id. */
    {
        static uint8_t xbuf[CD_PART_SECTORS * 2048u];
        static iso_entry ents[5];
        static iso_fs fs;
        static const struct { const char *path; uint32_t lba; int dir; } t[5] = {
            { "/0", 100, 0 }, { "/SUB", 21, 1 }, { "/SUB/A.BIN", 200, 0 },
            { "/SUB/B.BIN", 300, 0 }, { "/Z.TXT", 400, 0 },
        };
        for (int i = 0; i < 5; i++) {
            memset(&ents[i], 0, sizeof ents[i]);
            strcpy(ents[i].path, t[i].path);
            ents[i].lba = t[i].lba; ents[i].size = 2048; ents[i].is_dir = t[i].dir;
        }
        fs.entries = ents; fs.nentries = 5;
#define FI_FAD() (((uint32_t)xbuf[0] << 24) | ((uint32_t)xbuf[1] << 16) | \
                  ((uint32_t)xbuf[2] << 8) | xbuf[3])
#define CD_CMD(a, b, c, d) do { S.cd.cmd_stage[0] = (a); S.cd.cmd_stage[1] = (b); \
        S.cd.cmd_stage[2] = (c); S.cd.cmd_stage[3] = (d); memset(xbuf, 0, 16); \
        cdb_execute(&S); } while (0)
        reset_state();
        S.cd.iso = &fs; S.cd.xfer = xbuf;
        CD_CMD(0x7300, 0, 0, 2);
        ck("root id 2 is /0", FI_FAD(), 100 + 150);
        CD_CMD(0x7300, 0, 0, 4);
        ck("subdirectories keep their id slot", FI_FAD(), 400 + 150);
        CD_CMD(0x7000, 0, 0, 3);                 /* ChangeDirectory /SUB */
        CD_CMD(0x7300, 0, 0, 2);
        ck("id 2 after ChangeDirectory is /SUB/A.BIN", FI_FAD(), 200 + 150);
        CD_CMD(0x7300, 0, 0, 3);
        ck("id 3 after ChangeDirectory is /SUB/B.BIN", FI_FAD(), 300 + 150);
        CD_CMD(0x7400, 0, 0, 3);                 /* ReadFile B.BIN */
        ck("ReadFile uses the current directory", S.cd.play_end_fad, 300 + 150 + 1);
        CD_CMD(0x7000, 0, 0, 1);                 /* ".." */
        CD_CMD(0x7300, 0, 0, 2);
        ck("parent directory restores root ids", FI_FAD(), 100 + 150);
        CD_CMD(0x7000, 0, 0, 3);
        CD_CMD(0x7000, 0, 0x17FF, 0xFFFF);       /* root, filter byte ignored */
        CD_CMD(0x7300, 0, 0, 2);
        ck("0xFFFFFF selects the root", FI_FAD(), 100 + 150);
        CD_CMD(0x7000, 0, 0, 2);                 /* a file is not a directory */
        CD_CMD(0x7300, 0, 0, 2);
        ck("ChangeDirectory to a file is rejected", FI_FAD(), 100 + 150);
#undef CD_CMD
#undef FI_FAD
    }

    if (fails == 0) printf("PASS  CD/CS2 bus decode (%d checks)\n", checks);
    else            printf("FAIL  CD/CS2 bus decode: %d of %d\n", fails, checks);
    return fails ? 1 : 0;
}
