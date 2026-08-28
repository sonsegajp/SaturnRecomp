/* game_config.h — per-game declaration file (TOML subset).
 *
 * The recompiler contains NOTHING game-specific. Everything a title needs is
 * declared here: which disc, which files are code, where they load, how they
 * are compressed, and what evidence we have about function boundaries.
 * Adding a second game means adding games/<name>/game.toml, not touching C.
 */
#ifndef GAME_CONFIG_H
#define GAME_CONFIG_H

#include <stdint.h>

#define GC_MAX_MODULES 64
#define GC_MAX_ENTRIES 256

typedef enum {
    GC_COMP_NONE = 0,
    GC_COMP_PRS,          /* Sega PRS (LZ77 + RLE) */
} gc_compression;

typedef enum {
    GC_CPU_SH2 = 0,       /* master/slave SH-2 code */
    GC_CPU_M68K,          /* SCSP sound driver      */
} gc_cpu;

typedef struct {
    char           name[64];
    char           file[128];     /* ISO9660 path, e.g. "/0NIGHTS"        */
    gc_cpu         cpu;
    gc_compression compression;
    uint32_t       load_addr;     /* where the game copies it in memory   */
    uint32_t       entry;         /* known entry point, 0 if unknown      */
    int            is_first_read; /* loaded by IPL from the IP.BIN header */

    /* Manually declared function entry points (evidence). */
    uint32_t       entries[GC_MAX_ENTRIES];
    int            nentries;
} gc_module;

typedef struct {
    char      name[128];
    char      prefix[32];         /* output filename prefix               */
    char      product_no[16];     /* expected IP.BIN product number       */
    char      disc[512];          /* path to .cue/.bin/.iso               */
    char      bios[512];          /* path to a 512 KB Saturn BIOS ROM.
                                   * Required for authentic boot: the real IPL
                                   * builds the interrupt vector table and the
                                   * BIOS call table the game depends on. When
                                   * absent we fall back to HLE stubs, which is
                                   * faster to iterate on but not accurate. */

    gc_module modules[GC_MAX_MODULES];
    int       nmodules;

    char      err[256];
} game_config;

/* Load and validate a game.toml. Paths inside are resolved relative to the
 * config file's directory. Returns 0 on success. */
int gc_load(game_config *g, const char *toml_path);

const char *gc_compression_name(gc_compression c);
const char *gc_cpu_name(gc_cpu c);

#endif /* GAME_CONFIG_H */
