/* disc.c — Saturn disc image access. See disc.h. */
#include "disc.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static const uint8_t SYNC12[12] = {
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00
};

static uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void trim_trailing(char *s) {
    size_t n = strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\r' || s[n-1] == '\n')) s[--n] = 0;
}

static uint32_t msf_to_frames(int m, int s, int f) {
    return (uint32_t)(((m * 60) + s) * 75 + f);
}

/* ------------------------------------------------------- compressed CDDA --
 * A CUE may store Red Book tracks as MP3.  Those files are containers, not
 * 2352-byte CD sectors: their compressed byte length cannot define track LBAs
 * and their bytes cannot be sent directly to the SCSP CDDA input.  Keep disc
 * access sector-oriented by exposing decoded 44.1 kHz stereo s16 as ordinary
 * 2352-byte (588 stereo-frame) sectors.
 *
 * mpg123 is loaded at runtime so raw BIN/ISO users do not acquire a new link
 * dependency.  Windows title builds place libmpg123-0.dll beside the runner. */
#ifdef _WIN32
typedef void mpg123_handle;
typedef struct mp3_api {
    HMODULE dll;
    int (*init)(void);
    mpg123_handle *(*new_handle)(const char *, int *);
    void (*delete_handle)(mpg123_handle *);
    int (*open_fixed64)(mpg123_handle *, const char *, int, int);
    int (*getformat)(mpg123_handle *, long *, int *, int *);
    int (*scan)(mpg123_handle *);
    int64_t (*length64)(mpg123_handle *);
    int64_t (*seek64)(mpg123_handle *, int64_t, int);
    int (*read)(mpg123_handle *, void *, size_t, size_t *);
    int (*close)(mpg123_handle *);
    int ready;
} mp3_api;

static mp3_api mp3;

static int mp3_load_api(char *err, size_t err_size)
{
    if (mp3.ready) return 0;
    if (!mp3.dll) mp3.dll = LoadLibraryA("libmpg123-0.dll");
    if (!mp3.dll) {
        snprintf(err, err_size,
                 "MP3 CD tracks require libmpg123-0.dll beside SaturnRecomp");
        return -1;
    }
#define MP3_SYM(field, name) do {                                             \
    FARPROC proc = GetProcAddress(mp3.dll, name);                             \
    _Static_assert(sizeof(mp3.field) == sizeof(proc),                         \
                   "Windows function pointer size mismatch");                \
    memcpy(&mp3.field, &proc, sizeof(mp3.field));                             \
    if (!mp3.field) {                                                         \
        snprintf(err, err_size, "libmpg123-0.dll is missing %s", name);      \
        return -1;                                                            \
    }                                                                         \
} while (0)
    MP3_SYM(init,          "mpg123_init");
    MP3_SYM(new_handle,    "mpg123_new");
    MP3_SYM(delete_handle, "mpg123_delete");
    MP3_SYM(open_fixed64,  "mpg123_open_fixed64");
    MP3_SYM(getformat,     "mpg123_getformat");
    MP3_SYM(scan,          "mpg123_scan");
    MP3_SYM(length64,      "mpg123_length64");
    MP3_SYM(seek64,        "mpg123_seek64");
    MP3_SYM(read,          "mpg123_read");
    MP3_SYM(close,         "mpg123_close");
#undef MP3_SYM
    if (mp3.init() != 0) {
        snprintf(err, err_size, "libmpg123 initialization failed");
        return -1;
    }
    mp3.ready = 1;
    return 0;
}

static int mp3_open_file(disc *d, disc_file *df)
{
    mpg123_handle *mh;
    int decoder_error = 0, channels = 0, encoding = 0;
    int64_t frames;
    long rate = 0;

    if (mp3_load_api(d->err, sizeof(d->err)) != 0) return -1;
    mh = mp3.new_handle(NULL, &decoder_error);
    if (!mh) {
        snprintf(d->err, sizeof(d->err),
                 "cannot create MP3 decoder for %.180s (error %d)",
                 df->path, decoder_error);
        return -1;
    }
    /* MPG123_STEREO=2, MPG123_ENC_SIGNED_16=0xD0.  Native byte order on the
     * Windows targets is the little-endian order expected by SCSP CDDA. */
    if (mp3.open_fixed64(mh, df->path, 2, 0xD0) != 0 ||
        mp3.getformat(mh, &rate, &channels, &encoding) != 0 ||
        rate != 44100 || channels != 2 || encoding != 0xD0) {
        snprintf(d->err, sizeof(d->err),
                 "MP3 CD track must decode as 44100 Hz stereo s16: %.180s",
                 df->path);
        mp3.close(mh);
        mp3.delete_handle(mh);
        return -1;
    }
    if (mp3.scan(mh) != 0 || (frames = mp3.length64(mh)) < 0) {
        snprintf(d->err, sizeof(d->err),
                 "cannot index MP3 CD track: %.190s", df->path);
        mp3.close(mh);
        mp3.delete_handle(mh);
        return -1;
    }
    df->audio_decoder = mh;
    df->pcm_frames = (uint64_t)frames;
    df->pcm_pos = 0;
    /* A CD track occupies an integral number of 2352-byte sectors. */
    df->size = ((df->pcm_frames * 4u + 2351u) / 2352u) * 2352u;
    return 0;
}
#endif

static int path_is_mp3(const char *path)
{
    size_t n = strlen(path);
    return n >= 4 && path[n - 4] == '.' &&
           tolower((unsigned char)path[n - 3]) == 'm' &&
           tolower((unsigned char)path[n - 2]) == 'p' &&
           tolower((unsigned char)path[n - 1]) == '3';
}

/* ------------------------------------------------------------------ open */

static int open_bin(disc *d, const char *path)
{
    disc_file *df;
    if (d->nfiles >= (int)(sizeof(d->files)/sizeof(d->files[0]))) return -1;
    df = &d->files[d->nfiles];
    df->fp = fopen(path, "rb");
    if (!df->fp) return -1;
    snprintf(df->path, sizeof(df->path), "%s", path);
    fseek(df->fp, 0, SEEK_END);
    df->size = (uint64_t)ftell(df->fp);
    fseek(df->fp, 0, SEEK_SET);
    df->is_mp3 = path_is_mp3(path);
    d->nfiles++;
    return 0;
}

/* Probe a file for 2352-byte sectors by looking for the sync pattern. */
static uint32_t detect_sector_size(disc_file *df)
{
    uint8_t buf[12];
    if (fseek(df->fp, 0, SEEK_SET) != 0) return 2048;
    if (fread(buf, 1, 12, df->fp) != 12) return 2048;
    return memcmp(buf, SYNC12, 12) == 0 ? 2352u : 2048u;
}

/* Resolve a relative FILE reference in a cue against the cue's directory. */
static void resolve_rel(const char *cue_path, const char *name, char *out, size_t n)
{
    const char *slash = strrchr(cue_path, '/');
    const char *bslash = strrchr(cue_path, '\\');
    const char *cut = slash > bslash ? slash : bslash;
    int absolute = name[0] == '/' || name[0] == '\\' ||
                   (isalpha((unsigned char)name[0]) && name[1] == ':');
    if (absolute) {
        snprintf(out, n, "%s", name);
    } else if (cut) {
        size_t dirlen = (size_t)(cut - cue_path) + 1;
        if (dirlen >= n) dirlen = n - 1;
        memcpy(out, cue_path, dirlen);
        snprintf(out + dirlen, n - dirlen, "%s", name);
    } else {
        snprintf(out, n, "%s", name);
    }
}

static int parse_cue(disc *d, const char *cue_path)
{
    FILE *f = fopen(cue_path, "r");
    char line[1024];
    int cur_track = -1;
    uint32_t pending_pregap = 0;

    if (!f) { snprintf(d->err, sizeof(d->err), "cannot open cue: %s", cue_path); return -1; }

    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        if (strncmp(p, "FILE", 4) == 0) {
            char name[512] = {0}, full[1024];
            const char *q = strchr(p, '"');
            if (q) {
                const char *e = strchr(q + 1, '"');
                if (e && (size_t)(e - q - 1) < sizeof(name)) {
                    memcpy(name, q + 1, (size_t)(e - q - 1));
                    name[e - q - 1] = 0;
                }
            }
            if (!name[0]) continue;
            resolve_rel(cue_path, name, full, sizeof(full));
            if (open_bin(d, full) != 0) {
                snprintf(d->err, sizeof(d->err), "cue references missing file: %.200s", full);
                fclose(f);
                return -1;
            }
        } else if (strncmp(p, "TRACK", 5) == 0) {
            int num; char modestr[32] = {0};
            if (sscanf(p, "TRACK %d %31s", &num, modestr) == 2 &&
                d->ntracks < DISC_MAX_TRACKS) {
                disc_track *t = &d->tracks[d->ntracks];
                memset(t, 0, sizeof(*t));
                t->num        = num;
                t->file_index = d->nfiles ? d->nfiles - 1 : 0;
                t->pregap     = pending_pregap;
                pending_pregap = 0;
                if (strncmp(modestr, "MODE1", 5) == 0)      t->mode = TRACK_MODE1;
                else if (strncmp(modestr, "MODE2", 5) == 0) t->mode = TRACK_MODE2;
                else                                        t->mode = TRACK_AUDIO;
                t->sector_size = strstr(modestr, "2048") ? 2048u : 2352u;
                cur_track = d->ntracks;
                d->ntracks++;
            }
        } else if (strncmp(p, "PREGAP", 6) == 0) {
            int m, s, fr;
            if (sscanf(p, "PREGAP %d:%d:%d", &m, &s, &fr) == 3) {
                uint32_t g = msf_to_frames(m, s, fr);
                if (cur_track >= 0 && d->tracks[cur_track].start_lba == 0 &&
                    d->tracks[cur_track].pregap == 0)
                    d->tracks[cur_track].pregap = g;
                else
                    pending_pregap = g;
            }
        } else if (strncmp(p, "INDEX", 5) == 0) {
            int idx, m, s, fr;
            if (sscanf(p, "INDEX %d %d:%d:%d", &idx, &m, &s, &fr) == 4 &&
                idx == 1 && cur_track >= 0)
                d->tracks[cur_track].start_lba = msf_to_frames(m, s, fr);
        }
    }
    fclose(f);
    return d->ntracks ? 0 : -1;
}

/* Sum of pregaps declared before (and including) the track containing lba. */
static uint32_t pregap_before(const disc *d, uint32_t lba)
{
    uint32_t sum = 0;
    for (int i = 0; i < d->ntracks; i++) {
        if (d->tracks[i].start_lba > lba) break;
        sum += d->tracks[i].pregap;
    }
    return sum;
}

/* Resolve every track's ABSOLUTE disc LBA.
 *
 * A cue's INDEX is relative to the FILE it appears in, not to the disc. With a
 * single BIN holding every track that distinction vanishes -- file base 0 makes
 * the two identical -- which is why reading INDEX straight into start_lba
 * worked until now. With one BIN per track, every track declares
 * INDEX 01 00:00:00 and they would all claim LBA 0, stacking the whole disc on
 * top of itself.
 *
 * So: walk the files in cue order, keeping a running disc position. A file
 * holding exactly one track contributes that track at the running position
 * (plus any pregap, which by definition is not stored in the file). A file
 * holding several keeps its INDEX values as offsets within that file. */
static void layout_tracks(disc *d)
{
    uint32_t running = 0;
    int fi, i;

    for (fi = 0; fi < d->nfiles; fi++) {
        uint32_t base = running, ss = 2352u;
        int first = -1, ntf = 0;

        for (i = 0; i < d->ntracks; i++)
            if (d->tracks[i].file_index == fi) {
                if (first < 0) first = i;
                ntf++;
            }
        if (first < 0) continue;
        if (d->tracks[first].sector_size) ss = d->tracks[first].sector_size;

        for (i = 0; i < d->ntracks; i++) {
            if (d->tracks[i].file_index != fi) continue;
            if (ntf == 1) {
                /* Sole track in its file: it begins at the top of that file. */
                base += d->tracks[i].pregap;
                d->tracks[i].file_lba  = 0;
                d->tracks[i].start_lba = base;
            } else {
                /* Several tracks share this file, so the cue's INDEX is
                 * already the offset within it. */
                d->tracks[i].file_lba   = d->tracks[i].start_lba;
                d->tracks[i].start_lba += base;
            }
        }
        running = base + (uint32_t)(d->files[fi].size / ss);
    }
    d->total_sectors = running;
}

int disc_open(disc *d, const char *path)
{
    size_t n = strlen(path);
    memset(d, 0, sizeof(*d));

    if (n > 4 && (strcmp(path + n - 4, ".cue") == 0 || strcmp(path + n - 4, ".CUE") == 0)) {
        if (parse_cue(d, path) != 0) return -1;
    } else {
        disc_track *t;
        if (open_bin(d, path) != 0) {
            snprintf(d->err, sizeof(d->err), "cannot open image: %s", path);
            return -1;
        }
        t = &d->tracks[0];
        memset(t, 0, sizeof(*t));
        t->num         = 1;
        t->mode        = TRACK_MODE1;
        t->sector_size = detect_sector_size(&d->files[0]);
        t->start_lba   = 0;
        d->ntracks     = 1;
    }

    /* Decode compressed CDDA geometry before laying out files.  Track start
     * addresses must derive from decoded duration, never compressed bytes. */
    for (int i = 0; i < d->nfiles; i++) {
        if (!d->files[i].is_mp3) continue;
#ifdef _WIN32
        if (mp3_open_file(d, &d->files[i]) != 0) return -1;
#else
        snprintf(d->err, sizeof(d->err),
                 "MP3 CD tracks are not supported by this build: %.180s",
                 d->files[i].path);
        return -1;
#endif
    }

    /* Verify the declared sector size against the file, and total up.
     *
     * CD-DA is exempt: detect_sector_size() probes for the MODE1 sync pattern,
     * and audio tracks have no sync, so it reports 2048 for every one of them.
     * Redbook audio is 2352 raw bytes per sector by definition -- letting the
     * probe demote it corrupts both the LBA arithmetic and the samples. */
    for (int i = 0; i < d->nfiles; i++) {
        uint32_t ss = detect_sector_size(&d->files[i]);
        for (int t = 0; t < d->ntracks; t++)
            if (d->tracks[t].file_index == i &&
                d->tracks[t].mode != TRACK_AUDIO &&
                d->tracks[t].sector_size != 2048)
                d->tracks[t].sector_size = ss;
    }
    layout_tracks(d);

    /* Decide whether pregaps occupy space in the BIN. A single-BIN rip either
     * stores them (file sectors == last track end) or omits them (file is
     * short by exactly the summed pregap). Compare against the ISO volume
     * size later; for now assume omitted only if the cue declares any. */
    d->pregap_in_file = 1;
    {
        uint32_t total_pregap = 0;
        for (int i = 0; i < d->ntracks; i++) total_pregap += d->tracks[i].pregap;
        if (total_pregap) d->pregap_in_file = 0;   /* refined by iso_read() */
    }
    return 0;
}

void disc_close(disc *d)
{
    for (int i = 0; i < d->nfiles; i++) {
#ifdef _WIN32
        if (d->files[i].audio_decoder && mp3.ready) {
            mp3.close((mpg123_handle *)d->files[i].audio_decoder);
            mp3.delete_handle((mpg123_handle *)d->files[i].audio_decoder);
        }
#endif
        if (d->files[i].fp) fclose(d->files[i].fp);
    }
    memset(d, 0, sizeof(*d));
}

const disc_track *disc_track_for_lba(const disc *d, uint32_t lba)
{
    const disc_track *best = NULL;
    for (int i = 0; i < d->ntracks; i++)
        if (d->tracks[i].start_lba <= lba &&
            (!best || d->tracks[i].start_lba >= best->start_lba))
            best = &d->tracks[i];
    return best;
}

/* Disc LBA -> sector offset inside the track's own BIN.
 *
 * With one BIN per track the two are wildly different: track 2 of Fighting
 * Vipers starts at disc LBA 35244, but at offset 0 of its own file. Using the
 * disc LBA as a file offset seeks 82 MB into a 26 MB file and fails every
 * read, which is why CD audio produced nothing.
 *
 * Single-BIN images keep the original pregap-based path untouched -- there the
 * cue's INDEX really is the file offset, and that case is already known good. */
static int64_t resolve_file_lba(const disc *d, const disc_track *t, uint32_t lba)
{
    if (d->nfiles > 1) {
        int64_t rel = (int64_t)lba - (int64_t)t->start_lba;
        if (rel < 0) return -1;
        return (int64_t)t->file_lba + rel;
    }
    {
        uint32_t file_lba = lba;
        if (!d->pregap_in_file) {
            uint32_t g = pregap_before(d, lba);
            if (g > file_lba) return -1;
            file_lba -= g;
        }
        return (int64_t)file_lba;
    }
}

int disc_read_sector(disc *d, uint32_t lba, void *out2048, track_mode *out_mode)
{
    const disc_track *t = disc_track_for_lba(d, lba);
    uint8_t raw[2352];
    uint64_t off;
    uint32_t file_lba;
    disc_file *df;
    unsigned data_off;

    if (!t) return -1;
    if (out_mode) *out_mode = t->mode;
    if (t->mode == TRACK_AUDIO) return -2;

    {
        int64_t fl = resolve_file_lba(d, t, lba);
        if (fl < 0) return -1;
        file_lba = (uint32_t)fl;
    }

    df = &d->files[t->file_index];
    if (t->sector_size == 2048) {
        off = (uint64_t)file_lba * 2048u;
        if (off + 2048 > df->size) return -1;
        if (fseek(df->fp, (long)off, SEEK_SET) != 0) return -1;
        return fread(out2048, 1, 2048, df->fp) == 2048 ? 0 : -1;
    }

    off = (uint64_t)file_lba * 2352u;
    if (off + 2352 > df->size) return -1;
    if (fseek(df->fp, (long)off, SEEK_SET) != 0) return -1;
    if (fread(raw, 1, 2352, df->fp) != 2352) return -1;

    /* Detect the real mode from the sector itself; cue sheets lie. */
    if (memcmp(raw, SYNC12, 12) != 0) {
        if (out_mode) *out_mode = TRACK_AUDIO;
        return -2;                 /* audio / not a data sector */
    }
    switch (raw[15]) {
    case 1:  data_off = 16; if (out_mode) *out_mode = TRACK_MODE1; break;
    case 2:  data_off = 24; if (out_mode) *out_mode = TRACK_MODE2; break;
    default: return -2;
    }
    memcpy(out2048, raw + data_off, 2048);
    return 0;
}

int disc_read_raw(disc *d, uint32_t lba, void *out2352)
{
    const disc_track *t = disc_track_for_lba(d, lba);
    uint32_t file_lba;
    disc_file *df;
    uint64_t off;

    if (!t || t->sector_size != 2352) return -1;
    {
        int64_t fl = resolve_file_lba(d, t, lba);
        if (fl < 0) return -1;
        file_lba = (uint32_t)fl;
    }
    df  = &d->files[t->file_index];
    if (df->is_mp3) {
#ifdef _WIN32
        uint8_t *out = (uint8_t *)out2352;
        uint64_t target = (uint64_t)file_lba * 588u;
        size_t done = 0;
        int rc = 0;
        if (!df->audio_decoder || target >= df->pcm_frames) return -1;
        if (df->pcm_pos != target) {
            int64_t at = mp3.seek64((mpg123_handle *)df->audio_decoder,
                                    (int64_t)target, SEEK_SET);
            if (at < 0) return -1;
            df->pcm_pos = (uint64_t)at;
        }
        while (done < 2352u && df->pcm_pos < df->pcm_frames) {
            size_t got = 0;
            rc = mp3.read((mpg123_handle *)df->audio_decoder,
                          out + done, 2352u - done, &got);
            done += got;
            df->pcm_pos += got / 4u;
            if (rc == -11) continue;        /* MPG123_NEW_FORMAT */
            if (rc == -12) break;           /* MPG123_DONE       */
            if (rc != 0) return -1;
            if (!got) break;
        }
        memset(out + done, 0, 2352u - done);
        return 0;
#else
        return -1;
#endif
    }
    off = (uint64_t)file_lba * 2352u;
    if (off + 2352 > df->size) return -1;
    if (fseek(df->fp, (long)off, SEEK_SET) != 0) return -1;
    return fread(out2352, 1, 2352, df->fp) == 2352 ? 0 : -1;
}

size_t disc_read(disc *d, uint32_t lba, void *out, size_t size)
{
    uint8_t *o = (uint8_t *)out;
    size_t done = 0;
    while (done < size) {
        uint8_t sec[2048];
        size_t chunk = size - done < 2048 ? size - done : 2048;
        if (disc_read_sector(d, lba + (uint32_t)(done / 2048), sec, NULL) != 0)
            break;
        memcpy(o + done, sec, chunk);
        done += chunk;
    }
    return done;
}

/* ---------------------------------------------------------------- ISO9660 */

static void iso_push(iso_fs *fs, const iso_entry *e)
{
    if (fs->nentries == fs->cap) {
        int nc = fs->cap ? fs->cap * 2 : 128;
        iso_entry *ne = (iso_entry *)realloc(fs->entries, (size_t)nc * sizeof(*ne));
        if (!ne) return;
        fs->entries = ne;
        fs->cap = nc;
    }
    fs->entries[fs->nentries++] = *e;
}

static void iso_walk(disc *d, iso_fs *fs, uint32_t lba, uint32_t len, const char *prefix, int depth)
{
    uint8_t *dir;
    uint32_t nsec, o;

    if (depth > 8 || len == 0 || len > (1u << 20)) return;
    nsec = (len + 2047) / 2048;
    dir = (uint8_t *)malloc((size_t)nsec * 2048);
    if (!dir) return;
    for (uint32_t i = 0; i < nsec; i++)
        if (disc_read_sector(d, lba + i, dir + (size_t)i * 2048, NULL) != 0)
            memset(dir + (size_t)i * 2048, 0, 2048);

    o = 0;
    while (o < len) {
        uint8_t rlen = dir[o];
        uint8_t flags, nlen;
        iso_entry e;
        uint32_t ext, size;

        if (rlen == 0) {                     /* padding to next sector */
            o = (o / 2048 + 1) * 2048;
            continue;
        }
        if (o + rlen > len) break;

        ext   = rd_le32(dir + o + 2);
        size  = rd_le32(dir + o + 10);
        flags = dir[o + 25];
        nlen  = dir[o + 32];

        if (!(nlen == 1 && (dir[o + 33] == 0 || dir[o + 33] == 1))) {
            char name[256];
            uint32_t su_off, su_len;
            size_t cn = nlen < sizeof(name) - 1 ? nlen : sizeof(name) - 1;
            memcpy(name, dir + o + 33, cn);
            name[cn] = 0;
            { char *sc = strchr(name, ';'); if (sc) *sc = 0; }

            memset(&e, 0, sizeof(e));
            snprintf(e.path, sizeof(e.path), "%.180s/%.60s", prefix, name);
            e.lba    = ext;
            e.size   = size;
            e.is_dir = (flags & 2) ? 1 : 0;
            e.unit_size  = dir[o + 26];
            e.gap_size   = dir[o + 27];
            e.attributes = (uint8_t)(flags & 0x02);

            /* The CD block record carries the XA file number, not the ISO
             * directory index.  Ordinary MODE1 records have no XA system-use
             * data and therefore retain file_num=0 from the memset above. */
            su_off = 33u + ((uint32_t)nlen | 1u);
            su_len = rlen > su_off ? (uint32_t)rlen - su_off : 0;
            if (su_len >= 14u && dir[o + su_off + 6] == 'X' &&
                                  dir[o + su_off + 7] == 'A') {
                e.attributes |= (uint8_t)(dir[o + su_off + 4] & 0xF8);
                e.file_num = dir[o + su_off + 8];
            }
            iso_push(fs, &e);

            if (e.is_dir)
                iso_walk(d, fs, ext, size, e.path, depth + 1);
        }
        o += rlen;
    }
    free(dir);
}

int iso_read(disc *d, iso_fs *fs)
{
    uint8_t pvd[2048];
    memset(fs, 0, sizeof(*fs));

    if (disc_read_sector(d, 16, pvd, NULL) != 0) {
        snprintf(d->err, sizeof(d->err), "cannot read PVD at LBA 16");
        return -1;
    }
    if (memcmp(pvd + 1, "CD001", 5) != 0) {
        snprintf(d->err, sizeof(d->err), "no ISO9660 signature at LBA 16");
        return -1;
    }

    memcpy(fs->volume_id, pvd + 40, 32);
    fs->volume_id[32] = 0;
    trim_trailing(fs->volume_id);
    fs->volume_sectors = rd_le32(pvd + 80);
    fs->root_lba       = rd_le32(pvd + 156 + 2);
    fs->root_size      = rd_le32(pvd + 156 + 10);

    /* Now that the volume size is known, settle the pregap question: if the
     * image is short by roughly the summed pregap, the pregaps are not stored.
     * Tolerance of a few sectors covers rips that trim the run-out. */
    {
        uint32_t total_pregap = 0, phys = d->total_sectors;
        for (int i = 0; i < d->ntracks; i++) total_pregap += d->tracks[i].pregap;
        if (total_pregap) {
            long diff = (long)fs->volume_sectors - (long)phys;
            long slack = diff - (long)total_pregap;
            if (slack < 0) slack = -slack;
            d->pregap_in_file = (slack <= 4) ? 0 : 1;
        }
    }

    iso_walk(d, fs, fs->root_lba, fs->root_size, "", 0);

    /* Mark which entries are actually backed by readable data sectors. */
    for (int i = 0; i < fs->nentries; i++) {
        uint8_t probe[2048];
        track_mode tm;
        int rc = disc_read_sector(d, fs->entries[i].lba, probe, &tm);
        if (rc == 0) {
            fs->entries[i].readable = 1;
        } else if (rc == -2) {
            /* Sega stores large streamed media (Sofdec .SFD and friends) as
             * CD-DA tracks: raw 2352-byte sectors with no sync or ECC, so the
             * drive can stream them at full rate. The data is present; it just
             * needs raw access. */
            uint8_t raw[2352];
            fs->entries[i].in_audio = 1;
            fs->entries[i].readable = (disc_read_raw(d, fs->entries[i].lba, raw) == 0) ? 2 : 0;
        } else {
            fs->entries[i].readable = 0;
        }
    }
    return 0;
}

void iso_free(iso_fs *fs)
{
    free(fs->entries);
    memset(fs, 0, sizeof(*fs));
}

void *iso_extract(disc *d, const iso_entry *e, size_t *out_size)
{
    uint8_t *buf;
    size_t got;
    if (e->is_dir || e->size == 0) return NULL;
    buf = (uint8_t *)malloc(e->size);
    if (!buf) return NULL;
    if (e->in_audio) {
        /* Raw 2352-byte sectors, no header. */
        size_t done = 0;
        uint32_t sec = 0;
        while (done < e->size) {
            uint8_t raw[2352];
            size_t chunk;
            if (disc_read_raw(d, e->lba + sec, raw) != 0) break;
            chunk = e->size - done < 2352 ? e->size - done : 2352;
            memcpy(buf + done, raw, chunk);
            done += chunk;
            sec++;
        }
        got = done;
    } else {
        got = disc_read(d, e->lba, buf, e->size);
    }
    if (got != e->size) { free(buf); return NULL; }
    if (out_size) *out_size = e->size;
    return buf;
}

/* ----------------------------------------------------------------- IP.BIN */

static void copy_field(char *dst, const uint8_t *src, size_t n)
{
    memcpy(dst, src, n);
    dst[n] = 0;
    trim_trailing(dst);
}

int ip_read(disc *d, saturn_ip *ip)
{
    uint8_t s[2048];
    memset(ip, 0, sizeof(*ip));
    if (disc_read_sector(d, 0, s, NULL) != 0) return -1;

    copy_field(ip->hardware_id,  s + 0x00, 16);
    copy_field(ip->maker_id,     s + 0x10, 16);
    copy_field(ip->product_no,   s + 0x20, 10);
    copy_field(ip->version,      s + 0x2A, 6);
    copy_field(ip->release_date, s + 0x30, 8);
    copy_field(ip->device_info,  s + 0x38, 8);
    copy_field(ip->area,         s + 0x40, 10);
    copy_field(ip->peripherals,  s + 0x50, 16);
    copy_field(ip->title,        s + 0x60, 112);

    ip->ip_size         = rd_be32(s + 0xE0);
    ip->stack_m         = rd_be32(s + 0xE8);
    ip->stack_s         = rd_be32(s + 0xEC);
    ip->first_read_addr = rd_be32(s + 0xF0);
    ip->first_read_size = rd_be32(s + 0xF4);
    ip->valid = (memcmp(s, "SEGA SEGASATURN", 15) == 0);
    return ip->valid ? 0 : -1;
}
