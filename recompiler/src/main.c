/* main.c — SaturnRecomp CLI.
 *
 * Contains no game-specific knowledge. Every command either operates on a
 * disc image directly (so any Saturn title can be inspected on day one) or on
 * a games/<name>/game.toml declaration.
 *
 *   saturnrecomp inspect <disc>              disc header, tracks, file list
 *   saturnrecomp extract <disc> <path> <out> pull one file off the disc
 *   saturnrecomp disasm  <file> <base> [n]   SH-2 disassembly of a raw binary
 *   saturnrecomp modules <game.toml>         resolve a game's declared modules
 */
#include "disc.h"
#include "game_config.h"
#include "../../external/sh2-recomp-core/common/sh2_isa.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *mode_name(track_mode m)
{
    switch (m) {
    case TRACK_MODE1: return "MODE1";
    case TRACK_MODE2: return "MODE2";
    default:          return "AUDIO";
    }
}

static void print_disc_header(disc *d)
{
    saturn_ip ip;

    printf("tracks: %d   sectors in image: %u   pregaps stored in image: %s\n",
           d->ntracks, d->total_sectors, d->pregap_in_file ? "yes" : "no");
    for (int i = 0; i < d->ntracks; i++) {
        const disc_track *t = &d->tracks[i];
        printf("  track %2d  %-5s  %u B/sector  start LBA %-8u pregap %u\n",
               t->num, mode_name(t->mode), t->sector_size, t->start_lba, t->pregap);
    }

    if (ip_read(d, &ip) == 0) {
        printf("\nIP.BIN\n");
        printf("  hardware        %s\n", ip.hardware_id);
        printf("  maker           %s\n", ip.maker_id);
        printf("  product         %s  %s\n", ip.product_no, ip.version);
        printf("  released        %s\n", ip.release_date);
        printf("  device          %s\n", ip.device_info);
        printf("  area            %s\n", ip.area);
        printf("  peripherals     %s\n", ip.peripherals);
        printf("  title           %s\n", ip.title);
        printf("  IP size         0x%08X\n", ip.ip_size);
        printf("  stack M / S     0x%08X / 0x%08X\n", ip.stack_m, ip.stack_s);
        printf("  1st-read addr   0x%08X\n", ip.first_read_addr);
        printf("  1st-read size   0x%08X%s\n", ip.first_read_size,
               ip.first_read_size ? "" : "  (whole file)");
    } else {
        printf("\nIP.BIN: not a Saturn disc (no SEGA SEGASATURN signature)\n");
    }
}

static int cmd_inspect(const char *path)
{
    disc d;
    iso_fs fs;
    int nfile = 0, ndir = 0, nunreadable = 0, nraw = 0;
    uint64_t total = 0;

    if (disc_open(&d, path) != 0) {
        fprintf(stderr, "error: %s\n", d.err[0] ? d.err : "cannot open disc");
        return 1;
    }
    printf("image: %s\n", path);
    print_disc_header(&d);

    if (iso_read(&d, &fs) != 0) {
        fprintf(stderr, "\nerror: %s\n", d.err);
        disc_close(&d);
        return 1;
    }

    printf("\nISO9660: volume \"%s\", %u sectors declared\n",
           fs.volume_id, fs.volume_sectors);
    printf("%-6s %-9s %-11s %s\n", "TYPE", "LBA", "SIZE", "PATH");
    for (int i = 0; i < fs.nentries; i++) {
        const iso_entry *e = &fs.entries[i];
        if (e->is_dir) { ndir++; }
        else           { nfile++; total += e->size; if (!e->readable) nunreadable++;
                         if (e->readable == 2) nraw++; }
        printf("%-6s %-9u %-11u %s%s\n",
               e->is_dir ? "DIR" : "FILE", e->lba, e->size, e->path,
               e->is_dir ? "" :
               e->readable == 2 ? "   [CD-DA track, raw 2352]" :
               e->readable == 0 ? "   [UNREADABLE]" : "");
    }
    printf("\n%d files, %d dirs, %llu bytes\n",
           nfile, ndir, (unsigned long long)total);
    if (nraw)
        printf("%d file(s) stored as CD-DA tracks (raw 2352-byte sectors) - readable.\n", nraw);
    if (nunreadable)
        printf("WARNING: %d file(s) are not backed by any readable sector.\n", nunreadable);

    iso_free(&fs);
    disc_close(&d);
    return 0;
}

static int cmd_extract(const char *path, const char *isopath, const char *out)
{
    disc d;
    iso_fs fs;
    int rc = 1;

    if (disc_open(&d, path) != 0) {
        fprintf(stderr, "error: %s\n", d.err);
        return 1;
    }
    if (iso_read(&d, &fs) != 0) {
        fprintf(stderr, "error: %s\n", d.err);
        disc_close(&d);
        return 1;
    }
    for (int i = 0; i < fs.nentries; i++) {
        const iso_entry *e = &fs.entries[i];
        if (e->is_dir) continue;
        if (strcmp(e->path, isopath) != 0) continue;
        {
            size_t n = 0;
            void *buf = iso_extract(&d, e, &n);
            FILE *f;
            if (!buf) { fprintf(stderr, "error: read failed for %s\n", isopath); break; }
            f = fopen(out, "wb");
            if (!f) { fprintf(stderr, "error: cannot write %s\n", out); free(buf); break; }
            fwrite(buf, 1, n, f);
            fclose(f);
            free(buf);
            printf("extracted %s (%zu bytes, LBA %u) -> %s\n", isopath, n, e->lba, out);
            rc = 0;
        }
        break;
    }
    if (rc) fprintf(stderr, "error: %s not found on disc\n", isopath);
    iso_free(&fs);
    disc_close(&d);
    return rc;
}

static int cmd_disasm(const char *file, const char *base_s, const char *count_s)
{
    FILE *f = fopen(file, "rb");
    uint8_t *buf;
    long size;
    uint32_t base = (uint32_t)strtoul(base_s, NULL, 0);
    long limit = count_s ? strtol(count_s, NULL, 0) : 64;

    if (!f) { fprintf(stderr, "error: cannot open %s\n", file); return 1; }
    fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);
    buf = (uint8_t *)malloc((size_t)size);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "error: read failed\n"); fclose(f); free(buf); return 1;
    }
    fclose(f);

    for (long i = 0; i + 1 < size && (limit <= 0 || i / 2 < limit); i += 2) {
        uint16_t op = (uint16_t)((buf[i] << 8) | buf[i + 1]);   /* big-endian */
        uint32_t pc = base + (uint32_t)i;
        char txt[64];
        if (sh2_format(op, pc, txt))
            printf("%08X  %04X    %s\n", pc, op, txt);
        else
            printf("%08X  %04X    .word 0x%04X\n", pc, op, op);
    }
    free(buf);
    return 0;
}

static int cmd_modules(const char *toml)
{
    game_config g;
    disc d;
    iso_fs fs;
    saturn_ip ip;

    if (gc_load(&g, toml) != 0) {
        fprintf(stderr, "error: %s\n", g.err);
        return 1;
    }
    printf("game     : %s\n", g.name);
    printf("prefix   : %s\n", g.prefix);
    printf("disc     : %s\n", g.disc);

    if (disc_open(&d, g.disc) != 0) {
        fprintf(stderr, "error: %s\n", d.err);
        return 1;
    }
    if (ip_read(&d, &ip) == 0) {
        printf("product  : %s %s   (declared: %s)%s\n",
               ip.product_no, ip.version,
               g.product_no[0] ? g.product_no : "-",
               (g.product_no[0] && strncmp(g.product_no, ip.product_no,
                                           strlen(g.product_no)))
                   ? "   *** MISMATCH ***" : "");
    }
    if (iso_read(&d, &fs) != 0) {
        fprintf(stderr, "error: %s\n", d.err);
        disc_close(&d);
        return 1;
    }

    printf("\n%-12s %-14s %-5s %-6s %-11s %-11s %s\n",
           "MODULE", "FILE", "CPU", "COMP", "LOAD", "SIZE", "STATUS");
    int missing = 0;
    for (int i = 0; i < g.nmodules; i++) {
        const gc_module *m = &g.modules[i];
        const iso_entry *found = NULL;
        for (int j = 0; j < fs.nentries; j++)
            if (!fs.entries[j].is_dir && strcmp(fs.entries[j].path, m->file) == 0)
                { found = &fs.entries[j]; break; }

        printf("%-12s %-14s %-5s %-6s 0x%08X ", m->name, m->file,
               gc_cpu_name(m->cpu), gc_compression_name(m->compression),
               m->load_addr);
        if (!found) { printf("%-11s NOT ON DISC\n", "-"); missing++; }
        else printf("%-11u %s\n", found->size,
                    found->readable ? "ok" : "UNREADABLE (not in a data track)");
    }
    printf("\n%d module(s) declared, %d missing\n", g.nmodules, missing);

    iso_free(&fs);
    disc_close(&d);
    return missing ? 1 : 0;
}

static void usage(void)
{
    fprintf(stderr,
        "SaturnRecomp - static recompiler for Sega Saturn titles\n\n"
        "usage:\n"
        "  saturnrecomp inspect <disc.cue|bin|iso>\n"
        "  saturnrecomp extract <disc> <\"/ISO/PATH\"> <outfile>\n"
        "  saturnrecomp disasm  <rawfile> <base-addr> [instr-count]\n"
        "  saturnrecomp modules <games/<name>/game.toml>\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 2; }
    if (!strcmp(argv[1], "inspect") && argc == 3) return cmd_inspect(argv[2]);
    if (!strcmp(argv[1], "extract") && argc == 5) return cmd_extract(argv[2], argv[3], argv[4]);
    if (!strcmp(argv[1], "disasm")  && argc >= 4) return cmd_disasm(argv[2], argv[3],
                                                                    argc > 4 ? argv[4] : NULL);
    if (!strcmp(argv[1], "modules") && argc == 3) return cmd_modules(argv[2]);
    usage();
    return 2;
}
