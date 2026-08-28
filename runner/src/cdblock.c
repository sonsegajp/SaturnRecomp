/* cdblock.c — Saturn CD block (the SH-1 subsystem), high-level.
 *
 * The host never talks to the drive. It writes a four-word command packet into
 * CR1..CR4, the CD block executes it and writes a four-word response back into
 * the same registers, then raises flags in HIRQ. Bulk data comes back through
 * the data transfer register at 0x25890000.
 *
 * This is an HLE implementation: it answers the command protocol using the
 * ISO9660 layer we already have, rather than emulating the SH-1 and its
 * firmware. That is the right trade here -- the protocol is the contract the
 * BIOS and the game are written against, and it is far better specified than
 * the firmware's internal behaviour.
 *
 * Power-on behaviour matters and is easy to get wrong: CR1..CR4 initially spell
 * "CDBLOCK", and the BIOS spins reading them until that signature is replaced
 * by a real status response. Until then it never issues a single command --
 * which is exactly the 170,000-read stall we measured before this existed.
 */
#include "saturn.h"
#include "disc.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ constants */

/* CR1 high byte: drive status. */
#define ST_BUSY     0x00
#define ST_PAUSE    0x01
#define ST_STANDBY  0x02
#define ST_PLAY     0x03
#define ST_SEEK     0x04
#define ST_SCAN     0x05
#define ST_OPEN     0x06
#define ST_NODISC   0x07
#define ST_RETRY    0x08
#define ST_ERROR    0x09
#define ST_FATAL    0x0A
#define ST_PERIODIC 0x20   /* OR'd in for an unsolicited status report */
#define ST_XFERREQ  0x40   /* kStatusFlagXferRequest: data staged, host may read */
#define ST_WAIT     0x80
#define ST_REJECT   0xFF

/* HIRQ flags. */
#define HIRQ_CMOK   0x0001  /* command dispatch possible   */
#define HIRQ_DRDY   0x0002  /* data transfer ready         */
#define HIRQ_CSCT   0x0004  /* sector stored               */
#define HIRQ_BFUL   0x0008  /* buffer full                 */
#define HIRQ_PEND   0x0010  /* play end                    */
#define HIRQ_DCHG   0x0020  /* disc changed                */
#define HIRQ_ESEL   0x0040  /* selector settings done      */
#define HIRQ_EHST   0x0080  /* host I/O done               */
#define HIRQ_ECPY   0x0100  /* duplication/move done       */
#define HIRQ_EFLS   0x0200  /* file system done            */
#define HIRQ_SCDQ   0x0400  /* subcode Q updated           */
#define HIRQ_MPED   0x0800  /* MPEG processing finished    */

/* Register indices into saturn.cdb_reg (word-addressed from 0x25890000). */
#define R_DTR   (0x00 >> 1)
#define R_HIRQ  (0x08 >> 1)
#define R_HMSK  (0x0C >> 1)
#define R_CR1   (0x18 >> 1)
#define R_CR2   (0x1C >> 1)
#define R_CR3   (0x20 >> 1)
#define R_CR4   (0x24 >> 1)

/* First FAD of the data area. Frame addresses are LBA + 150. */
#define FAD_BASE 150u

/* -------------------------------------------------------------- helpers */

/* Setting a flag that the host has left unmasked asserts the CD block's
 * interrupt line, which reaches the SH-2 as SCU external interrupt 00. This is
 * how a title that issues a command and then sleeps gets woken; polling-only
 * titles simply never unmask, so raising it unconditionally is wrong. */
/* Ymir CDBlock::UpdateInterrupts (cdblock.cpp:1225): the CD block's interrupt
 * line is LEVEL-driven off (HIRQ & HIRQMASK), so it must be re-evaluated on
 * every event that can change either side -- a flag being set, the host
 * clearing flags, and the host writing HIRQMASK.  That last one is the one
 * that was missing: HIRQMASK writes were being dropped on the floor by the
 * bus, so HMSK read 0 for the whole run and this test could never be true. */
void cdb_update_interrupts(saturn *s)
{
    if (s->cdb_reg[R_HIRQ] & s->cdb_reg[R_HMSK])
        scu_raise(s, 16);
}

static void hirq_set(saturn *s, uint16_t bits)
{
    /* SATURN_PENDLOG: PEND drives the BIOS's boot decision at ROM 0x2D72 --
     * set means "re-read the IP", clear means "carry on and boot". Ymir takes
     * the clear branch there and we take the set one, so knowing exactly who
     * raises the bit, and when, is the whole question. */
    if ((bits & HIRQ_PEND) && getenv("SATURN_PENDLOG"))
        printf("[pend] raised at cy=%llu pc=%08X\n",
               (unsigned long long)s->master.cycles, s->master.pc);
    s->cdb_reg[R_HIRQ] |= bits;
    cdb_update_interrupts(s);
}

/* The drive is asynchronous by DEFAULT now. Satisfying a read inside the
 * command handler raised PEND ~2M cycles before the BIOS tested it at ROM
 * 0x2D72, so the BIOS re-read the IP forever instead of booting. With the read
 * deferred to the paced streamer -- one sector per CDB_SECTOR_CYCLES/read_speed
 * -- PEND lands after that test, exactly as Ymir's CheckPlayEnd does, and the
 * BIOS goes on to issue ChangeDirectory/GetFileSystemScope/GetFileInfo/ReadFile
 * and load the game off the disc.
 *
 * SATURN_SYNCDRIVE=1 restores the old burst-inside-the-command behaviour. */
static int asyncdrv(void)
{
    static int v = -1;
    if (v < 0) v = getenv("SATURN_SYNCDRIVE") == NULL;
    return v;
}

static int cdlog_on = -1;
static int cdlog(void) {
    if (cdlog_on < 0) cdlog_on = getenv("SATURN_CDLOG") != NULL;
    return cdlog_on;
}

static void respond(saturn *s, uint16_t c1, uint16_t c2, uint16_t c3, uint16_t c4)
{
    if (cdlog())
        printf("[cd<] %04X %04X %04X %04X\n", c1, c2, c3, c4);
    /* [TEST] Force the PERI bit into every reply's status byte. */
    if (getenv("SATURN_ALLPERI")) c1 |= 0x2000;
    s->cdb_reg[R_CR1] = c1;
    s->cdb_reg[R_CR2] = c2;
    s->cdb_reg[R_CR3] = c3;
    s->cdb_reg[R_CR4] = c4;
    /* A command reply stays in CR1..CR4 until the host has read it. The
     * periodic report must not overwrite it in the meantime or the host reads
     * a drive-status block where its answer should have been. */
    /* Who is putting a bare (0x20-less) status back into CR1 after the
     * periodic report has set it? Log the first few such writes. */
    if (s->cd.periodic_count > 0 && ((c1 >> 8) & 0x20) == 0 && s->nbadcr < 8) {
        s->badcr_pc[s->nbadcr]  = s->master.pc;
        s->badcr_val[s->nbadcr] = c1;
        s->nbadcr++;
    }
    s->cd.resp_pending = 1;
    s->cd.resp_fresh   = 1;
    hirq_set(s, HIRQ_CMOK);
}

/* The standard status response: status, then the current play position. */
/* Which partition a command targets: CR3's high byte on most commands,
 * cd_conn for the streaming tick. Clamped to the valid range. */
#define PNUM(cr3)  (((unsigned)((cr3) >> 8)) < CD_NUM_PARTS ? ((unsigned)((cr3) >> 8)) : 0u)

static void respond_status(saturn *s)
{
    cdblock *cd = &s->cd;
    uint8_t st = cd->status;
    /* [TEST] The IPL demands the PERI bit in the status byte. */
    if (getenv("SATURN_ALWAYSPERI")) st |= ST_PERIODIC;
    /* Ymir ReportCDStatus: RR0 = status<<8 | flags<<4 | repeatCount.
     * cd->flag already carries the flags pre-shifted into bits 7-4; the
     * repeat count in bits 3-0 was simply missing. */
    respond(s,
            (uint16_t)((uint16_t)st << 8) | (uint16_t)(cd->flag & 0xF0)
                                          | (uint16_t)(cd->play_rptcnt & 0x0F),
            (uint16_t)(((uint16_t)cd->ctrl << 8) | cd->track),
            (uint16_t)(((uint16_t)cd->index << 8) | ((cd->fad >> 16) & 0xFF)),
            (uint16_t)(cd->fad & 0xFFFF));
}

/* Hand the host a buffer to read out through the data transfer register. */
static void cd_set_position(saturn *s, uint32_t fad);

static void begin_transfer(saturn *s, const void *src, uint32_t bytes)
{
    cdblock *cd = &s->cd;
    if (bytes > (CD_PART_SECTORS * 2048u)) bytes = (CD_PART_SECTORS * 2048u);
    memcpy(cd->xfer, src, bytes);
    if (getenv("SATURN_XFER")) {
        const uint8_t *q = (const uint8_t *)src;
        printf("[xfer] %u bytes:", bytes);
        for (unsigned k = 0; k < 12 && k < bytes; k++) printf(" %02X", q[k]);
        printf("  |");
        for (unsigned k = 0; k < 12 && k < bytes; k++)
            printf("%c", (q[k] >= 32 && q[k] < 127) ? q[k] : '.');
        printf("\n");
    }
    if (s->nstaged < 12) {
        s->stagelog[s->nstaged].size = bytes;
        s->stagelog[s->nstaged].prev_size = cd->xfer_size;
        s->stagelog[s->nstaged].prev_read = cd->xfer_pos;
    }
    s->staged_total += bytes;
    s->nstaged++;
    cd->xfer_size = bytes;
    cd->xfer_pos  = 0;
    if (s->nxferlog < 8) {
        s->xferlog[s->nxferlog].bytes = bytes;
        memcpy(s->xferlog[s->nxferlog].head, cd->xfer, bytes < 12 ? bytes : 12);
        if (bytes >= 36) memcpy(s->xferlog[s->nxferlog].more, cd->xfer + 12, 24);
        s->nxferlog++;
    }
    hirq_set(s, HIRQ_DRDY);
}

/* ---------------------------------------------------------------- setup */

static void filters_reset(cdblock *cd);
static void cdda_tick(saturn *s);
static int  route_sector(const cdblock *cd, const uint8_t sub[4], uint32_t fad);
static int  read_sector_sub(disc *d, uint32_t lba, uint8_t *out2048, uint8_t sub[4]);

void cdb_init(saturn *s, void *disc_handle, void *iso_handle)
{
    cdblock *cd = &s->cd;
    /* Allocate partition buffers on the heap -- 24 x 512 x 2048 = 25MB,
     * far too much for the stack-allocated saturn struct. */
    if (!cd->part) {
        cd->part     = calloc(CD_NUM_PARTS, CD_PART_SECTORS * 2048u);
        cd->part_fad = calloc(CD_NUM_PARTS, CD_PART_SECTORS * sizeof(uint32_t));
        cd->xfer     = calloc(1, CD_PART_SECTORS * 2048u);
        if (!cd->part || !cd->part_fad || !cd->xfer) {
            fprintf(stderr, "FATAL: cannot allocate CD partition buffers (%u MB)\n",
                    (unsigned)(CD_NUM_PARTS * CD_PART_SECTORS * 2048u / (1024*1024)));
            exit(1);
        }
    }
    memset(cd->part_sectors, 0, sizeof(cd->part_sectors));
    memset(cd->part_bytes, 0, sizeof(cd->part_bytes));
    /* Filters must hold Ymir's power-on defaults before the first command:
     * pass output = the filter's own index. A zeroed struct would instead say
     * "pass everything to partition 0 and fail to filter 0", which routes a
     * game's second stream on top of its first. */
    filters_reset(cd);
    cd->disc   = disc_handle;
    cd->iso    = iso_handle;
    /* A disc is present and stopped. Reporting BUSY here leaks into every
     * response's CR1 (status<<8) while the host is already issuing
     * commands, and CR1 is the high half of the transfer length it
     * reads back. boot_delay only paces the CMOK handshake. */
    /* Idle drive state. [PIN] against the CD block manual; sweepable so the
     * value the IPL actually wants can be measured rather than guessed. */
    cd->status = (uint8_t)(getenv("SATURN_CDSTATUS")
                           ? strtoul(getenv("SATURN_CDSTATUS"), NULL, 0)
                           : ST_PAUSE);
    /* CTRL/ADR nibbles of the subcode-Q byte: 4 = data track, 1 = position.
     * Sweepable -- these are unvalidated constants of exactly the kind that
     * produced the last four boot-blocking bugs. */
    cd->ctrl   = (uint8_t)(getenv("SATURN_CDCTRL")
                           ? strtoul(getenv("SATURN_CDCTRL"), NULL, 0) : 0xFF);           /* data track                        */
    cd->flag   = 0xFF;   /* flags nibble = 0xF (bits 7-4 of RR0's low byte) */
    /* Ymir Reset(): m_status.repeatCount = 0xF. This is the LOW nibble of
     * that same byte, so an idle drive reports 0xFF and only a live play
     * reports a real count (0 on start, ++ per repeat). Leaving it 0 here
     * makes the drive answer 0xF0 where hardware answers 0xFF. */
    cd->play_rptcnt = 0xFu;
    cd->read_speed  = 1;      /* Ymir Reset(): m_readSpeed = 1. A 2x default
                               * finishes a 16-sector read 3M cycles sooner,
                               * which lands PEND before the BIOS reads it. */
    cd->drive_next  = 0;
    cd->track  = 0xFF;   /* Ymir Reset(): position UNKNOWN until a read */
    cd->index  = 0xFF;
    cd->fad    = (uint32_t)(getenv("SATURN_CDFAD")
                            ? strtoul(getenv("SATURN_CDFAD"), NULL, 0) : 0xFFFFFFu);
    /* Frames of power-on init before we replace the signature with a real
     * status response. Long enough that the BIOS observes the signature,
     * short enough that the game's own commands are not all discarded. */
    /* The drive is ready early enough for the IPL's boot-animation probe.
     * Sixty fields made the US 1.01a BIOS decide the drive was absent, skip
     * its SCU-DSP animation, and enter the CD player. Twenty preserves the
     * reset settling interval while matching the successful hardware path. */
    cd->boot_delay = getenv("SATURN_BOOTDELAY")
                   ? atoi(getenv("SATURN_BOOTDELAY")) : 20;

    /* Power-on signature, read by the BIOS while it waits for us. */
    /* Read straight out of the BIOS ROM: the IPL memcmps CR1..CR4 against the
     * 8 bytes at 0x00004DB0, which are 00 43 44 42 4C 4F 43 4B -- that is
     * "\\0CDBLOCK", a LEADING NUL, not a trailing one. Getting this shifted by
     * one byte means the IPL never recognises the drive and never issues a
     * single command. Compared at 0x00004AAC. */
    s->cdb_reg[R_CR1] = 0x0043;  /* NUL 'C' */
    s->cdb_reg[R_CR2] = 0x4442;  /* "DB"    */
    s->cdb_reg[R_CR3] = 0x4C4F;  /* "LO"    */
    s->cdb_reg[R_CR4] = 0x434B;  /* "CK"    */
    /* CMOK ("ready for a command") is mandatory, not cosmetic: the game's
     * CD driver at 0x060682DA tests HIRQ bit 0 and returns -2 without sending
     * anything when it is clear. It has to be set from reset, and re-raised
     * whenever a command completes -- which respond() does. */
    /* Ymir Reset(hard): m_HIRQ = 0x0BE1 = CMOK|DCHG|ESEL|EHST|EFLS|MPED.
     * We powered on with CMOK alone, so every bit the host expects to
     * find already asserted -- notably MPED, "MPEG processing finished",
     * which nothing in our block ever raises -- read as still pending.
     * DCHG (0x20) is deliberately withheld: measured here, asserting it
     * at power-on wedges the IPL permanently and nothing clears it. */
    s->cdb_reg[R_HIRQ] = (uint16_t)(HIRQ_CMOK | HIRQ_DCHG | HIRQ_ESEL |
                                    HIRQ_EHST | HIRQ_ECPY | HIRQ_EFLS |
                                    HIRQ_MPED);   /* = 0x0BE1 exactly */
}

/* Called once per emulated frame. Completes power-on init, after which the
 * signature is replaced by a real status response and the BIOS proceeds. */
/* Sector period at 1x: 28.6364 MHz / 75 sectors per second. */
#define CDB_SECTOR_CYCLES 381818u

/* The drive issues its periodic status report on its OWN sector clock, not on
 * the video frame. That distinction is the whole ballgame for booting: the
 * IPL's status check at 0x00003BAA requires the PERI bit (0x20) in the status
 * byte, and it polls for it ~1400 times inside a single frame with interrupts
 * masked. Issuing the report only at the top of the frame loop means the IPL's
 * own GetStatus reply overwrites CR1 with a non-periodic status before the
 * poll even starts, so every poll in that frame returns -8 and the IPL retries
 * forever. Re-issuing on the sector clock lets a report land in the middle of
 * the poll, which is exactly what real hardware does -- and it is also why the
 * IPL reads the block twice and demands the two agree: on hardware a report
 * genuinely can land between them. */
void cdb_periodic_maybe(saturn *s)
{
    cdblock *cd = &s->cd;
    uint64_t now, iv;
    uint8_t saved;

    if (cd->boot_delay || !cd->first_cmd_seen) return;
    /* Ymir ProcessDriveState: the drive only reports while it is not in the
     * middle of a command (m_readyForPeriodicReports && !m_processingCommand).
     * Without this the report lands on top of a reply the host is halfway
     * through reading -- it collects CR1 from its answer and CR2..CR4 from a
     * drive-status block, which is a garbage 24-bit length or file id. */
    if (cd->processing_cmd) return;
    now = s->master.cycles;
    iv  = getenv("SATURN_PERICY")
          ? strtoull(getenv("SATURN_PERICY"), NULL, 0) : CDB_SECTOR_CYCLES;
    if (now - cd->last_peri_cy < iv) return;
    cd->last_peri_cy = now;
    cd->periodic_count++;

    saved = cd->status;
    cd->status = (uint8_t)(saved | ST_PERIODIC);
    respond_status(s);
    cd->status = saved;
    cd->resp_pending = 0;
    /* Ymir raises SCDQ with EVERY periodic report (ProcessDriveState). */
    hirq_set(s, HIRQ_SCDQ);
}

void cdb_tick(saturn *s)
{
    cdblock *cd = &s->cd;

    cdda_tick(s);

    if (cd->auth_delay > 0 && --cd->auth_delay == 0) {
        /* Security-ring read complete. */
        cd->authenticated = 1;
        cd->status = ST_PAUSE;
        respond_status(s);
        hirq_set(s, HIRQ_EFLS | HIRQ_CSCT);
    }

    if (cd->boot_delay > 0) {
        if (--cd->boot_delay == 0) {
            /* Power-on complete. The documented signal is HIRQ.CMOK -- the
             * "CDBLOCK" signature STAYS in CR1..CR4 until the host issues its
             * first command. Overwriting it with a status response makes the
             * BIOS poll CR1..CR4 forever waiting for a signature that has
             * already gone, which is exactly the 380,006-read timeout we
             * measured before this. */
            /* Power-on complete: report a real status AND raise CMOK. The
             * host reads CR1..CR4 to learn the drive state, so leaving the
             * "CDBLOCK" signature in place forever means it never sees one and
             * never issues a command. */
            cd->status = ST_PAUSE;
            cd->ready  = 1;
            respond_status(s);
            /* NOT HIRQ_DCHG. Bit 0x20 means "disc changed", and the IPL
             * refuses to proceed while it is set: 0x000032DC returns 1 when
             * (HIRQ & 0x20) != 0 and its caller at 0x000033E0 requires 0.
             * Nothing ever clears it either -- the IPL's own mask write of
             * 0x0BE1 preserves bit 5 -- so asserting it at power-on wedges the
             * boot permanently. A disc that was present all along has not
             * changed. */
            hirq_set(s, HIRQ_CMOK | HIRQ_ESEL | HIRQ_EHST | HIRQ_ECPY |
                        HIRQ_EFLS | HIRQ_SCDQ);
        }
        return;
    }
    /* Idle: subcode Q keeps ticking and the block is ready for a command. */
    /* Only the flags the block genuinely asserts while idle. Blanket-setting
     * every HIRQ bit each frame makes the host's "wait for flag X" succeed
     * spuriously on a stale bit, which breaks command sequencing. */
    hirq_set(s, HIRQ_CMOK | HIRQ_SCDQ);
    if (getenv("SATURN_ESEL")) hirq_set(s, HIRQ_ESEL);

    /* The block issues a periodic response: it rewrites CR1..CR4 with the
     * current drive state once per sector period, whether or not a command is
     * outstanding. The BIOS proves the drive is alive by watching those values
     * move -- with a static response it reads CR1..CR4 forever and never
     * issues its first command, which is the 380,006-read spin we measured.
     * Advancing the subcode-Q FAD while paused is what real hardware does. */
    /* [TEST] Do NOT advance the subcode-Q FAD while paused. The BIOS reads
     * CR1..CR4 twice and requires the two reads to be identical; a value that
     * moves underneath it fails that check. */
    /* An unsolicited report is tagged with ST_PERIODIC in the status byte --
     * that bit is how the host tells "this is the drive talking on its own"
     * from "this is the reply to my command". Sending the bare status makes a
     * host that is waiting for a periodic report poll CR1..CR4 forever. */
    /* The IPL's status check at 0x00003BAA reads the status byte and REQUIRES
     * bit 0x20 (ST_PERIODIC) to be set, else it returns -8 and retries. So the
     * unsolicited report has to keep flowing; gating it behind a pending reply
     * starved the IPL of the only status it will accept. A reply is still
     * protected for one tick by resp_fresh. */
    /* The power-on "\\0CDBLOCK" signature must stay in CR1..CR4 until the host
     * has issued its first command -- that is what the IPL memcmps at
     * 0x00004AAC. Only after that does the block start issuing unsolicited
     * periodic reports (status | 0x20), which is the only status the IPL will
     * accept from 0x00003BAA onward. Tying the switchover to the first command
     * satisfies both; a fixed frame delay cannot, because too short kills the
     * signature check and too long starves the status poll. */
    if (cd->init_busy > 0 && --cd->init_busy == 0) {
        cd->status = ST_PAUSE;          /* spin-up finished */
        hirq_set(s, HIRQ_CMOK | HIRQ_ESEL | HIRQ_EFLS | HIRQ_SCDQ);
    }
    /* Periodic drive reports are emitted by cdb_periodic_maybe() on the
     * sector clock.  Do not emit another report here on the video-field clock:
     * real hardware has no such source, and the duplicate could replace the
     * command block between the IPL's IP transfer and filesystem continuation.
     * It also made the report cadence depend on PAL/NTSC video timing. */

    /* A Saturn drive keeps reading once it is playing: sectors flow through the
     * filter chain into a partition on their own, and the host discovers them
     * by polling GetSectorNumber. Modelling the drive as idle means that poll
     * never returns anything -- which is exactly the 7,201-call spin we see.
     * Stream a few sectors per frame while the partition has room. */
    /* Pace the data drive. CD_SECTORS_PER_TICK alone delivers 16 sectors every
     * FRAME -- about 960/s, some twelve times a real drive -- so a 16-sector IP
     * read finished inside a single tick and PEND fired ~2M cycles before the
     * BIOS tested it at ROM 0x2D72. Ymir's drive runs at
     * kDriveCyclesPlaying1x / readSpeed, so its PEND lands AFTER that test,
     * which is why it boots and we did not.
     *
     * SATURN_DRIVEPACE=0 restores the old burst behaviour for comparison. */
    /* NOT while an audio track is playing. The drive has ONE head: a sector is
     * either delivered to the SCSP as CD-DA or routed into a buffer partition
     * as data, never both, and it advances the position exactly once (Ymir
     * ProcessDriveStatePlay). This streamer also calls cd_set_position(), so
     * with a CD-DA track playing the two consumers raced on cd->fad and the
     * audio path lost one or two sectors out of every two or three -- measured
     * as 239 discontinuities in the first 300 sectors of track 2, which is
     * music that audibly races ahead and then settles the moment the data
     * partition fills up and this loop stops running. */
    if (cd->playing && !cd->cdda_play &&
        cd->part_sectors[cd->cd_conn] < CD_PART_SECTORS) {
        unsigned p = cd->cd_conn;
        int quota = CD_SECTORS_PER_TICK;
        static int pace = -1;
        if (pace < 0) {
            const char *e = getenv("SATURN_DRIVEPACE");
            pace = e ? atoi(e) : 1;
        }
        if (pace) {
            /* One sector per drive period, at the configured read speed. */
            uint32_t per = CDB_SECTOR_CYCLES / (cd->read_speed ? cd->read_speed : 2u);
            if (cd->drive_next == 0) cd->drive_next = s->clk;
            quota = 0;
            while (cd->drive_next <= s->clk && quota < CD_SECTORS_PER_TICK) {
                cd->drive_next += per;
                quota++;
            }
            if (quota == 0) return;
        }
        for (int i = 0; i < quota; i++) {
            uint8_t sec[2048];
            uint32_t lba = cd->fad >= FAD_BASE ? cd->fad - FAD_BASE : 0;
            if (cd->play_end_fad && cd->fad >= cd->play_end_fad) {
                /* End of the commanded range: pause, and if a ReadFile is in
                 * flight, EFLS is its completion signal.
                 *
                 * PEND ("play end") must fire too. It marks the completion of
                 * ANY commanded play range, data or audio -- the CD-DA path
                 * below already raises it, and only this data path did not. A
                 * streaming movie player commands a range, drains it, and then
                 * waits on PEND to learn the stream finished; without it the
                 * player streams forever and the movie never ends. That is why
                 * Fighting Vipers loops in its intro FMV: 1003 sector reads,
                 * still polling, HIRQ 0x03C4 with bit 4 permanently clear. */
                cd->playing = 0;
                cd->play_end_fad = 0;
                cd->status = ST_PAUSE;
                /* NOTE: do NOT raise PEND here. It looks like the obvious
                 * counterpart to the CD-DA path below, which does raise it --
                 * but MEASURED, adding it truncates Fighting Vipers' intro
                 * movie: with PEND the picture corrupts at ~110s and never
                 * recovers; without it the movie completes and the title
                 * screen appears. Whatever the host does with PEND on a
                 * continuously streamed data range, it is not "the stream
                 * finished, carry on". Left out deliberately. */
                if (cd->efls_at_end) { cd->efls_at_end = 0; hirq_set(s, HIRQ_EFLS); }
                /* Ymir CheckPlayEnd(): PEND fires whenever the COMMANDED
                 * range runs out, data or audio. The synchronous path
                 * raises it from PlayDisc itself (play_done), so the host
                 * sees it either way; on the async path nothing did, and
                 * the BIOS sat waiting after its 16-sector IP read and
                 * never issued another command. Gated on the async opt-in
                 * so the measured Fighting Vipers result above stands. */
                if (asyncdrv()) hirq_set(s, HIRQ_PEND | HIRQ_EFLS);
                break;
            }
            uint8_t sub[4];
            int dest;
            if (cd->part_sectors[p] >= CD_PART_SECTORS) break;
            if (read_sector_sub((disc *)cd->disc, lba, sec, sub) != 0) {
                cd->playing = 0;
                break;
            }
            /* Route through the selector, exactly as PlayDisc does. Writing
             * straight to cd_conn here bypassed the filter chain entirely, so
             * a title that split one stream across two partitions got both
             * halves piled into one during continuous playback. */
            dest = route_sector(cd, sub, cd->fad);
            if (dest < 0) { cd_set_position(s, cd->fad + 1u); continue; }   /* filtered out */
            p = (unsigned)dest;
            if (cd->part_sectors[p] >= CD_PART_SECTORS) break;
            memcpy(cd->part[p] + (size_t)cd->part_sectors[p] * 2048u, sec, 2048);
            cd->part_fad[p][cd->part_sectors[p]] = cd->fad;
            cd->part_sectors[p]++;
            cd->part_bytes[p] += 2048u;
            cd_set_position(s, cd->fad + 1u);
        }
        hirq_set(s, HIRQ_CSCT);
        if (cd->part_sectors[p] >= CD_PART_SECTORS) hirq_set(s, HIRQ_BFUL);
    }
}

/* Feed the SCSP from an audio track, paced by how full its ring already is.
 * Ymir does the same with the return of its CDDA callback (cdblock.cpp:1069):
 * under a third full, run faster; over two thirds, ease off. */
/* CD-DA runs at DRIVE SPEED: 75 sectors a second, each 2352 bytes = 588
 * stereo frames = exactly 1/75 s at 44.1 kHz. Pushing sectors as fast as the
 * ring would take them made the music race, because the ring drops the oldest
 * samples once it overflows and playback effectively skips forward. Gate on
 * the master clock so a sector is only ever handed over once per 1/75 s. */
/* Derive this from the SAME clock the mixer uses, not from a second guess at
 * the SH-2 frequency. sound.c generates one 44.1 kHz sample every 649.351
 * cycles (a 28.636 MHz SH-2); a sector is 588 stereo frames, so one sector is
 * 649.351 * 588 cycles. Using 26843545/75 here instead -- the other Saturn
 * clock -- fed sectors about 6.7% early, so the music ran fast against the
 * mixer that was consuming it. */
#define CDDA_CY_PER_SECTOR  ((uint64_t)(649.351 * 588.0))

static void cdda_tick(saturn *s)
{
    cdblock *cd = &s->cd;
    int budget = CD_SECTORS_PER_TICK;
    static uint64_t next_cy;
    static uint32_t loop_start;

    if (!cd->cdda_play || !cd->playing) { next_cy = 0; return; }
    if (next_cy == 0) { next_cy = s->clk; loop_start = cd->fad; }
    /* Only pace when the emulated clock is actually running. tests/cdda_play
     * drives this directly without advancing the master, and a hard gate there
     * stalls the stream after one sector.
     *
     * Pace off s->clk, the MACHINE clock, not s->master.cycles. The two are
     * not the same clock: sh2_run returns after `n` instructions' worth of
     * work and s->clk advances by exactly n, but c->cycles charges 3 for a
     * taken delayed branch and 13 for an interrupt, so master.cycles runs
     * steadily AHEAD. sound_run consumes off n, so pacing the drive off
     * master.cycles delivered sectors faster than the mixer could drain them:
     * the ring overran, whole sectors were lost, and the music played fast. */
    if (s->clk > 0 && s->clk < next_cy) return;

    while (budget-- > 0) {
        uint8_t raw[2352];
        uint32_t lba, fill;

        if (cd->play_end_fad && cd->fad >= cd->play_end_fad) {
            /* CR3's high byte is the play mode; bits 3-0 are the repeat count
             * and 0xF means repeat forever. Sonic 3D Blast asks for its stage
             * music with CR3=0x0F00, so a track that simply stopped at the end
             * left every stage silent after one pass. */
            /* Ymir ProcessDriveStatePlay: a repeat only rewinds the head and
             * bumps repeatCount -- it raises NO interrupt. PEND means
             * "playback ENDED", and CheckPlayEnd fires it once, when the
             * repeats are exhausted. We used to pulse PEND on every loop,
             * which announced a track end each time round; audio drivers
             * restart a track on PEND (the repeating-sound bug), and the BIOS
             * CD shell polls PEND to decide whether the disc changed under it,
             * so a stale PEND sent it back to re-read the IP instead of
             * booting. Count UP toward play_repeat like Ymir does, so the
             * count we report in RR0 bits 3-0 is the one hardware reports. */
            if (cd->play_repeat == 0xFu || cd->play_rptcnt < cd->play_repeat) {
                cd->fad = loop_start;
                if (cd->play_rptcnt < 0xFu) cd->play_rptcnt++;
                continue;
            }
            cd->cdda_play = 0;
            cd->playing   = 0;
            cd->status    = ST_PAUSE;
            hirq_set(s, HIRQ_PEND | HIRQ_EFLS);
            return;
        }
        lba = cd->fad >= FAD_BASE ? cd->fad - FAD_BASE : 0;
        if (disc_read_raw((disc *)cd->disc, lba, raw) != 0) {
            cd->cdda_play = 0;
            cd->playing   = 0;
            cd->status    = ST_PAUSE;
            hirq_set(s, HIRQ_PEND | HIRQ_EFLS);
            return;
        }
        fill = scsp_cdda_push(s, raw);
        /* A return value of 3 means the SCSP ring REFUSED this sector. Do not
         * move the optical head or consume its drive-time deadline: doing so
         * discarded one sector on every backpressure event, so playback
         * jumped rapidly into the track until the rings happened to settle.
         * Resume from this same FAD after one sector period. */
        if (fill >= 3u) {
            if (s->clk > 0) next_cy = s->clk + CDDA_CY_PER_SECTOR;
            return;
        }
        cd_set_position(s, cd->fad + 1u);
        next_cy += CDDA_CY_PER_SECTOR;      /* one sector per 1/75 s */
        hirq_set(s, HIRQ_CSCT);
        if (s->clk > 0 && s->clk < next_cy) break;
        if (fill >= 2) break;          /* ring over two thirds full: ease off */
    }
}

/* ------------------------------------------------------- data transfer */

uint16_t cdb_read_dtr(saturn *s)
{
    /* Diagnostic: how often does the host pull the transfer register when
     * there is nothing staged? Every such read hands it a zero, which is
     * indistinguishable from real data and silently corrupts whatever it
     * is building. */
    if (s->cd.xfer_pos >= s->cd.xfer_size) s->dtr_dry++;
    else                                   s->dtr_ok++;

    cdblock *cd = &s->cd;
    uint16_t v;
    if (cd->xfer_pos + 1 >= cd->xfer_size) {
        cd->xfer_pos = cd->xfer_size;
        /* Diagnostic: with SATURN_DTRFILL set, hand back a recognisable
         * pattern instead of zeros. If a title gets further on pattern data
         * than on zeros, its loader is gated on data *flowing*; if it dies the
         * same way, the loader is validating content and needs the real file. */
        if (s->dtr_fill) return (uint16_t)(0xA500 | (s->dtr_seq++ & 0xFF));
        return 0;
    }
    v = (uint16_t)((cd->xfer[cd->xfer_pos] << 8) | cd->xfer[cd->xfer_pos + 1]);
    cd->xfer_pos += 2;
    if (cd->xfer_pos >= cd->xfer_size) hirq_set(s, HIRQ_EHST);
    return v;
}

/* ---------------------------------------------------- filesystem helpers */

/* Build the CD block's 12-byte-per-entry file info record for entry i of the
 * ISO directory. Layout: FAD(4), size(4), unit size(1), gap(1), file no(1),
 * attributes(1). */
/* The CD block's file list is PER-DIRECTORY. Our iso_fs is a flat, recursive
 * walk of the whole disc with full paths, so "how many files are here" and
 * "which file is id N" must both be answered against the entries that live
 * DIRECTLY in the current directory -- an entry is in the root exactly when
 * its path has one slash. Counting the flat list instead reported 191 files
 * for a root that holds 176, and let a file id resolve to something inside a
 * subdirectory; the host then built its file table from records that do not
 * belong to the directory it asked about. */
/* An audio track carries no filesystem data: its sectors are raw PCM bound
 * for the SCSP, not for a buffer partition. Ymir branches on the sector's
 * control/ADR nibble (cdblock.cpp:1057, `discPos.controlADR == 0x01`); we ask
 * the track table the same question. */
/* Derive the subcode-Q position fields from a FAD, the way a real drive does:
 * they describe WHERE THE HEAD IS. Ymir keeps controlADR/trackNum/indexNum at
 * 0xFF until the drive has actually read something (CDBlock::Reset leaves them
 * there) and fills them in as it plays -- which is why its live trace answers
 * 01FF FFFF FFFF FFFF to every command issued before the first read, then
 * 4101/0100 afterwards. We reported a fixed track 1 / index 1 / FAD 150 from
 * power-on instead, i.e. we claimed a settled position before the drive had
 * moved. This lets the position follow the head. */
static void cd_set_position(saturn *s, uint32_t fad)
{
    cdblock *cd = &s->cd;
    const disc *d = (const disc *)cd->disc;
    uint32_t lba = fad >= FAD_BASE ? fad - FAD_BASE : 0;
    int i, best = -1;

    cd->fad = fad;
    if (!d || d->ntracks <= 0) return;
    for (i = 0; i < d->ntracks; i++)
        if (d->tracks[i].start_lba <= lba &&
            (best < 0 || d->tracks[i].start_lba > d->tracks[best].start_lba))
            best = i;
    if (best < 0) return;
    cd->ctrl  = (uint8_t)(d->tracks[best].mode == TRACK_AUDIO ? 0x01 : 0x41);
    cd->track = (uint8_t)d->tracks[best].num;
    cd->index = 1;
    /* Once the position is KNOWN, Ymir replaces the reset 0xF/0xF with
     *   flags = (controlADR == 0x41) ? 0x8 : 0x0;  repeatCount = 0;
     * (ProcessDriveStatePlay and friends), so the low byte of RR1 becomes 0x80
     * on a data track and 0x00 on an audio one. 0xFF means `no position yet''
     * and must not survive the first read. */
    cd->flag  = (uint8_t)((cd->ctrl == 0x41) ? 0x80 : 0x00);
}

static int lba_is_audio(const disc *d, uint32_t lba)
{
    const disc_track *t = disc_track_for_lba(d, lba);
    return t && t->mode == TRACK_AUDIO;
}

/* ------------------------------------------------------- selector engine
 * Ported from Ymir (cdblock_filter.hpp Filter::Test, cdblock.cpp ~1095).
 * These were previously stubbed: 0x40/0x42/0x44/0x46 answered with a status
 * and stored nothing, and PlayDisc dropped every sector into the partition
 * named by cd_conn. That is fine for a plain file read, where one stream goes
 * to one partition, and fatal for FMV, where the whole point of the selector
 * is to split one interleaved stream into two partitions. */

/* Ymir Filter::Reset: conditions cleared, pass output = the filter's own
 * index, fail output disconnected. The pass_out default matters -- it is why
 * an unconfigured selector still delivers sectors to partition N. */
static void filter_reset_one(cdblock *cd, unsigned i)
{
    memset(&cd->filter[i], 0, sizeof(cd->filter[i]));
    cd->filter[i].pass_out = (uint8_t)i;
    cd->filter[i].fail_out = CD_DISCONNECTED;
}

static void filters_reset(cdblock *cd)
{
    unsigned i;
    for (i = 0; i < CD_NUM_FILTERS; i++) filter_reset_one(cd, i);
}

/* True when the sector satisfies this filter's conditions. `sub` is the
 * MODE2 subheader (file, channel, submode, coding info); it reads as all
 * zeroes on a MODE1 track, which has no subheader. */
static int filter_test(const cdblock *cd, unsigned fi,
                       const uint8_t sub[4], uint32_t fad)
{
    const uint8_t m = cd->filter[fi].mode;
    int pass = 1;

    if (m & 0x01) pass &= (sub[0] == cd->filter[fi].file_num);
    if (m & 0x02) pass &= (sub[1] == cd->filter[fi].chan_num);
    if (m & 0x04) pass &= ((sub[2] & cd->filter[fi].submode_mask)
                           == cd->filter[fi].submode_val);
    if (m & 0x08) pass &= ((sub[3] & cd->filter[fi].coding_mask)
                           == cd->filter[fi].coding_val);
    if (m & 0x10) pass = !pass;                 /* invert subheader result */
    if (!pass) return 0;

    if (m & 0x40) {                              /* FAD range              */
        if (fad < cd->filter[fi].start_fad ||
            fad >= cd->filter[fi].start_fad + cd->filter[fi].fad_count)
            return 0;
    }
    return 1;
}

/* Walk the chain from cd_conn. Returns the destination partition, or -1 when
 * the sector is discarded (no filter passed it, or the winner's pass output
 * is disconnected). Bounded by CD_NUM_FILTERS so a fail_out cycle programmed
 * by a game cannot hang the drive. */
static int route_sector(const cdblock *cd, const uint8_t sub[4], uint32_t fad)
{
    unsigned fn = cd->cd_conn;
    unsigned hops;

    for (hops = 0; hops < CD_NUM_FILTERS; hops++) {
        if (fn >= CD_NUM_FILTERS) return -1;         /* disconnected       */
        if (filter_test(cd, fn, sub, fad)) {
            uint8_t out = cd->filter[fn].pass_out;
            return out >= CD_NUM_PARTS ? -1 : (int)out;
        }
        fn = cd->filter[fn].fail_out;
    }
    return -1;
}

/* Read one sector's 2048 user bytes AND its subheader.
 *
 * disc_read_raw only succeeds on a 2352-byte track, which is the only place a
 * subheader physically exists. On a 2048-byte ISO rip the subheader bytes were
 * discarded at dump time, so they read as zero and subheader filtering cannot
 * distinguish anything -- that is a property of the image, not of the filter. */
static int read_sector_sub(disc *d, uint32_t lba, uint8_t *out2048, uint8_t sub[4])
{
    uint8_t raw[2352];

    sub[0] = sub[1] = sub[2] = sub[3] = 0;
    if (disc_read_raw(d, lba, raw) == 0) {
        if (raw[15] == 0x02) {          /* MODE2: subheader at +16, data +24 */
            sub[0] = raw[16]; sub[1] = raw[17];
            sub[2] = raw[18]; sub[3] = raw[19];
            memcpy(out2048, raw + 24, 2048);
        } else {                        /* MODE1: no subheader, data at +16  */
            memcpy(out2048, raw + 16, 2048);
        }
        return 0;
    }
    return disc_read_sector(d, lba, out2048, NULL);
}

/* Track numbers in the TOC's A0/A1 points are BCD, per Ymir's
 * TOC::LoadFrom (media/cd_defs.cpp), which stores to_bcd(trackNum). */
static uint8_t bcd(unsigned v) { return (uint8_t)(((v / 10u) << 4) | (v % 10u)); }

static int entry_in_cwd(const iso_entry *e)
{
    const char *p = e->path;
    int slashes = 0;
    while (*p) { if (*p == '/') slashes++; p++; }
    return slashes == 1;
}

static void file_record(const iso_entry *e, int fileno, uint8_t *out)
{
    uint32_t fad = e->lba + FAD_BASE;
    out[0] = (uint8_t)(fad >> 24); out[1] = (uint8_t)(fad >> 16);
    out[2] = (uint8_t)(fad >> 8);  out[3] = (uint8_t)fad;
    out[4] = (uint8_t)(e->size >> 24); out[5] = (uint8_t)(e->size >> 16);
    out[6] = (uint8_t)(e->size >> 8);  out[7] = (uint8_t)e->size;
    out[8]  = 0;
    out[9]  = 0;
    out[10] = (uint8_t)fileno;
    out[11] = e->is_dir ? 0x02 : 0x00;
}

/* ------------------------------------------------------------- dispatch */

void cdb_execute(saturn *s)
{
    cdblock *cd = &s->cd;
    uint16_t cr1 = cd->cmd_stage[0];
    uint16_t cr2 = cd->cmd_stage[1];
    uint16_t cr3 = cd->cmd_stage[2];
    uint16_t cr4 = cd->cmd_stage[3];
    uint8_t  op  = (uint8_t)(cr1 >> 8);
    iso_fs  *fs  = (iso_fs *)cd->iso;

    if (cdlog())
        /* Log the CALLER and the flags it can see. "the BIOS polls GetStatus
         * forever" only becomes actionable once you know which routine is
         * doing the polling and which HIRQ bit it is waiting on. */
        printf("[cd>] op=%02X  %04X %04X %04X %04X  cy=%llu pc=%08X hirq=%04X\n",
               op, cr1, cr2, cr3, cr4,
               (unsigned long long)s->master.cycles,
               s->master.pc, s->cdb_reg[R_HIRQ]);
    if (cdlog())
        printf("        caller pr=%08X r4=%08X r5=%08X\n",
               s->master.pr, s->master.r[4], s->master.r[5]);

    /* No "not ready yet" guard here: the host gates itself on HIRQ.CMOK
     * before it will send a packet, so anything that arrives is a real
     * command. Refusing early commands just stalls the boot. */

    /* Record the command stream; this doubles as the spec for what still
     * needs implementing. */
    {
        int found = 0;
        for (int i = 0; i < s->ncdcmd; i++)
            if (s->cdcmd[i].op == op) { s->cdcmd[i].count++; found = 1; break; }
        if (!found && s->ncdcmd < CDCMD_SLOTS) {
            s->cdcmd[s->ncdcmd].op    = op;
            s->cdcmd[s->ncdcmd].cr1   = cr1;
            s->cdcmd[s->ncdcmd].cr2   = cr2;
            s->cdcmd[s->ncdcmd].cr3   = cr3;
            s->cdcmd[s->ncdcmd].cr4   = cr4;
            s->cdcmd[s->ncdcmd].count = 1;
            s->ncdcmd++;
        }
    }

    /* Record the response too: the caller reads CR1/CR2 back as a 32-bit
     * transfer length, so a wrong CR2 becomes a runaway read. */
    #define LOGRESP()                                                                do { if (s->ncdresp < 16) {                                                           s->cdresp[s->ncdresp].op  = op;                                              s->cdresp[s->ncdresp].cr1 = s->cdb_reg[R_CR1];                               s->cdresp[s->ncdresp].cr2 = s->cdb_reg[R_CR2];                               s->cdresp[s->ncdresp].cr3 = s->cdb_reg[R_CR3];                               s->cdresp[s->ncdresp].cr4 = s->cdb_reg[R_CR4];                               s->ncdresp++; } } while (0)

    cd->first_cmd_seen = 1;
    if (getenv("SATURN_CDSEQ")) {
        static int seqn;
        printf("[cd %3d cy=%llu] op=%02X CR1=%04X CR2=%04X CR3=%04X CR4=%04X pr=%08X\n",
               ++seqn, (unsigned long long)s->master.cycles, op, cr1, cr2, cr3, cr4, s->master.pr);
    }

    switch (op) {

    case 0x00:  /* GetStatus */
        respond_status(s);
        break;

    case 0x01:  /* GetHardwareInfo. CR4 is the MPEG capability word; claiming
                 * 0x0400 tells the BIOS an MPEG card is fitted. Tunable while
                 * we work out what the IPL wants: "cr2,cr3,cr4". */
        {
            unsigned h2 = 0x0002, h3 = 0x0000, h4 = 0x0600;  /* DRIVE version (Ymir trace #0: ...0600), not MPEG */
            const char *e = getenv("SATURN_HWINFO");
            if (e) sscanf(e, "%x,%x,%x", &h2, &h3, &h4);
            respond(s, (uint16_t)((uint16_t)cd->status << 8), (uint16_t)h2,
                    (uint16_t)h3, (uint16_t)h4);
        }
        break;

    case 0x02: { /* GetTOC -- 102 longwords: 99 track slots, then points A0,
                  * A1, A2. Built from the disc's real track list, exactly as
                  * Ymir does (media/cd_defs.cpp TOC::LoadFrom):
                  *   track n  : (ctrladr << 24) | start FAD        [binary]
                  *   A0 (99)  : (ctrladr << 24) | first track << 16 [BCD]
                  *   A1 (100) : (ctrladr << 24) | last track  << 16 [BCD]
                  *   A2 (101) : (ctrladr << 24) | lead-out FAD     [binary]
                  * Every unused slot stays 0xFFFFFFFF. This used to be
                  * hardcoded to a single data track, which is why the BIOS's
                  * CD player displayed "TRACKS 1" on a twenty-track disc: it
                  * was faithfully echoing a TOC that described a disc we had
                  * invented. */
        uint8_t toc[102 * 4];
        const disc *d = (const disc *)cd->disc;
        int i, first = 0, last = 0;
        memset(toc, 0xFF, sizeof(toc));

        for (i = 0; i < d->ntracks; i++) {
            const disc_track *t = &d->tracks[i];
            uint32_t fad = t->start_lba + FAD_BASE;
            uint8_t  ctl = (t->mode == TRACK_AUDIO) ? 0x01 : 0x41;
            int n = t->num;
            if (n < 1 || n > 99) continue;
            if (!first || n < first) first = n;
            if (n > last) last = n;
            toc[(n - 1) * 4 + 0] = ctl;
            toc[(n - 1) * 4 + 1] = (uint8_t)(fad >> 16);
            toc[(n - 1) * 4 + 2] = (uint8_t)(fad >> 8);
            toc[(n - 1) * 4 + 3] = (uint8_t)fad;
        }
        if (!first) { first = last = 1; }

        toc[99 * 4 + 0] = 0x41;  toc[99 * 4 + 1] = bcd((unsigned)first);
        toc[99 * 4 + 2] = 0x00;  toc[99 * 4 + 3] = 0x00;
        toc[100 * 4 + 0] = 0x41; toc[100 * 4 + 1] = bcd((unsigned)last);
        toc[100 * 4 + 2] = 0x00; toc[100 * 4 + 3] = 0x00;
        {
            uint32_t lo = FAD_BASE + d->total_sectors;
            toc[101 * 4 + 0] = 0x41;
            toc[101 * 4 + 1] = (uint8_t)(lo >> 16);
            toc[101 * 4 + 2] = (uint8_t)(lo >> 8);
            toc[101 * 4 + 3] = (uint8_t)lo;
        }
        if (getenv("SATURN_TOCDUMP")) {
            fprintf(stderr, "[toc] ntracks=%d first=%d last=%d leadout=%u\n",
                    d->ntracks, first, last, FAD_BASE + d->total_sectors);
            for (i = 0; i < d->ntracks && i < 30; i++)
                fprintf(stderr, "[toc]  track %2d ctl=%02X fad=%u\n",
                        d->tracks[i].num,
                        d->tracks[i].mode == TRACK_AUDIO ? 0x01 : 0x41,
                        d->tracks[i].start_lba + FAD_BASE);
        }
        begin_transfer(s, toc, sizeof(toc));
        respond(s, (uint16_t)((uint16_t)cd->status << 8), 0x00CC, 0x0000, 0x0000);
        hirq_set(s, HIRQ_DRDY);
        break;
    }

    case 0x03:  /* GetSessionInfo. CR1 low byte selects: 0 = summary (count
                 * in RR2 high), N = that session's first-track/start-FAD, and
                 * NONEXISTENT sessions must answer FFFF/FFFF (Ymir
                 * CmdGetSessionInfo). Answering every probe with a plausible
                 * session made the BIOS read the disc as multisession -- a
                 * burned CD-R by its rules -- and it fell to the CD player
                 * instead of auto-booting the game. */
        cd->status = ST_PAUSE;
        {
            /* Exactly Mednafen cdb.cpp COMMAND_GET_SESSINFO: the summary
             * carries the LEAD-OUT FAD (disc end), session 1 carries FAD 0,
             * and higher sessions answer FF/FFFFFF. Both a plausible session 2
             * (reads as multisession CD-R -> CD player) and a wrong session-1
             * FAD (reads as damaged -> "Disc unsuitable") break the boot. */
            /* Exactly Mednafen cdb.cpp COMMAND_GET_SESSINFO: the summary
             * carries the LEAD-OUT FAD (disc end), session 1 carries FAD 0,
             * and higher sessions answer FF/FFFFFF.
             *
             * TRIED AND REVERTED: Ymir's CmdGetSessionInfo instead returns
             * RR3 = 0 for the summary and the TOC start FAD for session 1.
             * Matching Ymir here REGRESSED Sonic 3D Blast -- it stopped
             * booting and sat in the CD player playing track 1 -- while doing
             * nothing for NiGHTS' auto-boot. Our surrounding CD model (TOC,
             * PlayDisc, status) is built around the Mednafen values, so one
             * command cannot be swapped to Ymir's convention in isolation. */
            uint8_t  snum = (uint8_t)(cr1 & 0xFF);
            uint32_t fad;
            uint8_t  rsw;
            if (snum == 0) {
                /* Ymir's LIVE trace, boot entry #9: 0300 -> 0100 0000 0100 0000,
                 * i.e. RR3 = count<<8 = 0x0100 and RR4 = 0. NOT the lead-out
                 * FAD, which is what we used to report here. Session 1 (#10)
                 * is byte-identical in Ymir and our session-1 path already
                 * matches -- changing BOTH is what regressed Sonic 3D earlier,
                 * so only session 0 moves. */
                fad = 0;
                rsw = 0x01;                      /* session count */
            } else if (snum == 1) {
                fad = 0;
                rsw = snum;
            } else {
                fad = 0xFFFFFFu;
                rsw = 0xFF;
            }
            respond(s, (uint16_t)((uint16_t)cd->status << 8), 0,
                    (uint16_t)((rsw << 8) | ((fad >> 16) & 0xFF)),
                    (uint16_t)fad);
        }
        break;

    case 0x04:  /* InitializeCDSystem: the drive spins up and reports what it
                 * found. This is the point a disc that was already in the tray
                 * becomes "newly seen", so DCHG belongs HERE -- not at power-on
                 * where it blocks the IPL's own readiness check (M24). */
        /* A real drive SPINS UP here: it reports BUSY, then settles to
         * PAUSE a few sector periods later. The game's CD driver waits
         * for exactly that transition -- it samples CR1 and loops until
         * the value moves (0x060402E4). Replying PAUSE immediately means
         * CR1 never changes and the driver spins forever. */
        /* [MEASURED] Reporting BUSY here BREAKS the boot. The IPL polls
         * the status hard for the next few frames (90k reads) and needs
         * to see a settled drive; a BUSY spin-up makes it give up and
         * fall through to the CD player instead of reading the disc.
         * Opt-in only. */
        if (getenv("SATURN_SPINUP")) {
            cd->status    = ST_BUSY;
            cd->init_busy = 4;
        } else {
            const char *e = getenv("SATURN_CDSTATUS");
            cd->status = e ? (uint8_t)strtoul(e, NULL, 0) : ST_PAUSE;
        }
        cd->xfer_size = cd->xfer_pos = 0;
        /* THE DRIVE GOES TO 2x HERE. Ymir resets to 1x
         * (cdblock.cpp:125 `m_readSpeed = 1`) and then, in
         * CmdInitializeCDSystem, sets `m_readSpeed = m_readSpeedFactor` --
         * which is 2 -- ignoring the speed bits the host asked for
         * (cdblock.cpp:1954). We stayed at 1x for the whole session, so every
         * streaming title got HALF the bandwidth a real drive gives it.
         *
         * That is what starves Sonic 3D Blast's TrueMotion intro: it plays
         * smoothly for about twelve seconds -- the length of its preloaded
         * buffer -- and then updates only as sectors trickle in, which looks
         * like a movie that goes choppy and then sticks. MEASURED over the
         * same 2e9-instruction window: 38 distinct frames at 1x, 86 with
         * delivery unthrottled.
         *
         * Keeping the 1x RESET default is what preserves the auto-boot fix:
         * the IPL does its 16-sector read before ever issuing this command,
         * so its PEND timing is unchanged. */
        cd->read_speed = 2;
        /* Ymir CmdInitializeCDSystem parks the head at FAD 150 whenever the
         * drive is neither Open nor NoDisc (m_status.frameAddress = 150). */
        /* Ymir's CmdInitializeCDSystem sets frameAddress = 150 and NOTHING else:
         * controlADR / trackNum / indexNum stay 0xFF until the drive actually
         * READS. Its trace answers 01FF FFFF FF00 0096 here. Routing this
         * through cd_set_position filled the position in early and produced
         * 01FF 4101 0100 0096. Park the head only. */
        cd->fad = 150;
        /* CR1 bit 0 is SOFT RESET (Ymir: softReset): it clears the disc
         * authentication result, the play parameters and the partition
         * manager. We ignored the bit entirely. */
        if (cr1 & 1u) {
            cd->authenticated = 0;
            cd->auth_delay    = 0;
            cd->playing       = 0;
            cd->play_end_fad  = 0;
            for (unsigned q = 0; q < CD_NUM_PARTS; q++) {
                cd->part_sectors[q] = 0;
                cd->part_bytes[q]   = 0;
            }
        }
        respond_status(s);
        /* Ymir CmdInitializeCDSystem: EFLS|ECPY|EHST|ESEL|CMOK -- no SCDQ. */
        hirq_set(s, HIRQ_ESEL | HIRQ_EHST | HIRQ_ECPY | HIRQ_EFLS);
        if (getenv("SATURN_DCHG")) hirq_set(s, HIRQ_DCHG);
        break;

    case 0x06: {  /* EndDataTransfer */
        /* Ymir CmdEndDataTransfer: RR0 = status<<8 | count>>16, RR1 = count,
         * where count is m_xferCount -- initialised to 0xFFFFFF and reset to
         * it by EndTransfer(). "No transfer in progress" is therefore reported
         * as -1, NOT as zero. We answered 0100 0000 where hardware answers
         * 01FF FFFF; that is the first divergence in the ROM boot chain. */
        uint32_t count = cd->xfer_pos ? (cd->xfer_pos >> 1) : 0xFFFFFFu;
        respond(s, (uint16_t)((cd->status << 8) | ((count >> 16) & 0xFF)),
                (uint16_t)(count & 0xFFFF), 0, 0);
        cd->xfer_size = cd->xfer_pos = 0;
        /* TRIED TWICE AND REVERTED: Ymir raises EHST here only for a real
         * sector transfer (EndTransfer() skips TOC/FileInfo/Subcode). Gating
         * on cd->xfer_size regressed Sonic 3D to the Set Clock screen; gating
         * on a transfer TYPE left the CD player's status bar empty, because
         * our TOC path relies on this EHST. Our transfers complete inside the
         * command handler, so the unconditional raise is load-bearing until
         * the transfer state machine is modelled over time. */
        hirq_set(s, HIRQ_EHST);
        break;
    }

    case 0x10: { /* PlayDisc: CR1 low + CR2 = start FAD, CR3 low + CR4 = end.
        printf("[PlayDisc] CR1=%04X CR2=%04X CR3=%04X CR4=%04X -> start=%u nsec=%u\n",
               cr1, cr2, cr3, cr4,
               (unsigned)((((uint32_t)(cr1 & 0xFF) << 16) | cr2) & 0x7FFFFFu),
               (unsigned)((((uint32_t)(cr3 & 0xFF) << 16) | cr4) & 0x7FFFFFu));
                  * Real hardware streams the range into a partition through
                  * the filter chain; we read it straight off the disc into the
                  * partition buffer, which is what the host then drains with
                  * GetSectorData. Without this a title configures its filters
                  * and then waits forever for sectors that never arrive. */
        /* Bit 23 of the FAD/count fields is a flag, not address data: NiGHTS
         * asks for 0x8000A6, which is FAD 166 = LBA 16, the ISO volume
         * descriptor. Failing to mask it yields a nonsense LBA and the read
         * silently returns nothing. */
        uint32_t start = ((((uint32_t)cr1 & 0xFF) << 16) | cr2) & 0x7FFFFFu;
        uint32_t nsec  = ((((uint32_t)cr3 & 0xFF) << 16) | cr4) & 0x7FFFFFu;
        uint32_t got   = 0;
        int      play_done = 0;   /* the commanded range finished in the burst */
        printf("[PlayDisc] CR1=%04X CR2=%04X CR3=%04X CR4=%04X -> start=%u nsec=%u\n",
               cr1, cr2, cr3, cr4,
               (unsigned)((((uint32_t)(cr1 & 0xFF) << 16) | cr2) & 0x7FFFFFu),
               (unsigned)((((uint32_t)(cr3 & 0xFF) << 16) | cr4) & 0x7FFFFFu));

        /* The position is EITHER a FAD or a track/index pair, and bit 23 of
         * the raw field says which: SET = FAD, CLEAR = track/index with the
         * track in bits 15-8 and the index in bits 7-0. Masking the bit off
         * and always treating the value as a FAD is why CD-DA never played:
         * Sonic 3D Blast asks for its music with CR1=1000 CR2=0500 CR3=0F00
         * CR4=0563 -- bit 23 CLEAR, i.e. "play track 5 index 0 through track 5
         * index 99" -- which as a FAD became LBA 1130, inside the DATA track,
         * so lba_is_audio() said no and the audio path never started. Every
         * data read uses the FAD form (CR1=0x1080 -> 0x8000A6), which is why
         * the FAD-only reading worked for everything else. */
        /* Ymir CmdPlayDisc: m_status.repeatCount = 0 ("first repeat") on
         * EVERY play, data or audio -- it is the low nibble of RR0's low
         * byte. Leaving the idle 0xF there made our data-read replies come
         * back 418F where Ymir answers 4180. */
        cd->play_rptcnt = 0;
        if (!(((uint32_t)(cr1 & 0xFF) << 16) & 0x800000u)) {
            const disc *d = (const disc *)cd->disc;
            unsigned tno = (start >> 8) & 0xFFu;
            int i, found = -1;
            for (i = 0; d && i < d->ntracks; i++)
                if ((unsigned)d->tracks[i].num == tno) { found = i; break; }
            if (found >= 0) {
                uint32_t s0 = d->tracks[found].start_lba, end = 0;
                for (i = 0; i < d->ntracks; i++)
                    if (d->tracks[i].start_lba > s0 && (end == 0 || d->tracks[i].start_lba < end))
                        end = d->tracks[i].start_lba;
                start = s0;
                nsec  = end > s0 ? end - s0 : 0x00FFFFFFu;
                if (cdlog())
                    printf("[cd:trk] play track %u -> LBA %u for %u sector(s)\n",
                           tno, (unsigned)start, (unsigned)nsec);
            } else {
                if (start >= FAD_BASE) start -= FAD_BASE;
            }
        } else if (start >= FAD_BASE) {
            start -= FAD_BASE;                          /* FAD -> LBA */
        }
        if (nsec == 0 || nsec > 0x00FFFFFFu) nsec = 1;
        /* Remember what was ASKED for before clamping the burst. A movie is a
         * range of thousands of sectors: reading the first 512 and stopping
         * dead starves the player, which then decodes blank frames forever.
         * The burst below satisfies small reads (the BIOS's 16-sector IP.BIN
         * fetch completes in one go, exactly as before) and anything larger
         * keeps streaming from cdb_tick as the host drains the partition. */
        {
            uint32_t want = nsec;
            if (nsec > CD_PART_SECTORS) nsec = CD_PART_SECTORS;
            cd->play_want = want;
        }

        /* Route each sector through the selector rather than dumping the
         * whole range into cd_conn's partition. For a plain file read every
         * sector passes filter cd_conn and lands in one partition, exactly as
         * before; for an interleaved movie the video and audio sectors split
         * into the two partitions the player programmed. */
        {
            uint32_t touched[CD_NUM_PARTS];
            unsigned q;
            for (q = 0; q < CD_NUM_PARTS; q++) touched[q] = 0;

            /* An audio range is playback, not a read: hand it to cdb_tick to
             * stream at drive speed. Slurping 150 sectors of PCM into the ring
             * here would drop all but the last few and play a click. */
            if (lba_is_audio((disc *)cd->disc, start)) {
                cd->playing      = 1;
                cd->cdda_play    = 1;
                cd_set_position(s, start + FAD_BASE);
                /* Use the length the host ASKED for, not the burst-clamped
                 * one. `nsec` was already clipped to CD_PART_SECTORS above to
                 * bound a DATA partition read; applying that to audio ended a
                 * 4134-sector music track after 512 sectors -- 6.8 seconds --
                 * and then stopped or restarted it. Playback streams at drive
                 * speed from cdb_tick, so it needs no such clamp. */
                cd->play_end_fad = start + FAD_BASE +
                                   (cd->play_want ? cd->play_want : nsec);
                cd->play_repeat  = (cr3 >> 8) & 0xFu;   /* play mode: repeats */
                cd->play_rptcnt  = 0;   /* Ymir resets repeatCount on a new play */
                cd->status       = ST_PLAY;
                if (cdlog())
                    printf("[cd:cdda] play LBA %u for %u sector(s) repeat=%X\n",
                           (unsigned)start,
                           (unsigned)(cd->play_want ? cd->play_want : nsec),
                           cd->play_repeat);
                respond_status(s);
                /* Ymir CmdPlayDisc: CMOK ONLY. PEND means "playback has
                 * STOPPED"; raising it as playback starts tells a driver that
                 * waits on PEND for end-of-play that the play is already
                 * over. */
                break;
            }

            /* Do not satisfy the request inside the
             * command. Ymir's drive walks SEEK -> PLAY -> PAUSE and delivers
             * sectors over time (ProcessDriveState/ProcessDriveStatePlay), so
             * its PlayDisc reply reports the drive still at the REQUESTED FAD
             * with nothing yet transferred. Ours reads the whole range here,
             * which is why our reply carries an already-advanced position.
             * The streaming path below (cd->playing / cdb_tick) already
             * exists for requests too big for one burst -- this just routes
             * every request through it. SATURN_SYNCDRIVE restores the old
             * immediate/burst behavior strictly as a diagnostic override.
             */
            if (asyncdrv()) {
                if (!cd->play_want) cd->play_want = nsec;
                nsec = 0;
            }
            for (uint32_t i = 0; i < nsec; i++) {
                uint8_t sec[2048], sub[4];
                uint32_t fad = start + i + FAD_BASE;
                int part;

                if (read_sector_sub((disc *)cd->disc, start + i, sec, sub) != 0)
                    break;
                got++;                       /* read off the disc, routed or not */

                part = route_sector(cd, sub, fad);
                if (part < 0) continue;      /* filtered out and discarded */
                if (touched[part] >= CD_PART_SECTORS) continue;

                memcpy(cd->part[part] + (size_t)touched[part] * 2048u, sec, 2048);
                cd->part_fad[part][touched[part]] = fad;
                touched[part]++;
            }

            for (q = 0; q < CD_NUM_PARTS; q++) {
                if (!touched[q]) continue;
                cd->part_sectors[q] = touched[q];
                cd->part_bytes[q]   = touched[q] * 2048u;
                if (cdlog())
                    printf("[cd:route] partition %u <- %u sector(s)%s",
                           q, (unsigned)touched[q], "\n");
            }
        }
        cd_set_position(s, start + FAD_BASE + got);
        /* CmdPlayDisc begins in SEEK. Ymir's immediate response for the IPL
         * read is 0400 4101 0100 0096: the requested position is known, but
         * RR0's data-track flag is still clear. ProcessDriveStatePlay sets it
         * to 0x80 only when the first sector actually arrives. Advertising
         * 0480 here moves that state transition one drive phase too early and
         * sends the US IPL down its CD-player fallback after the IP transfer. */
        if (asyncdrv() && got == 0)
            cd->flag = 0;
        if (cdlog() && got != nsec)
            printf("[cd:play-short] wanted %u got %u first-missing LBA %u%s",
                   (unsigned)nsec, (unsigned)got, (unsigned)(start + got), "\n");
        if (cd->play_want > got) {
            /* More was requested than the burst delivered: keep the drive
             * running so the rest arrives as the host frees buffer space. */
            cd->playing      = 1;
            cd->cdda_play    = 0;
            /* Ymir's CmdPlayDisc reply carries kStatusCodeSeek (0x04):
             * the drive has been TOLD to go there and has not arrived.
             * ProcessDriveState walks Seek -> Play as it lands. */
            cd->status       = asyncdrv() ? ST_SEEK : ST_PLAY;
            cd->play_end_fad = start + FAD_BASE + cd->play_want;
            if (cdlog())
                printf("[cd:stream] burst %u of %u sectors, streaming to FAD %u%s",
                       (unsigned)got, (unsigned)cd->play_want,
                       (unsigned)cd->play_end_fad, "\n");
            /* still streaming: NOT the end of playback */
        } else {
            cd->status       = ST_PAUSE;
            cd->playing      = 0;
            cd->play_end_fad = 0;
            play_done = 1;
        }

        respond_status(s);
        /* Ymir CmdPlayDisc raises CMOK only; CSCT comes from the drive as
         * each sector lands (ProcessDriveStatePlay) and PEND/EFLS from
         * CheckPlayEnd when the commanded range RUNS OUT. We satisfy a small
         * request inside the command itself, so for that case the sectors
         * have landed and the play has already ended and both signals are
         * due right here -- but for a request the burst could not satisfy
         * (a movie: thousands of sectors) the drive is still PLAYING, and
         * telling the host "playback ended" at that moment is a lie that
         * cdb_tick then has to walk back. */
        if (got) hirq_set(s, HIRQ_CSCT);
        if (play_done) hirq_set(s, HIRQ_PEND | HIRQ_EFLS);
        break;
    }

    case 0x11: { /* SeekDisc -- CMOK only, no PEND (Ymir CmdSeekDisc).
                  *
                  * The parameter is not merely a position. Three cases:
                  *   0xFFFFFF -> pause the drive where it stands
                  *   0        -> STOP it; a stopped drive settles in STANDBY
                  *   else     -> seek there, then pause
                  * We answered PAUSE unconditionally and never moved the head,
                  * so the stop case reported the wrong state at a stale FAD.
                  * The IPL issues exactly this (CR1=0x1100 CR2=0x0000) as the
                  * last thing it does before deciding what the disc is, then
                  * polls GetStatus -- and a standby that never arrives is why
                  * it gave up and fell through to the CD player instead of
                  * booting the game. */
        uint32_t tgt = (((uint32_t)cr1 & 0xFFu) << 16) | cr2;
        cd->playing   = 0;
        cd->cdda_play = 0;
        if (tgt == 0xFFFFFFu) {
            cd->status = ST_PAUSE;
        } else if (tgt == 0) {
            cd->status = ST_STANDBY;
            cd_set_position(s, FAD_BASE);
        } else {
            cd->status = ST_PAUSE;
            cd_set_position(s, tgt & 0x7FFFFFu);
        }
        respond_status(s);
        break;
    }

    case 0x20:  /* GetSubcodeQ / GetSubcodeRW */
        respond(s, (uint16_t)((uint16_t)cd->status << 8), 0x0005, 0, 0);
        hirq_set(s, HIRQ_DRDY);
        break;

    /* ---- filter / selector family -------------------------------------
     * The selector model routes incoming sectors to partitions by FAD range,
     * subheader conditions and mode. NiGHTS programs it on every boot, and a
     * REJECT here sends the game into an error path and off into garbage --
     * these have to answer, even before partitions do anything real. */
    /* Register layouts below are Ymir's (cdblock.cpp CmdSetFilter*). Every
     * one of these carries the filter number in CR3's HIGH byte. */
    case 0x40: {  /* SetFilterRange: CR1lo:CR2 = start FAD, CR3lo:CR4 = count */
        unsigned fi = (cr3 >> 8) & 0xFF;
        if (fi >= CD_NUM_FILTERS) { respond_status(s); hirq_set(s, HIRQ_ESEL); break; }
        cd->filter[fi].start_fad = (uint32_t)((cr1 & 0xFF) << 16) | cr2;
        cd->filter[fi].fad_count = (uint32_t)((cr3 & 0xFF) << 16) | cr4;
        respond_status(s);
        hirq_set(s, HIRQ_ESEL);
        break;
    }

    case 0x42: {  /* SetFilterSubheaderConditions
                   *   CR1 lo = channel      CR2 hi = submode mask
                   *   CR2 lo = coding mask  CR3 hi = filter number
                   *   CR3 lo = file id      CR4 hi = submode value
                   *   CR4 lo = coding value                            */
        unsigned fi = (cr3 >> 8) & 0xFF;
        if (fi >= CD_NUM_FILTERS) { respond_status(s); hirq_set(s, HIRQ_ESEL); break; }
        cd->filter[fi].chan_num     = (uint8_t)(cr1 & 0xFF);
        cd->filter[fi].submode_mask = (uint8_t)(cr2 >> 8);
        cd->filter[fi].coding_mask  = (uint8_t)(cr2 & 0xFF);
        cd->filter[fi].file_num     = (uint8_t)(cr3 & 0xFF);
        cd->filter[fi].submode_val  = (uint8_t)(cr4 >> 8);
        cd->filter[fi].coding_val   = (uint8_t)(cr4 & 0xFF);
        if (cdlog())
            printf("[cd:filt] %u chan=%02X file=%02X sm=%02X/%02X ci=%02X/%02X%s",
                   fi, cd->filter[fi].chan_num, cd->filter[fi].file_num,
                   cd->filter[fi].submode_mask, cd->filter[fi].submode_val,
                   cd->filter[fi].coding_mask, cd->filter[fi].coding_val, "\n");
        respond_status(s);
        hirq_set(s, HIRQ_ESEL);
        break;
    }

    case 0x44: {  /* SetFilterMode: CR1 lo = mode, CR3 hi = filter number.
                   * Mode bit 7 is "reset this filter's conditions" and is not
                   * part of the stored mode. */
        unsigned fi = (cr3 >> 8) & 0xFF;
        uint8_t mode = (uint8_t)(cr1 & 0xFF);
        if (fi >= CD_NUM_FILTERS) { respond_status(s); hirq_set(s, HIRQ_ESEL); break; }
        if (mode & 0x80) {
            uint8_t po = cd->filter[fi].pass_out, fo = cd->filter[fi].fail_out;
            filter_reset_one(cd, fi);          /* conditions only ... */
            cd->filter[fi].pass_out = po;      /* ... connections survive */
            cd->filter[fi].fail_out = fo;
        }
        cd->filter[fi].mode = (uint8_t)(mode & 0x5F);
        if (cdlog())
            printf("[cd:filt] %u mode=%02X%s", fi, cd->filter[fi].mode, "\n");
        respond_status(s);
        hirq_set(s, HIRQ_ESEL);
        break;
    }

    case 0x46: {  /* SetFilterConnection
                   *   CR1 lo b0 = set pass, b1 = set fail
                   *   CR2 hi = pass output, CR2 lo = fail output
                   *   CR3 hi = filter number                           */
        unsigned fi = (cr3 >> 8) & 0xFF;
        if (fi >= CD_NUM_FILTERS) { respond_status(s); hirq_set(s, HIRQ_ESEL); break; }
        if (cr1 & 0x01) cd->filter[fi].pass_out = (uint8_t)(cr2 >> 8);
        if (cr1 & 0x02) cd->filter[fi].fail_out = (uint8_t)(cr2 & 0xFF);
        if (cdlog())
            printf("[cd:filt] %u pass=%02X fail=%02X%s", fi,
                   cd->filter[fi].pass_out, cd->filter[fi].fail_out, "\n");
        respond_status(s);
        hirq_set(s, HIRQ_ESEL);
        break;
    }

    case 0x4A:  /* SetFilterSelectorConnection -- not modelled separately. */
        respond_status(s);
        hirq_set(s, HIRQ_ESEL);
        break;

    case 0x41:  /* GetFilterRange: FAD start in CR1/CR2, count in CR3/CR4 */
        respond(s, (uint16_t)((uint16_t)cd->status << 8), FAD_BASE,
                0x0000, 0xFFFF);
        break;
    case 0x43:  /* GetFilterSubheaderConditions -- Ymir: CMOK | ESEL */
        respond(s, (uint16_t)((uint16_t)cd->status << 8), 0, 0, 0);
        hirq_set(s, HIRQ_ESEL);
        break;
    case 0x45:  /* GetFilterMode -- Ymir: CMOK | ESEL */
        respond(s, (uint16_t)((uint16_t)cd->status << 8), 0, 0, 0);
        hirq_set(s, HIRQ_ESEL);
        break;
    case 0x47:  /* GetFilterConnection -- Ymir: CMOK only. */
        respond(s, (uint16_t)((uint16_t)cd->status << 8), 0, 0, 0);
        break;

    case 0x31:  /* GetCDDeviceConnection -- Ymir: CMOK only, no ESEL. */
        respond(s, (uint16_t)((uint16_t)cd->status << 8), 0, 0, 0);
        break;
    case 0x32:  /* GetLastBufferDestination */
        respond(s, (uint16_t)((uint16_t)cd->status << 8), 0, 0, 0);
        break;
    case 0x61:   /* GetSectorData */
    case 0x63: { /* GetThenDeleteSectorData -- 0x63, NOT 0x62 (Ymir cdblock
                  * command table). With the two swapped, the BIOS's boot-time
                  * IP.BIN fetch (0x63) fell into a pure delete: no transfer
                  * was armed, 18,432 DTR reads came back dry, and the check
                  * ended at "Disc unsuitable for this system".
                  * CR2 = sector offset within the
                  * partition, CR4 = sector count. This is how a title actually
                  * collects the bytes PlayDisc streamed in; without it the
                  * partition fills and nothing ever drains it. */
        unsigned p = PNUM(cr3);
        uint32_t off = cr2;
        uint32_t cnt = cr4 ? cr4 : 1;
        uint32_t bytes;

        if (s->nsdlog < 12) {
            s->sdlog[s->nsdlog].want = cr4 ? cr4 : 1;
            s->sdlog[s->nsdlog].have = cd->part_sectors[p];
            s->sdlog[s->nsdlog].off  = cr2;
            s->nsdlog++;
        }
        if (off > cd->part_sectors[p]) off = cd->part_sectors[p];
        if (off + cnt > cd->part_sectors[p]) cnt = cd->part_sectors[p] - off;
        bytes = cnt * 2048u;

        if (cd->sec_len_get == 2352u) {
            /* The host asked for RAW sectors -- the BIOS's disc check drains
             * the IP.BIN range at 2352 bytes a sector and memcmps structure
             * our 2048-byte cooked data does not have (measured: 37,680 bytes
             * drained, 816 staged, "Disc unsuitable for this system"). The
             * bin is MODE1/2352, so serve the true raw bytes. */
            uint32_t q, ok = 0;
            for (q = 0; q < cnt; q++) {
                uint32_t lba = cd->part_fad[p][off + q] >= FAD_BASE
                             ? cd->part_fad[p][off + q] - FAD_BASE : 0;
                if ((size_t)(ok + 1) * 2352u > (CD_PART_SECTORS * 2048u)) break;
                if (disc_read_raw((disc *)cd->disc, lba,
                                  cd->xfer + (size_t)ok * 2352u) != 0) break;
                ok++;
            }
            cd->xfer_size = ok * 2352u;
            cd->xfer_pos  = 0;
            bytes = cnt * 2048u;   /* partition accounting stays cooked */
        } else {
            begin_transfer(s, cd->part[p] + (size_t)off * 2048u, bytes);
            if (getenv("SATURN_XFERDUMP")) {
                const uint8_t *q = cd->part[p] + (size_t)off * 2048u;
                unsigned k;
                fprintf(stderr, "[xfer] p=%u off=%u cnt=%u bytes=%u fad=%u first32:",
                        p, off, cnt, bytes, cnt ? cd->part_fad[p][off] : 0);
                for (k = 0; k < 32 && k < bytes; k++) fprintf(stderr, " %02X", q[k]);
                fprintf(stderr, "  as-text: '");
                for (k = 0; k < 16 && k < bytes; k++)
                    fprintf(stderr, "%c", (q[k] >= 32 && q[k] < 127) ? q[k] : '.');
                fprintf(stderr, "'\n");
            }
        }
        /* Ymir reports the status with kStatusFlagXferRequest (0x40) OR'd
         * into the status BYTE on a successful sector transfer:
         *   ReportCDStatus(GetStatusCode() | kStatusFlagXferRequest)
         * Its live trace shows 4180 4101 0100 00A6 here -- 0x41 is Pause|0x40,
         * not a status code of its own. We answered a bare 0x01, so a host
         * polling for `data staged, come and read it'' never saw the flag. */
        {
            uint8_t save = cd->status;
            cd->status = (uint8_t)(cd->status | ST_XFERREQ);
            respond_status(s);
            cd->status = save;
        }
        /* Ymir CmdGetSectorData / CmdGetThenDeleteSectorData: CMOK | DRDY.
         * EHST ("host I/O finished") is raised when the transfer ENDS, which
         * cdb_read_dtr already does -- raising it here as well told the host
         * the read was complete before it had pulled a single word. */
        hirq_set(s, HIRQ_DRDY);
        if (op == 0x63) {
            /* GetThenDeleteSectorData removes ONLY the sectors just handed
             * over, not the whole partition. Emptying it meant a host reading
             * a large file in chunks got the first chunk and then zeros --
             * NiGHTS reads /0NIGHTS (219 sectors) 4 sectors at a time and was
             * losing everything after the first. Shift the remainder down. */
            uint32_t taken = cnt;
            if (taken > cd->part_sectors[p]) taken = cd->part_sectors[p];
            if (taken) {
                uint32_t rest = cd->part_sectors[p] - (off + taken);
                if (off + taken <= cd->part_sectors[p] && rest) {
                    memmove(cd->part[p] + (size_t)off * 2048u,
                            cd->part[p] + (size_t)(off + taken) * 2048u,
                            (size_t)rest * 2048u);
                    memmove(cd->part_fad[p] + off, cd->part_fad[p] + off + taken,
                            (size_t)rest * sizeof(uint32_t));
                }
                cd->part_sectors[p] -= taken;
                cd->part_bytes[p]    = cd->part_sectors[p] * 2048u;
            }
        }
        break;
    }

    case 0x62: { /* DeleteSectorData (0x62 per Ymir): removes CR4 sectors at
                  * partition offset CR2, hands nothing over, compacts the
                  * rest. Nuking the whole partition here left a title that
                  * read 12 sectors and deleted the FIRST ONE polling forever
                  * for the 11 that vanished -- NiGHTS's post-launch loader
                  * stalled exactly there. */
        unsigned p = PNUM(cr3);
        uint32_t off = cr2, cnt = cr4 ? cr4 : 1;
        if (off > cd->part_sectors[p]) off = cd->part_sectors[p];
        if (cnt > cd->part_sectors[p] - off) cnt = cd->part_sectors[p] - off;
        if (cnt) {
            uint32_t rest = cd->part_sectors[p] - (off + cnt);
            if (rest) {
                memmove(cd->part[p] + (size_t)off * 2048u,
                        cd->part[p] + (size_t)(off + cnt) * 2048u,
                        (size_t)rest * 2048u);
                memmove(cd->part_fad[p] + off, cd->part_fad[p] + off + cnt,
                        (size_t)rest * sizeof(uint32_t));
            }
            cd->part_sectors[p] -= cnt;
            cd->part_bytes[p]    = cd->part_sectors[p] * 2048u;
        }
        respond_status(s);
        hirq_set(s, HIRQ_EHST);
        break;
    }

    case 0x50:  /* GetBufferSize */
        /* CR3 carries the selector count in its HIGH byte. */
        { uint32_t total = 0; int q;
          for (q = 0; q < CD_NUM_PARTS; q++) total += cd->part_sectors[q];
          respond(s, (uint16_t)((uint16_t)cd->status << 8),
                (uint16_t)(CD_PART_SECTORS * CD_NUM_PARTS - total),
                0x1800, (uint16_t)(CD_PART_SECTORS * CD_NUM_PARTS));
        }
        break;
    case 0x51: { /* GetSectorNumber */
        unsigned p = PNUM(cr3);
        respond(s, (uint16_t)((uint16_t)cd->status << 8), 0, 0,
                (uint16_t)cd->part_sectors[p]);
        /* Ymir CmdGetSectorNumber: CMOK only. This command is the hot poll of
         * every streaming driver, and every poll was asserting DRDY ("data
         * transfer ready") with nothing staged and ESEL ("selector settings
         * done") with no selector command outstanding. */
        break;
    }
    case 0x52: { /* CalculateActualSize: CR2 = sector offset, CR4 = count
                  * (Mednafen CALC_ACTSIZE printer). Answering with the WHOLE
                  * partition's size made a title that asked about its next
                  * 4-sector chunk see a 342KB answer and wait forever for a
                  * sane one -- NiGHTS's loader stalled at exactly that. */
        unsigned p = PNUM(cr3);
        uint32_t off = cr2, cnt = cr4 ? cr4 : 1;
        if (off > cd->part_sectors[p]) off = cd->part_sectors[p];
        if (cnt > cd->part_sectors[p] - off) cnt = cd->part_sectors[p] - off;
        cd->calc_words = cnt * (uint32_t)(cd->sec_len_get ? cd->sec_len_get : 2048u) / 2u;
        respond_status(s);
        hirq_set(s, HIRQ_ESEL);
        break;
    }
    case 0x53:  /* GetActualSize: result in words, CR1 low : CR2 */
        respond(s, (uint16_t)((cd->status << 8) | ((cd->calc_words >> 16) & 0xFF)),
                (uint16_t)(cd->calc_words & 0xFFFF), 0, 0);
        hirq_set(s, HIRQ_ESEL);
        break;

    case 0x70:  /* ChangeDirectory: scan the named directory. A fresh scan
                 * puts the file cursor at the first real entry -- ids 0 and 1
                 * are "." and ".." -- so GetFileSystemScope must report the
                 * first file id as 2 (Mednafen FileInfoOffs, Ymir
                 * GetFileOffset()+2). We answered 0, and the launcher walked
                 * a different, doomed path. */
        cd->dir_first = 2;
        respond_status(s);
        hirq_set(s, HIRQ_EFLS);
        break;

    case 0x71:
        printf("[ReadDirectory] CR1=%04X CR2=%04X CR3=%04X CR4=%04X\n",
               cr1, cr2, cr3, cr4);
        goto rd_body;
    rd_body: { /* ReadDirectory: select the directory whose contents the
                  * following GetFileInfo calls will report. CR3:CR4 carries
                  * the directory's file id; 0xFFFFFF means "the current one". */
        /* Rewind to the top of the directory. [TESTED] Making the sentinel
         * mean "carry on" instead (so the cursor persists across the
         * ReadDirectory that precedes every GetFileInfo) removes the 142M halt
         * but costs all VDP1 activity -- 8192 commands drop to 0. Net worse,
         * so it is not done. */
        if (getenv("SATURN_DIRSEL")) {
            /* [TEST] CR3's high byte as the target FILE ID. NiGHTS sends
             * CR3 = 0x17FF, and 0x17 = 23 is exactly /DEMO08.PRS -- one of the
             * files it actually loaded. If that is the selector, GetFileInfo
             * should answer with that single record instead of a window. */
            uint32_t sel = (cr3 >> 8) & 0xFF;
            if (sel >= 2) cd->dir_first = sel;
        }
        else if (!getenv("SATURN_DIRPERSIST"))
            cd->dir_first = 2;      /* 0 and 1 are "." and ".." */
        else if (cd->dir_first < 2)
            cd->dir_first = 2;
        respond_status(s);
        hirq_set(s, HIRQ_EFLS);
        break;
    }

    case 0x72: { /* GetFileSystemScope: how many entries the directory holds,
                  * and the id of the first one. */
        uint16_t n = 0;
        if (fs) {
            for (int i = 0; i < fs->nentries; i++)
                if (entry_in_cwd(&fs->entries[i])) n++;
            /* The host sizes its receive buffer from this count and then
             * reads back however many words GetFileInfo claims. If the count
             * exceeds what its buffer holds, the transfer runs off the end
             * into whatever follows -- here, the driver's own context. */
            {
                const char *capv = getenv("SATURN_DIRCAP");
                /* Report the TRUE number of files. Measured: the loader stall is
                 * caused by the SIZE OF THE TRANSFER, not by this count --
                 * report 7 / serve 254 stalls, report 254 / serve 7 proceeds.
                 * So tell the host the truth here and bound the batch in 0x73. */
                uint16_t cap = capv ? (uint16_t)strtoul(capv, NULL, 0) : 254;
                if (n > cap) n = cap;
            }
        }
        /* Report the files REMAINING from the current cursor, not the whole
         * directory. The host pages with ReadDirectory/GetFileInfo and uses
         * this count to decide whether more are left; a constant total makes
         * it stop early (it walked only 4 of 24 pages). */
        if (getenv("SATURN_DIRREMAIN") && cd->dir_first > 2) {
            uint32_t done = cd->dir_first - 2u;
            n = (uint16_t)(n > done ? n - done : 0);
        }
        cd->dir_count = n;
        /* CR2 = number of files in the directory, CR3:CR4 = the id of the
         * first one. The 0x0100 that used to be OR'd into CR3 was invented and
         * never validated; the field is just the high half of the id. */
        /* CR3's HIGH byte is the end-of-directory flag (Ymir
         * CmdGetFileSystemScope: (endOfDirectory << 8) | id-hi). Without it
         * the CD player's launcher waited ~12.5M cycles for directory pages
         * that never come, then gave up and aborted the boot. Our whole root
         * always fits one scope. */
        respond(s, (uint16_t)((uint16_t)cd->status << 8), n,
                (uint16_t)((1u << 8) | ((cd->dir_first >> 16) & 0xFF)),
                (uint16_t)(cd->dir_first & 0xFFFF));
        hirq_set(s, HIRQ_EFLS);
        break;
    }

    case 0x73: { /* GetFileInfo: 12 bytes per entry through the data transfer
                  * register. A file id of 0xFFFFFF asks for the whole
                  * directory at once, which is what NiGHTS does at boot. */
        uint32_t id = ((uint32_t)(cr3 & 0xFF) << 16) | cr4;
        printf("[GetFileInfo] CR1=%04X CR2=%04X CR3=%04X CR4=%04X dir_first=%u\n",
               cr1, cr2, cr3, cr4, (unsigned)cd->dir_first);
        uint8_t  buf[254 * 12];
        uint32_t bytes = 0;

        memset(buf, 0, sizeof(buf));
        if (fs) {
            /* Any id with the high byte set is the "whole directory" sentinel
             * (NiGHTS uses 0xFFFFF8). Tested: requiring exactly 0xFFFFFF makes
             * the lookup miss, GetFileInfo returns 0 bytes and the loader
             * regresses to a single PlayDisc. The buffer overrun is solved by
             * the record window above, not by narrowing this test. */
            if ((cr3 & 0xFF) == 0xFF || id >= 0x00FFFFFFu) {
                /* [EXPERIMENT] cap the batch so the caller's transfer cannot
                 * outrun its buffer while the length path is still suspect. */
                /* [PIN] Empirical: NiGHTS gives us a ~92-byte receive buffer for the
                     * directory (its request object sits 92 bytes past the
                     * destination), so more than 7 records overruns it and
                     * corrupts the request. Measured: 6-7 lets the loader
                     * proceed, >=12 does not. If this turns out to be
                     * per-title it belongs in games/<name>/game.toml, not
                     * here. */
                    /* The "id" with its high bits set is a NEGATIVE 24-bit
                     * count, not an index: NiGHTS passes 0xFFFFF8 = -8, i.e.
                     * "give me 8 records". That matches the caller's buffer,
                     * which has room for ~8 records before the request object
                     * that follows it -- and it derives the window size instead
                     * of us guessing it (we had empirically settled on 7). */
                    int derived = (int)(0x1000000u - id);
                    if (derived < 1 || derived > 254) derived = 7;
                    int cap = getenv("SATURN_FILEBATCH")
                            ? atoi(getenv("SATURN_FILEBATCH")) : derived;
                /* The "all files" batch returns dir_count records starting at
                 * dir_first, and NOTHING ELSE. It must agree exactly with what
                 * GetFileSystemScope reported, because the host sizes its
                 * buffer from that count and then reads however many words we
                 * claim: including the "." and ".." entries here made us send
                 * 193 records against a 191-record buffer, a 24-byte overflow
                 * straight into the caller's driver context. The dot entries
                 * are still served for an explicit id 0 / id 1 lookup below. */
                int fileno = 2;   /* ids start at 2; window starts at dir_first */
                {   /* [DIAGNOSTIC] SATURN_MODSEQ: serve the modules game.toml
                     * declares, one per GetFileInfo call, as record[0]. The host
                     * takes record[0] as its answer, so if these are the files it
                     * actually wants it should proceed. Proves or kills the
                     * "it wants the declared modules" hypothesis. */
                    static uint32_t seq[3] = { 175, 122, 121 };
                    { const char *o = getenv("SATURN_MODORDER");
                      if (o) { unsigned a,b,c;
                        if (sscanf(o, "%u,%u,%u", &a,&b,&c) == 3)
                          { seq[0]=a; seq[1]=b; seq[2]=c; } } }
                    const char *ms = getenv("SATURN_MODSEQ");
                    if (ms) {
                        static int call = 0;
                        cd->dir_first = seq[call % 3];
                        call++;
                    }
                }
                {   /* [DIAGNOSTIC] force the window to start elsewhere, to test
                     * whether the loader only needs a given file to be VISIBLE */
                    const char *sk = getenv("SATURN_DIRSKIP");
                    if (sk) cd->dir_first = (uint32_t)strtoul(sk, NULL, 0);
                }
                if (0) {
                    memset(buf + bytes, 0, 12);
                    uint32_t dfad = fs->root_lba + FAD_BASE;
                    uint32_t dsz  = fs->root_size ? fs->root_size : 2048u;
                    buf[bytes + 0] = (uint8_t)(dfad >> 24);
                    buf[bytes + 1] = (uint8_t)(dfad >> 16);
                    buf[bytes + 2] = (uint8_t)(dfad >>  8);
                    buf[bytes + 3] = (uint8_t)(dfad);
                    buf[bytes + 4] = (uint8_t)(dsz >> 24);
                    buf[bytes + 5] = (uint8_t)(dsz >> 16);
                    buf[bytes + 6] = (uint8_t)(dsz >>  8);
                    buf[bytes + 7] = (uint8_t)(dsz);
                    buf[bytes + 10] = (uint8_t)fileno;
                    buf[bytes + 11] = 0x02;     /* attribute: directory      */
                    bytes += 12;
                }
                for (int i = 0; i < fs->nentries && bytes < (uint32_t)cap * 12u && fileno < 254; i++) {
                    /* The CD block enumerates DIRECTORIES as entries too, so
                     * skipping them shifts every subsequent file id and the
                     * host's index lookup lands on the wrong record. */
                    if (!entry_in_cwd(&fs->entries[i])) continue;
                    if (fileno < (int)cd->dir_first) { fileno++; continue; }
                    file_record(&fs->entries[i], fileno, buf + bytes);
                    bytes += 12;
                    fileno++;
                }
                /* Advance the cursor so the NEXT GetFileInfo returns the next
                 * page. The host reads the directory in windows sized to its
                 * buffer (NiGHTS issues ReadDirectory/GetFileInfo repeatedly);
                 * pinning dir_first at 2 handed it the same first records every
                 * time, so it could never see files further down the list. */
                cd->dir_first = (uint32_t)fileno;
            } else {
                int fileno = 2, found = -1;
                for (int i = 0; i < fs->nentries; i++) {
                    if (!entry_in_cwd(&fs->entries[i])) continue;
                    if ((uint32_t)fileno == id) { found = i; break; }
                    fileno++;
                }
                if (found >= 0) {
                    file_record(&fs->entries[found], (int)id, buf);
                    bytes = 12;
                    if (cdlog())
                        printf("[cd:fi] id=%u -> %s lba=%u size=%u\n",
                               (unsigned)id, fs->entries[found].path,
                               (unsigned)fs->entries[found].lba,
                               (unsigned)fs->entries[found].size);
                }
                /* Single-file reply, exactly Ymir CmdGetFileInfo: CR2 = size
                 * in words, CR3 = CR4 = 0, HIRQ DRDY (CMOK via respond). Our
                 * shared tail also echoed the count into CR3:CR4 and raised
                 * EFLS -- the CD player's Start Application verify reads those
                 * and aborts (17x AbortFile), which is what kept the BIOS from
                 * booting the game. The batch path keeps its NiGHTS-measured
                 * behaviour. */
                begin_transfer(s, buf, bytes);
                respond(s, (uint16_t)((uint16_t)cd->status << 8),
                        (uint16_t)(bytes / 2), 0, 0);
                hirq_set(s, HIRQ_DRDY);
                break;
            }
        }
        /* The caller reads its response struct at +4, i.e. CR3:CR4, and uses
         * it (masked to 24 bits) as the number of words to pull through the
         * data transfer register. Reporting the length only in CR2 leaves that
         * pair zero and the caller runs on stack garbage. */
        /* Stream the directory: stage ALL of it once, then hand the host one
         * buffer-sized chunk per GetFileInfo. The data transfer register keeps
         * its position across commands, so the host's next read continues where
         * the last stopped -- that is how a directory larger than the caller's
         * buffer gets walked, without any cursor. Re-staging on every call is
         * what made us either overrun its buffer (all 191 at once) or hand it
         * the same first page forever. */
        if (getenv("SATURN_DIRSTREAM") && (cr3 & 0xFF) == 0xFF) {
            uint32_t chunk = 7u * 12u;
            if (cd->xfer_pos >= cd->xfer_size) {   /* start a fresh walk */
                begin_transfer(s, buf, bytes);
            }
            {
                uint32_t left = cd->xfer_size - cd->xfer_pos;
                uint32_t words = (left < chunk ? left : chunk) / 2u;
                respond(s, (uint16_t)((uint16_t)cd->status << 8), (uint16_t)words,
                        (uint16_t)(words >> 16), (uint16_t)(words & 0xFFFF));
                hirq_set(s, HIRQ_DRDY | HIRQ_EFLS);
            }
            break;
        }
        begin_transfer(s, buf, bytes);
        /* The records also land in the PARTITION, not just the data-transfer
         * register: the host discovers them by polling GetSectorNumber, and a
         * driver that waits for a non-zero sector count sits at "no data
         * available" (code 6) forever if we only serve them through the DTR. */
        if (getenv("SATURN_FIPART") && bytes) {
            /* [OFF BY DEFAULT] Also depositing the records in the partition
             * leaves a non-zero sector count that the game's read loop reads
             * as "more data available", so it never sees the 6 ("no more
             * data") that its caller at 0x0606B672 requires to proceed. */
            uint32_t secs = (bytes + 2047u) / 2048u;
            if (secs > CD_PART_SECTORS) secs = CD_PART_SECTORS;
            memset(cd->part[0], 0, (size_t)secs * 2048u);
            memcpy(cd->part[0], buf, bytes);
            cd->part_sectors[0] = secs;
            cd->part_bytes[0]   = bytes;
            hirq_set(s, HIRQ_CSCT);
        }
        {
            uint32_t words = bytes / 2;
            respond(s, (uint16_t)((uint16_t)cd->status << 8), (uint16_t)words,
                    (uint16_t)(words >> 16), (uint16_t)(words & 0xFFFF));
        }
        hirq_set(s, HIRQ_DRDY | HIRQ_EFLS);
        break;
    }

    case 0x74: { /* ReadFile: stream the file's sectors into the partition,
                  * exactly as a bounded play would (Mednafen COMMAND_READ_FILE
                  * = Filter_SetRange(file fad, sectors) + StartSeek(...,
                  * HIRQ_EFLS)). The BIOS launcher issues this and then DRAINS
                  * the partition with GetSectorNumber/GetThenDelete in a poll
                  * loop whose exit condition is drive-paused AND EFLS -- so
                  * EFLS must rise at the END of the play, not at issue.
                  * Staging the bytes in the transfer FIFO instead (the old
                  * model) left the partition empty and EFLS pre-raised: the
                  * loop exited instantly with 0 bytes and the player reloaded
                  * itself -- the launch-verdict bug. CR1 low + CR2 carry a
                  * sector offset into the file. */
        uint32_t id  = ((uint32_t)(cr3 & 0xFF) << 16) | cr4;
        uint32_t off = ((uint32_t)(cr1 & 0xFF) << 16) | cr2;
        /* File ids number the directory's FILES from 2, skipping subdirs --
         * the same mapping GetFileInfo uses. Indexing fs->entries[id]
         * directly landed on an unrelated entry (id 2 -> LBA 2600, a 14-
         * sector file) and streamed the wrong data past the real extent. */
        const iso_entry *e = NULL;
        if (fs) {
            int fileno = 2;
            for (int i = 0; i < fs->nentries; i++) {
                if (!entry_in_cwd(&fs->entries[i])) continue;
                if ((uint32_t)fileno == id) { e = &fs->entries[i]; break; }
                fileno++;
            }
        }
        if (e) {
            uint32_t nsec = (uint32_t)((e->size + 2047u) / 2048u);
            if (off > nsec) off = nsec;
            cd_set_position(s, FAD_BASE + e->lba + off);
            cd->play_end_fad = FAD_BASE + e->lba + nsec;
            cd->playing      = 1;
            cd->efls_at_end  = 1;
            cd->status       = ST_PLAY;
            respond_status(s);
        } else {
            cd->status = ST_PAUSE;
            respond_status(s);
        }
        /* Ymir CmdReadFile: CMOK | DRDY, on both the accepted and the
         * rejected path. */
        hirq_set(s, HIRQ_DRDY);
        break;
    }

    case 0x60:  /* SetSectorLength. CR1's low byte selects the "get" sector
                 * size and CR2's high byte the "put" size (0=2048, 1=2336,
                 * 2=2340, 3=2352). This had NO handler: it fell through to a
                 * bare status, so CR2..CR4 came back carrying the subcode-Q
                 * track/index/FAD instead of zero. It is the LAST command the
                 * game's CD driver issues before it stops making progress. */
        {
            /* 0xFF in either selector means LEAVE UNCHANGED -- masking it to
             * 3 silently reprogrammed the size to 2352. NiGHTS sends
             * CR2 = 0xFF00 ("keep the put size") on entry. */
            static const uint16_t len[4] = { 2048, 2336, 2340, 2352 };
            unsigned g = (unsigned)(cr1 & 0xFF), p = (unsigned)((cr2 >> 8) & 0xFF);
            /* Ymir gates on `< 4`, not on the 0xFF sentinel specifically: ANY
             * out-of-range selector leaves that length unchanged. */
            if (g < 4) cd->sec_len_get = len[g];
            if (p < 4) cd->sec_len_put = len[p];
        }
        /* Ymir CmdSetSectorLength: ReportCDStatus() -- the FULL status block --
         * then CMOK|ESEL. Its trace answers 01FF FFFF FFFF FFFF here, where we
         * used to answer 0100 0000 with CR2..CR4 zeroed. */
        respond_status(s);
        hirq_set(s, HIRQ_CMOK | HIRQ_ESEL);
        break;

    case 0x67:  /* GetCopyError. This had NO handler and fell through to a
                 * bare status, whose CR4 carries the low half of the subcode-Q
                 * FAD -- a large nonzero value that the driver reads as an
                 * error code. That is what drives the AbortFile-and-retry
                 * loop. The real reply is status in CR1 and a zero error. */
        respond(s, (uint16_t)((uint16_t)cd->status << 8), 0x0000, 0x0000,
                0x0000);
        /* Ymir CmdGetCopyError: CMOK only. ECPY means an async copy/move has
         * finished; this command only READS the error code. */
        break;

    case 0x75:  /* AbortFile */
        cd->xfer_size = cd->xfer_pos = 0;
        cd->playing = 0;
        cd->play_end_fad = 0;
        cd->efls_at_end = 0;
        respond_status(s);
        /* Ymir CmdAbortFile: CMOK | EFLS. (EndTransfer() only raises EHST for
         * a sector transfer that was actually in flight.) */
        hirq_set(s, HIRQ_EFLS);
        break;

    case 0xE0:  /* AuthenticateDevice — we always pass (the physical ring is
                 * not emulated), but NOT instantly: the real drive spends 2-4
                 * seconds seeking the security ring, and the BIOS overlaps the
                 * SEGA logo with it. Its sequencer then holds a minimum-
                 * display gate (frame > 130) INSIDE the V-Blank-OUT handler --
                 * safe on hardware because auth always outlasts the gate, a
                 * deadlock here when auth returned in the same field the logo
                 * started. Delay the completion; frames keep advancing while
                 * the host polls. */
        cd->status = ST_SEEK;
        /* Instant by default, like Ymir ("always authenticated"): the BIOS
         * happily accepts an immediate result -- a long delay overruns its
         * patience and lands on "Disc unsuitable for this system". The
         * timing cover for the logo's frame gate turned out to be the sound
         * handshake, not this. SATURN_AUTHDELAY!=0 re-enables the delay. */
        cd->auth_delay = getenv("SATURN_AUTHDELAY")
                       ? atoi(getenv("SATURN_AUTHDELAY")) : 0;
        if (cd->auth_delay <= 0) {
            cd->authenticated = 1;
            cd->status = ST_PAUSE;
            respond_status(s);
            hirq_set(s, HIRQ_EFLS | HIRQ_CSCT);
        }
        break;

    case 0xE1:  /* IsDeviceAuthenticated: 4 = original Saturn disc */
        respond(s, (uint16_t)((uint16_t)cd->status << 8),
                (uint16_t)(cd->authenticated ? 0x0004 : 0x0000), 0, 0);
        break;

    case 0xE2:  /* GetMPEGROM */
        respond(s, (uint16_t)((uint16_t)cd->status << 8), 0, 0, 0);
        break;

    ANY_DONE:
    case 0x30:  /* SetDeviceConnection: connect a CD filter to the true
                 * (drive) output. CR3's high byte carries the filter number.
                 * This had NO handler and fell through to a bare status --
                 * NiGHTS issues it once per load cycle, so the filter/partition
                 * routing it sets up never happened. */
        printf("[SetDevConn] CR1=%04X CR2=%04X CR3=%04X CR4=%04X\n",
               cr1, cr2, cr3, cr4);
        cd->cur_filter = (uint8_t)((cr3 >> 8) & 0xFF);
        cd->cd_conn = cd->cur_filter;
        if (cd->cd_conn >= CD_NUM_PARTS) cd->cd_conn = 0;
        respond_status(s);
        hirq_set(s, HIRQ_ESEL);
        break;

    case 0x48:  /* ResetSelector: clear filter/partition state. The host clears
                 * CMOK|ESEL (HIRQ write 0xFFBE) before issuing this and then
                 * waits for ESEL to come back -- without it NiGHTS' loader
                 * stops here forever. */
        /* Resets the FILTER/SELECTOR configuration. It must not discard an
         * in-flight data transfer: NiGHTS issues ResetSelector between reads,
         * and tearing down xfer_pos/xfer_size here handed it zeros on the next
         * read (measured as 1024 dry DTR reads). */
        /* What gets reset is selected by CR1's low byte, NOT by the command
         * alone (Ymir CmdResetSelector):
         *   flags == 0 -> clear ONLY the partition in CR3's high byte, and
         *                 leave every filter untouched
         *   else bit 2 buffer data, 3 partition outputs, 4 filter conditions,
         *        5 filter inputs, 6 pass outputs, 7 fail outputs
         * Resetting all filters unconditionally is wrong and actively harmful:
         * a game programs its filters, then issues flags==0 to recycle one
         * partition, and a blanket reset throws away the routing it just set
         * up. The BIOS issues 0x48FC (bits 2-7) for a genuine full reset, so
         * both callers are distinguished purely by these flags. */
        {
            uint8_t flags = (uint8_t)(cr1 & 0xFF);
            unsigned i;

            if (flags == 0) {
                unsigned p = PNUM(cr3);
                if (p < CD_NUM_PARTS) {
                    cd->part_sectors[p] = 0;
                    cd->part_bytes[p]   = 0;
                }
            } else {
                if (flags & 0x04) {            /* all buffer data */
                    memset(cd->part_sectors, 0, sizeof(cd->part_sectors));
                    memset(cd->part_bytes,   0, sizeof(cd->part_bytes));
                }
                if (flags & 0x10)              /* filter conditions */
                    for (i = 0; i < CD_NUM_FILTERS; i++) {
                        cd->filter[i].start_fad = cd->filter[i].fad_count = 0;
                        cd->filter[i].mode = 0;
                        cd->filter[i].file_num = cd->filter[i].chan_num = 0;
                        cd->filter[i].submode_mask = cd->filter[i].submode_val = 0;
                        cd->filter[i].coding_mask  = cd->filter[i].coding_val  = 0;
                    }
                if (flags & 0x20) {            /* filter inputs */
                    for (i = 0; i < CD_NUM_FILTERS; i++)
                        cd->filter[i].fail_out = CD_DISCONNECTED;
                    cd->cd_conn = CD_DISCONNECTED;
                }
                if (flags & 0x40)              /* pass outputs -> own index */
                    for (i = 0; i < CD_NUM_FILTERS; i++)
                        cd->filter[i].pass_out = (uint8_t)i;
                if (flags & 0x80)              /* fail outputs */
                    for (i = 0; i < CD_NUM_FILTERS; i++)
                        cd->filter[i].fail_out = CD_DISCONNECTED;
            }
            if (cdlog())
                printf("[cd:ressel] flags=%02X part=%u%s",
                       flags, PNUM(cr3), "\n");
        }
        respond_status(s);
        hirq_set(s, HIRQ_ESEL);
        break;

    default:
        /* Unknown command: answer with a normal status rather than REJECT.
         * A rejection sends game code down an error path -- NiGHTS jumps into
         * garbage -- whereas a benign status lets it continue while the
         * command shows up in the log as work still to do. [PIN] the real
         * semantics from the CD block manual before relying on this. */
        respond_status(s);
        /* Ymir's unimplemented-command path: status + CMOK, nothing else.
         * EFLS|EHST|ESEL here satisfied any wait the host happened to be in. */
        break;
    }
    LOGRESP();
}
