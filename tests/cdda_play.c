/* cdda_play.c -- does a CD-DA track actually reach the audio mix?
 *
 * "The music does not play" has three plausible causes that look identical
 * from the outside: the disc layer cannot read the audio track, the CD block
 * never recognises the range as audio and streams nothing, or the SCSP drops
 * what it is handed. Getting a game far enough to start its soundtrack needs
 * menu input, so this drives the chain directly instead: open a real disc,
 * hand the CD block a PlayDisc over an audio range, tick it, and listen.
 *
 * Point it at a cue with at least one CD-DA track:
 *     cdda_play.exe "discs/sonic3d/Sonic 3D Blast (CDDA).cue"
 */
#include "saturn.h"
#include "disc.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static saturn S;
static disc   D;
static int    fails;

static void ck(const char *what, int ok, const char *detail)
{
    printf("  %-4s %-46s %s\n", ok ? "ok" : "FAIL", what, detail ? detail : "");
    if (!ok) fails++;
}

int main(int argc, char **argv)
{
    const char *img = argc > 1 ? argv[1]
                    : "discs/sonic3d/Sonic 3D Blast (CDDA).cue";
    const disc_track *aud = NULL;
    char msg[160];
    int i;

    printf("CD-DA playback path (%s)\n", img);

    if (disc_open(&D, img) != 0) {
        printf("  SKIP cannot open image: %s\n", D.err);
        return 0;                       /* not a failure: image may be absent */
    }

    /* 1. the disc layer must expose an audio track and read raw sectors. */
    for (i = 0; i < D.ntracks; i++)
        if (D.tracks[i].mode == TRACK_AUDIO) { aud = &D.tracks[i]; break; }
    if (!aud) { printf("  SKIP no CD-DA track on this image\n"); disc_close(&D); return 0; }

    snprintf(msg, sizeof msg, "track %d at LBA %u, %u-byte sectors",
             aud->num, aud->start_lba, aud->sector_size);
    ck("disc exposes a CD-DA track", 1, msg);
    ck("CD-DA sectors are 2352 bytes", aud->sector_size == 2352u, NULL);

    {   /* Find a sector with real signal -- the head of a track is silence. */
        uint8_t raw[2352];
        uint32_t lba = aud->start_lba + 75u * 10u;   /* ~10 s in */
        int rc = disc_read_raw(&D, lba, raw);
        int nz = 0, k;
        for (k = 0; k < 2352; k++) if (raw[k]) nz++;
        snprintf(msg, sizeof msg, "lba %u, %d/2352 non-zero", lba, nz);
        ck("raw audio sector reads back", rc == 0, msg);
        ck("audio sector carries signal", rc == 0 && nz > 512, NULL);
    }

    /* 2. the CD block must treat that range as playback and stream it. */
    memset(&S, 0, sizeof S);
    scsp_reset(&S);
    cdb_init(&S, &D, NULL);

    S.cd.playing      = 1;
    S.cd.cdda_play    = 1;
    S.cd.fad          = aud->start_lba + 75u * 10u + 150u;
    S.cd.play_end_fad = S.cd.fad + 200u;

    /* Master volume open, or every slot mixes to nothing. */
    S.scsp_reg[0x400 >> 1] = 0x000Fu;
    /* Ymir routes CDDA through EXTS using the EFSDL/EFPAN controls stored in
     * slots 16 and 17: hard-left EXTS0, hard-right EXTS1, full send. */
    S.scsp_reg[(16u * 0x20u + 0x16u) >> 1] = 0x00FFu;
    S.scsp_reg[(17u * 0x20u + 0x16u) >> 1] = 0x00EFu;

    /* A full SCSP ring applies backpressure. The drive must retry the SAME
     * sector later; advancing FAD here used to skip 1/75 s of the track on
     * every rejection and sounded like an initial fast-forward. */
    {
        uint32_t held_fad = S.cd.fad;
        S.cdda_rp = 0;
        S.cdda_wp = CDDA_RING - 2352u;
        cdb_tick(&S);
        ck("CD-DA backpressure does not skip a sector", S.cd.fad == held_fad, NULL);
        S.cdda_wp = S.cdda_rp = 0;
        S.cdda_ready = 0;
    }

    /* Tick and render alternately, the way the scheduler does. The ring holds
     * only 8 sectors, so ticking in a tight loop with nothing draining it just
     * laps the read pointer -- which measures the test, not the emulator. */
    {
        uint32_t start_fad = S.cd.fad;
        int      nonsilent = 0;
        int32_t  peak = 0;
        int      field, k;

        /* Sonic R's countdown uses the all-ones "keep current playback"
         * command immediately after choosing its short audio track. */
        uint32_t end_fad = S.cd.play_end_fad;
        S.cd.play_repeat = 0;
        S.cd.cmd_stage[0] = 0x10FF;
        S.cd.cmd_stage[1] = S.cd.cmd_stage[2] = S.cd.cmd_stage[3] = 0xFFFF;
        cdb_execute(&S);
        ck("keep-current command preserves CDDA", S.cd.playing && S.cd.cdda_play, NULL);
        ck("keep-current command preserves position", S.cd.fad == start_fad, NULL);
        ck("keep-current command preserves track end", S.cd.play_end_fad == end_fad, NULL);
        ck("keep-current command preserves repeat count", S.cd.play_repeat == 0, NULL);

        for (field = 0; field < 40; field++) {
            cdb_tick(&S);
            for (k = 0; k < 735; k++) {          /* one field at 44.1 kHz */
                int16_t l = 0, r = 0;
                scsp_render(&S, &l, &r);
                if (l || r) nonsilent++;
                if ( l > peak) peak =  l;
                if (-l > peak) peak = -l;
            }
        }

        snprintf(msg, sizeof msg, "%u sector(s) streamed", S.cd.fad - start_fad);
        ck("CD block streams audio sectors", S.cd.fad > start_fad, msg);

        snprintf(msg, sizeof msg, "%d/%d non-silent, peak %d",
                 nonsilent, 40 * 735, (int)peak);
        ck("CD-DA reaches the audio mix", nonsilent > 1000 && peak > 256, msg);
    }

    disc_close(&D);
    printf(fails ? "FAIL: %d CD-DA check(s) failed\n" : "PASS: CD-DA playback checks\n",
           fails);
    return fails ? 1 : 0;
}
