/* disc.h — Saturn disc image access. Game-agnostic.
 *
 * Handles the image formats Saturn rips actually come in:
 *   - .cue + .bin, single or multiple BIN files
 *   - bare .bin / .iso (2352 or 2048 byte sectors, auto-detected)
 *
 * Sector geometry is *detected*, not assumed. Real rips disagree with their
 * own cue sheets: pregaps may or may not be present in the BIN, and a single
 * ISO9660 volume routinely spans a MODE1 track, a MODE2 track (movies) and
 * CDDA tracks. Every accessor reports what it actually found.
 */
#ifndef DISC_H
#define DISC_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#define DISC_MAX_TRACKS 99

typedef enum {
    TRACK_MODE1 = 1,   /* 2048 user bytes at sector offset 16 */
    TRACK_MODE2 = 2,   /* form 1: 2048 @ 24; form 2: 2324 @ 24 */
    TRACK_AUDIO = 3,   /* raw 2352 PCM                        */
} track_mode;

typedef struct {
    int         num;
    track_mode  mode;
    uint32_t    sector_size;   /* bytes per sector in the file          */
    uint32_t    start_lba;     /* disc LBA of INDEX 01                  */
    uint32_t    pregap;        /* frames of pregap NOT stored in file   */
    int         file_index;    /* which BIN this track lives in         */
    uint32_t    file_lba;      /* sector offset of the track WITHIN its BIN */
} disc_track;

typedef struct {
    FILE       *fp;
    char        path[1024];
    uint64_t    size;
} disc_file;

typedef struct {
    /* One BIN per track is how most multi-track rips arrive (Fighting Vipers
     * ships 19), so this has to match the track ceiling, not a small guess. */
    disc_file   files[DISC_MAX_TRACKS];
    int         nfiles;
    disc_track  tracks[DISC_MAX_TRACKS];
    int         ntracks;

    /* Resolved LBA -> file mapping. */
    int         pregap_in_file;  /* 1 if pregaps occupy space in the BIN */
    uint32_t    total_sectors;   /* sectors physically present           */

    char        err[256];
} disc;

/* Open a .cue, .bin or .iso. Returns 0 on success. */
int  disc_open(disc *d, const char *path);
void disc_close(disc *d);

/* Read the 2048-byte user data of one logical sector. Returns 0 on success,
 * negative if the LBA is not backed by a readable data sector (past EOF, or
 * inside an audio track). *out_mode receives the detected track mode. */
int  disc_read_sector(disc *d, uint32_t lba, void *out2048, track_mode *out_mode);

/* Read `size` bytes starting at the beginning of `lba`, spanning sectors.
 * Returns bytes actually read. */
size_t disc_read(disc *d, uint32_t lba, void *out, size_t size);

/* Read a full raw 2352-byte sector (any track type). Returns 0 on success. */
int  disc_read_raw(disc *d, uint32_t lba, void *out2352);

/* Which track contains this LBA, or NULL. */
const disc_track *disc_track_for_lba(const disc *d, uint32_t lba);

/* ------------------------------------------------------------ ISO9660 */

typedef struct {
    char      path[256];
    uint32_t  lba;
    uint32_t  size;
    int       is_dir;
    int       readable;   /* 1 = MODE1/2 data sectors, 2 = raw audio-track  */
    int       in_audio;   /* stored in a CD-DA track as raw 2352-byte sectors */
} iso_entry;

typedef struct {
    char       volume_id[33];
    uint32_t   volume_sectors;
    uint32_t   root_lba;
    uint32_t   root_size;
    iso_entry *entries;
    int        nentries;
    int        cap;
} iso_fs;

/* Walk the ISO9660 filesystem on the data track. Returns 0 on success. */
int  iso_read(disc *d, iso_fs *fs);
void iso_free(iso_fs *fs);

/* Extract one file to a buffer the caller must free. Returns NULL on failure. */
void *iso_extract(disc *d, const iso_entry *e, size_t *out_size);

/* ------------------------------------------------------------- IP.BIN */

typedef struct {
    char     hardware_id[17];
    char     maker_id[17];
    char     product_no[11];
    char     version[7];
    char     release_date[9];
    char     device_info[9];
    char     area[11];
    char     peripherals[17];
    char     title[113];
    uint32_t ip_size;
    uint32_t stack_m, stack_s;
    uint32_t first_read_addr;
    uint32_t first_read_size;
    int      valid;
} saturn_ip;

/* Parse the Saturn IP.BIN header from sector 0. Returns 0 on success. */
int ip_read(disc *d, saturn_ip *ip);

#endif /* DISC_H */
