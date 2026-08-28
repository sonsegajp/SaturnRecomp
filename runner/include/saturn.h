/* saturn.h — clean-room Sega Saturn machine model.
 *
 * Memory is stored BIG-ENDIAN in the backing arrays, matching the hardware, so
 * byte accessors are plain indexing and only the 16/32-bit accessors assemble.
 *
 * Address decoding uses the bus address (addr & 0x07FFFFFF): on SH-2, bits
 * 31-29 select cache behaviour, not a device, so 0x06004000 and 0x26004000 are
 * the same byte of WRAM-H. See docs/HARDWARE.md §2.
 */
#ifndef SATURN_H
#define SATURN_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "m68k.h"

/* ------------------------------------------------------------ SH-2 state */

/* SR bits */
#define SR_T   (1u << 0)
#define SR_S   (1u << 1)
#define SR_I   (0xFu << 4)
#define SR_Q   (1u << 8)
#define SR_M   (1u << 9)
#define SR_MASK 0x000003F3u

typedef struct saturn saturn;

/* CD block state. See runner/src/cdblock.c. */
typedef struct {
    void    *disc;            /* disc *   (opaque here so this header stays
                               * independent of the recompiler's disc.h) */
    void    *iso;             /* iso_fs * */
    uint8_t  status;
    uint8_t  flag, ctrl, track, index;
    uint32_t fad;
    int      authenticated;
    int      ready;
    uint16_t cmd_stage[4];   /* CR1..CR4 as written by the host */
    uint32_t dir_first;      /* first file id in the selected directory */
    uint16_t dir_count;
    int      boot_delay;
    int      auth_delay;     /* fields until AuthenticateDevice completes */
    int      resp_pending;
    int      resp_fresh;
    /* Ymir CDBlock::m_processingCommand / m_readyForPeriodicReports.
     * Set when the host writes CR1 (it is starting a command packet), cleared
     * when it reads CR4 (it has collected the whole four-word reply). The
     * periodic drive report is suppressed in between, so it can never land on
     * top of a reply the host is halfway through reading. */
    int      processing_cmd;
    uint8_t  cur_filter;
    int      first_cmd_seen;
    int      init_busy;      /* ticks left of the spin-up after 0x04 */
    uint64_t last_peri_cy;   /* cycle of the last periodic report */
    uint32_t peri_seq;       /* moving subcode-Q low byte */
    uint16_t sec_len_get;    /* sector size for reads  */
    uint16_t sec_len_put;    /* sector size for writes */
    uint64_t periodic_count;      /* frames of power-on init left */

#define CD_SECTORS_PER_TICK 16   /* ~2x drive speed per frame */
/* Sectors the block can hold. NiGHTS issues PlayDisc for 219 sectors
 * (/0NIGHTS, 447,868 bytes) in one go, so 200 could not physically hold
 * the read and the transfer came up short. */
#define CD_PART_SECTORS 512      /* sectors one partition can hold */
#define CD_NUM_PARTS    24       /* Mednafen: Partitions[24] */
    uint8_t  (*part)[CD_PART_SECTORS * 2048];
    uint32_t (*part_fad)[CD_PART_SECTORS];
    uint32_t part_sectors[CD_NUM_PARTS];
    uint32_t part_bytes[CD_NUM_PARTS];
    uint32_t calc_words;

/* ---- selector: filters route incoming sectors to buffer partitions ------
 * A sector entering the block starts at the filter named by cd_conn
 * (SetDeviceConnection), is tested against that filter's conditions, and on
 * a PASS is stored in the partition named by pass_out. On a FAIL it moves to
 * the filter named by fail_out and is tested again, until a filter passes it
 * or the chain runs out (in which case the sector is discarded).
 *
 * This is what makes FMV work. A Saturn movie interleaves video and ADPCM
 * audio sectors in one MODE2 stream, distinguished only by the subheader's
 * channel/submode bytes; the player programs one filter per stream so each
 * lands in its own partition. Ymir: cdblock_filter.hpp, cdblock.cpp ~1095. */
#define CD_NUM_FILTERS  24       /* Ymir kNumFilters */
#define CD_DISCONNECTED 0xFFu    /* Ymir Filter::kDisconnected */
    struct {
        uint32_t start_fad;      /* FAD-range test, when mode bit 6 is set */
        uint32_t fad_count;
        uint8_t  mode;           /* b0 file b1 chan b2 submode b3 coding
                                  * b4 invert b6 FAD range                */
        uint8_t  file_num;
        uint8_t  chan_num;
        uint8_t  submode_mask, submode_val;
        uint8_t  coding_mask,  coding_val;
        uint8_t  pass_out;       /* partition number, or CD_DISCONNECTED  */
        uint8_t  fail_out;       /* next filter, or CD_DISCONNECTED       */
    } filter[CD_NUM_FILTERS];

    uint32_t cd_conn;
    int      playing;
    int      cdda_play;      /* the active play range is an audio track */
    uint32_t play_end_fad;
    uint32_t play_want;      /* sectors the host asked PlayDisc for */
    unsigned play_repeat;    /* CR3 play mode bits 3-0: repeats, 0xF = forever */
    unsigned read_speed;     /* 1 or 2; Ymir kDriveCyclesPlaying1x / readSpeed */
    uint64_t drive_next;     /* machine clock at which the next sector lands */
    unsigned play_rptcnt;    /* repeats performed so far; Ymir m_status.repeatCount,
                              * reported in RR0 bits 3-0 and counts UP to play_repeat */
    int      efls_at_end;
    uint8_t  *xfer;                        /* heap-allocated in cdb_init */
    uint32_t xfer_size, xfer_pos;
    /* Ymir TransferType. EHST is raised at EndDataTransfer ONLY for real
     * sector transfers (GetSector/GetThenDeleteSector/PutSector), never for
     * TOC/FileInfo/Subcode. 0 = none, 1 = sector, 2 = other. */
    uint8_t  xfer_type;
} cdblock;

typedef struct {
    uint32_t r[16];
    uint32_t sr, gbr, vbr;
    int      frt_pend;           /* cached: an enabled FRT flag is raised */
    uint32_t mach, macl, pr;
    uint32_t pc;

    uint64_t cycles;
    uint64_t run_target;       /* cumulative scheduler clocks owed to core */
    int      sleeping;
    int      halted;          /* set on an unrecoverable fault */
    int      is_slave;
    const char *fault;        /* reason, when halted */
    uint32_t fault_pc;

    /* SH-2 on-chip peripherals, 0xFFFFFE00-0xFFFFFFFF (bits 31-29 == 111).
     * One bank PER CORE, because that is what the hardware has: master and
     * slave each own their FRT, INTC, DMAC and BSC. A single shared bank meant
     * the slave's FTCSR poll was reading the master's flags -- and the master's
     * writes were landing in the register the slave was waiting on. */
    uint8_t  onchip[0x200];
    /* SH-2 CACHE DATA ARRAY, 4KB, directly addressable at 0xC0000000-0xC0000FFF
     * (Ymir sh2.hpp: "Data array, read/write space", dispatched on address bits
     * 31-29 == 0b110, with an undocumented mirror at 0x80000000-0x9FFFFFFF).
     *
     * This is the cache itself used as fast local RAM, which is a standard SGL
     * technique -- NiGHTS puts the SLAVE'S STACK here (r15 = 0xC00007E8) and
     * bases a work structure at 0xC0000000. With this unimplemented, our bus
     * folded 0xC0000000 to 0x000007E8 and every push landed in BIOS ROM: the
     * slave's saves were discarded and its restores read ROM contents back, so
     * a saved loop counter came back as a BIOS pointer.
     *
     * Per CORE, because it is that CPU's own cache. */
    uint8_t  cache_data[0x1000];
    /* Unified SH-2 cache metadata: 64 sets x 4 ways x 16-byte lines.  The
     * line bytes use the hardware data-array layout already represented by
     * cache_data: (way << 10) | (set << 4) | byte. */
    uint32_t cache_tag[64][4];   /* address bits 28..10 */
    uint8_t  cache_valid[64][4];
    uint8_t  cache_lru[64];      /* SH7604's six LRU state bits */
    /* Opcode-fetch page cache: host pointer into the live memory array for
     * the 4KB page PC last executed from. Tag 1 never matches an address. */
    const uint8_t *if_page;
    uint32_t if_tag;
    /* Most instruction fetches stay in the same 16-byte SH-2 cache line.
     * Remember its way so sequential code does not scan all four ways for
     * every opcode; metadata is still validated before every use. */
    uint32_t if_cache_base;
    uint8_t  if_cache_way;
    uint16_t frc;             /* free-running counter                        */
    uint32_t frt_pre;         /* prescaler remainder, in machine cycles      */

    saturn  *sys;
} sh2;

/* --------------------------------------------------------------- machine */

#define WRAM_L_SIZE   0x100000u
#define WRAM_H_SIZE   0x100000u
#define BIOS_SIZE     0x080000u
#define VDP1_VRAM_SZ  0x080000u
#define VDP1_FB_SZ    0x040000u
#define VDP2_VRAM_SZ  0x080000u
#define CRAM_SIZE     0x001000u
#define SOUND_RAM_SZ  0x080000u
/* Internal backup RAM: 32KB of DATA, but the chip is byte-wide on a 16-bit
 * bus, so it occupies 64KB of address space with the data in the ODD bytes
 * (Ymir backup_ram.cpp: ReadByte(a) = DataReadByte(a >> 1)). */
#define BUP_SIZE      0x010000u

/* How many distinct unmapped/unimplemented accesses to report before going
 * quiet. Boot code touches a lot of hardware; we want the first sightings. */
#define TRACE_SLOTS 1024
#define CDCMD_SLOTS 32


/* One SCSP PCM slot's playback state: the register file holds what the driver
 * wrote, this holds where the slot has actually got to. */
typedef enum {
    SCSP_ENV_ATTACK = 0, SCSP_ENV_DECAY1, SCSP_ENV_DECAY2, SCSP_ENV_RELEASE
} scsp_env_phase;

#define SCSP_ENV_MAX  0x3FF000u

/* Output ring: about a third of a second at 44.1kHz, which is plenty of slack
 * for a host audio callback that wakes every few milliseconds. */
/* ---- CD digital audio (CDDA) -------------------------------------------
 * A CD audio track is raw 16-bit stereo PCM at 44.1kHz -- exactly the SCSP's
 * output rate -- so the CD block hands whole 2352-byte sectors to the sound
 * chip and the SCSP drains four bytes per output sample. Ymir does this via
 * SCSP::ReceiveCDDA into a ring, then feeds DSP EXTS 0/1 each sample
 * (scsp.cpp:1053). EXTS reaches the final mix through the EFSDL/EFPAN fields
 * in slots 16 and 17; bypassing those controls makes CDDA plus PCM clip.
 *
 * Eight sectors: enough that a drive delivering one sector per tick cannot
 * underrun, small enough that audio stays close to the video it was
 * interleaved with. */
/* Fifteen sectors, matching Ymir's `m_cddaBuffer[2352 * 15]`. Eight left so
 * little slack that a drive delivering a short burst overran the ring. */
#define CDDA_RING     (2352u * 15u)

#define SND_RING      16384u
/* Ring fill the window's pacer steers toward, in stereo frames (~70 ms).
 * Must stay comfortably above one SDL callback (1024 frames) so a late
 * field cannot starve the device mid-buffer. */
/* Frames of audio to keep buffered before the pacer starts steering. This is
 * LATENCY: 3072 frames is ~70 ms at 44.1 kHz, and because the field pacer
 * nudges its period to hold this fill, the VIDEO follows a deep audio buffer.
 * That is how the runner can report a solid 59.8 fps and still feel laggy
 * against Ymir. 1024 frames is ~23 ms; the underrun ramp in audio_cb covers
 * the occasional miss. SATURN_SNDBUF=<frames> overrides it. */
#define SND_TARGET     1024u

typedef struct {
    int      active;
    uint32_t sa;          /* sample start address in sound RAM */
    /* Ymir Slot::currSample: a 16-bit counter that WRAPS, and that holds the
     * one's complement of the index while playing backwards, so the loop test
     * `(reverse ? ~pos : pos) + 1 > loopPoint` is the same expression in both
     * directions. The fetch un-complements it again. */
    uint16_t pos;
    uint32_t frac;        /* 10-bit phase fraction             */
    uint32_t env;
    uint32_t env_prev;   /* level BEFORE the last EG step; Ymir tests this
                          * one against 0x3C0 to deactivate the slot */         /* envelope level                    */
    int      phase;       /* scsp_env_phase                    */
    int      reverse;     /* playing backwards                 */
    int      crossed;     /* passed LSA: the loop segment is live now */
    int32_t  output;      /* previous completed pipeline output; slots
                           * 26-31 feed it at the next sample boundary */
} scsp_slot;

typedef struct {
    uint32_t addr;
    uint32_t count;
    uint8_t  is_write;
    uint8_t  size;
} trace_slot;

/* SCSP DSP effect processor state (Ymir scsp_dsp.hpp). */
typedef struct {
    uint64_t program[128];   /* MPRO, 60-bit instructions */
    int32_t  temp[128];      /* TEMP, 24-bit */
    int32_t  mems[32];       /* MEMS, 24-bit */
    uint16_t coef[64];       /* COEF, 13-bit stored << 3 */
    uint16_t madrs[32];      /* MADRS */
    int32_t  mixs[32];       /* MIXS, double-banked */
    int16_t  efreg[16];      /* EFREG, the effect output the mixer reads */
    int16_t  exts[2];        /* EXTS, external digital audio in */

    uint32_t pc, prog_len;
    uint32_t sft_reg, frc_reg, y_reg, adrs_reg;
    uint32_t mdec_ct;
    int32_t  inputs;
    uint32_t rw_addr, read_value;
    uint16_t write_value;
    uint32_t rbp, rbl;
    unsigned mixs_gen, mixs_null;
    int      read_pending, write_pending, read_nofl;
} scsp_dsp_state;

/* SCU geometry DSP state. This is independent of the SCSP audio DSP above. */
typedef struct {
    uint32_t program[256];
    uint32_t data[4][64];
    uint8_t executing, paused, ended, step;
    uint8_t pc, data_addr;
    uint8_t sign, zero, carry, overflow;
    uint8_t ct[4];
    uint32_t inc_ct;
    uint64_t alu, ac, p;             /* 48-bit registers */
    int32_t rx, ry;
    uint8_t loop_top;
    uint16_t loop_count;
    uint8_t looping;
    uint8_t dma_run, dma_to_d0, dma_hold, dma_count;
    uint8_t dma_src, dma_dst;
    uint32_t dma_read_addr, dma_write_addr, dma_addr_inc, dma_addr_d0, dma_pc;
    uint32_t next_instr;
    uint64_t cycle_spill;
} scu_dsp_state;

struct saturn {
    sh2      master, slave;
    int      slave_enabled;
    uint32_t slave_entry, slave_sp;  /* registered via BIOS slot 0x06000358 */
    int      slave_started;

    uint8_t  wram_l[WRAM_L_SIZE];
    uint8_t  wram_h[WRAM_H_SIZE];
    uint8_t  bios[BIOS_SIZE];
    uint8_t  vdp1_vram[VDP1_VRAM_SZ];
    uint8_t  vdp1_fb[2][VDP1_FB_SZ];
    /* Separate VDP1 mesh framebuffers, matching Ymir's transparent-mesh
     * enhancement.  A mesh command must not overwrite the ordinary sprite
     * framebuffer: that framebuffer may already contain the character or
     * scenery that is meant to show through the mesh.  The old implementation
     * kept only a one-byte flag and therefore lost that underlying pixel. */
    uint8_t  vdp1_meshfb[2][VDP1_FB_SZ];
    /* Frame interpolation (SATURN_INTERP): a synthesized midpoint frame is
     * rasterised HERE, never into the real framebuffers, so the swap/erase
     * cadence the game drives is untouched. `vdp1_fb_override` redirects the
     * rasteriser for that one pass. */
    uint8_t  vdp1_interp_fb[VDP1_FB_SZ];
    uint8_t  vdp1_interp_mesh[VDP1_FB_SZ];
    uint8_t *vdp1_fb_override, *vdp1_mesh_override;
    int      vdp1_interp_pass;      /* suppress bookkeeping during that pass */
    int      vdp1_interp_ready;     /* a midpoint frame is available to show */
    int      vdp1_show_interp;      /* the compositor should show it now     */
    uint8_t  vdp2_vram[VDP2_VRAM_SZ];
    uint8_t  cram[CRAM_SIZE];
    uint64_t vdp2_vram_epoch, cram_epoch; /* invalidate rendered BG cache */
    uint8_t  sound_ram[SOUND_RAM_SZ];

    /* ---- sound: MC68000 + SCSP ---- */

    uint16_t  scsp_reg[0x1000u / 2u];
    scsp_dsp_state dsp;
    int       scsp_kyonex;         /* KYONEX latched by a register write,
                                    * consumed once per sample step (Ymir) */
    scsp_slot scsp_slot[32];
    m68k      sound_cpu;          /* the sound-side MC68000            */
    int       sound_on;           /* SMPC SNDON has released the 68000 */
    uint64_t  m68k_acc;           /* fractional sound-CPU clock        */
    uint64_t  m68k_target;        /* cumulative cycles owed since reset */
    uint64_t  scsp_acc;           /* sample-clock accumulator          */
    uint32_t  sound_deferred;     /* SH-2 clocks waiting for sound sync */
    int16_t   snd_buf[SND_RING * 2];  /* stereo ring, written by the core */
    uint32_t  snd_wp, snd_rp;
#define COVER_BLOCKS 65536u   /* 32-byte blocks -> 2MB of code */
    uint8_t   cover[COVER_BLOCKS/8];
    struct { uint32_t addr, val; uint64_t cy, period; } poke[8];
    int       npoke;
    uint64_t  snd_nonsilent;      /* samples the SCSP actually made noise on */
    uint8_t   cdda[CDDA_RING];    /* raw CD audio awaiting playback */
    uint32_t  cdda_wp, cdda_rp;
    int       cdda_ready;         /* set once enough is buffered to start */
    /* Rate accounting. `pushed * 588` should equal `drained` over a track;
     * any gap is audible as the music running fast or stuttering, and these
     * say WHICH of the two without having to listen to it. */
    uint64_t  cdda_pushed, cdda_drained;
    uint64_t  cdda_over, cdda_under;
    uint8_t   scsp_timer[3];
    uint8_t   scsp_timer_reload[3];
    uint64_t  scsp_sample_ctr;
    uint8_t  bup[BUP_SIZE];

    uint16_t vdp1_reg[16];       /* 0x05D00000, word-addressed  */
    uint16_t vdp2_reg[256];      /* 0x05F80000, word-addressed  */
    uint32_t scu_reg[64];        /* 0x05FE0000, long-addressed  */
    scu_dsp_state scu_dsp;
    uint8_t  smpc_reg[0x80];     /* 0x00100000, odd bytes       */
    uint16_t cdb_reg[0x20];      /* 0x05890000                  */
    cdblock  cd;

    int      fb_draw;            /* which VDP1 framebuffer is the draw target */

    /* VDP1 status registers, modelled on Ymir vdp1_regs.hpp. EDSR/LOPR/COPR
     * and MODR are the only readable VDP1 registers; TVMR through ENDR are
     * write-only and read back 0 on hardware. */
    uint32_t vdp1_copr;          /* COPR: address of the command in progress */
    uint32_t vdp1_lopr;          /* LOPR: same, latched at the last swap     */
    uint8_t  vdp1_cef;           /* EDSR bit 1: end bit fetched this frame   */
    uint8_t  vdp1_bef;           /* EDSR bit 0: end bit fetched last frame   */
    uint8_t  vdp1_fbparams;      /* FBCR written since the last field change */
    uint16_t vdp1_ew_val;        /* latched EWDR                             */
    uint16_t vdp1_ew_x1, vdp1_ew_y1, vdp1_ew_x3, vdp1_ew_y3;   /* EWLR/EWRR  */

    /* SMPC */
    uint8_t  smpc_busy;
    uint8_t  smem[4];        /* SMPC backup memory        */
    int      smpc_ste;       /* set by SETSMEM/SETTIME; INTBACK OREG0 bit 7 */
    int      smpc_resd;      /* RESDISA latch; INTBACK OREG0 bit 6          */
    char     smpc_state_path[512];  /* where STE/SMEM persist between runs */
    uint8_t  pad1_lo, pad1_hi;/* controller 1, active high */
    /* Console region, as the SMPC reports it in INTBACK OREG9. The BIOS
     * checks the disc's IP.BIN area list against this and refuses to boot a
     * disc that does not name it -- a mismatch drops you at the CD player,
     * which is exactly what a hardcoded 'North America' did to a Japanese
     * disc on the Japanese BIOS. 0 means 'not set', treated as 4. */
    uint8_t  area_code;
    uint64_t pad_at;         /* SATURN_PADAT: hold buttons only after this master cycle */
    /* SATURN_PADSEQ="cy:lo,cy:lo,..." -- from each cycle mark, present that
     * pad byte until the next mark. Lets a headless run navigate menus. */
    struct { uint64_t cy; uint8_t lo; uint8_t hi; } padseq[64];
    int npadseq;
    int      clock_352;
    int      dtr_fill;
    uint32_t dtr_seq;
    int      cd_map;         /* SATURN_CDMAP: CR write-mapping candidate */
    struct { uint8_t cmd; uint32_t count; } smpccmd[32];
    int      nsmpccmd;
    uint8_t  intback_ireg[8][2];
    /* INTBACK phase state: the command is a PROTOCOL, not a single reply.
     * Status first; the host then writes IREG0 bit 7 as a CONTINUE request
     * (not a new command) for the peripheral block, or bit 6 to break.
     * Contract per Ymir smpc.cpp WriteINTBACK*Report and Mednafen smpc.cpp. */
    int      ib_active;      /* peripheral phase still owed              */
    int      ib_first;       /* next peripheral report is the first      */
    uint8_t  ib_p1md, ib_p2md;
    int      nintback;

    /* The core whose bus access is in flight. On-chip register accesses route
     * to this core's bank; every other region is genuinely shared. Defaults to
     * the master so bus access from outside CPU execution (image loading,
     * debug dumps) still resolves. */
    sh2     *cur;

    /* Global machine clock, in SH-2 cycles. Video timing must be derived from
     * this and never from a core's own `cycles`: while the master runs the
     * slave's counter is frozen and vice versa, so a per-core counter makes
     * H-Blank stand still for whichever core is not currently scheduled. */
    uint64_t clk;
    uint32_t line;            /* scanline being executed, 0..LINES_TOTAL-1   */
    /* saturn_run_field stops exactly when V-Blank-IN is asserted, like
     * Ymir's RunFrame.  The next call must execute that scanline without
     * asserting the same edge a second time.  This byte fits in the existing
     * alignment gap before `frames`, so raw diagnostic snapshots keep their
     * layout. */
    uint8_t  vblank_boundary_done;
    uint64_t frames;
    uint64_t frt_irqs;        /* interrupts sourced from a core's own FRT */
    /* SATURN_PROF: rdtsc cycle buckets, printed at end of run. */
    uint64_t prof_master, prof_slave, prof_video, prof_other;
    uint64_t fastpath_hits, slowpath_hits;
    uint32_t irqall_mach, irqall_macl, irqall_gbr;
    /* SATURN_PCLAST: the LAST 16 times a PC was reached, with registers.
     * SATURN_PCLOG keeps the FIRST 16, which are the healthy ones when a
     * routine works for a while and only later stops returning. */
    uint32_t pclast_pc;
    uint32_t dumpat_pc;   /* SATURN_DUMPAT: dump guest memory when this PC
                           * is first executed, not at exit -- overlays are
                           * overwritten before the run ends */
    int      dumpat_done;
    int      dumpat_n;    /* SATURN_DUMPAT_N: which hit to dump on (default 1) */
    struct { uint32_t pc, pr, r[16]; uint64_t cy; } pclast[16];
    uint32_t pclast_head;

    uint64_t dma_transfers;
    uint64_t dma_bytes;

    /* SCU interrupt controller. Pending bits use the SCU manual's numbering
     * (docs/HARDWARE.md §4.1); mask/status are also visible as SCU registers
     * at 0x25FE00A0 / 0x25FE00A4. */
    uint32_t scu_ipend;
    uint64_t irqs_taken;
    int      irq_depth, irq_depth_max, irq_last_level;
    int      irq_nest_outer, irq_nest_inner;
    uint32_t irq_nest_pc;
    uint64_t scu_dma_transfers, scu_dma_bytes;
    uint64_t vdp1_lists, vdp1_commands, vdp1_pixels;
    struct { uint32_t src, dst, cnt, md, ad; } dmalog[16];
    int      ndmalog;
    struct { uint32_t src, dst, cnt, chcr; } ocdmalog[16];
    int      nocdmalog;
    struct { uint32_t addr, val, pc; } cdwlog[24];
    int      ncdwlog;
    uint64_t cdwrites;
    struct { uint8_t op; uint16_t cr1,cr2,cr3,cr4; } cdresp[16];
    int      ncdresp;
    struct { uint32_t bytes; uint8_t head[12]; uint8_t more[24]; } xferlog[8];
    int      nxferlog;
    uint32_t rwatch_addr;
    struct { uint32_t pc; uint64_t count; } rwatch[16];
    int      nrwatch;
    uint32_t watch_first[16];
    uint32_t bal_a, bal_b, bal_sp, bal_bad_sp, bal_bad_exp;
    uint64_t bal_ok, bal_bad;
    int      bal_seen;
    uint32_t trace_r0[24], trace_r4[24], trace_prev[24];
    int nltrace;
    uint64_t watch_zero;
    uint32_t watch_zero_prev;
    uint32_t prev_pc;
    uint32_t r0_prev;
    uint64_t dtr_dry, dtr_ok;
    uint64_t staged_total; int nstaged;
    struct { uint32_t want, have, off; } sdlog[12];
    int nsdlog;
    struct { uint32_t size, prev_size, prev_read; } stagelog[12];
    uint32_t wrange_lo, wrange_hi;
    uint32_t rrange_lo, rrange_hi;
    struct { uint32_t pc; uint64_t n; } rrlog[16];
    int      nrrlog;
    struct { uint32_t addr, val, pc; int sz; uint64_t cy; } wrlog[64];
    int      nwrlog;
    uint64_t first_irq_cy;
    uint32_t r13w_pc[8], r13w_old[8], r13w_new[8];
    int nr13w;
    uint32_t r13_prev;
    uint32_t irqsave[7];      /* r8..r14 at interrupt entry */
    int      irqsave_valid;
    uint64_t irq_clobber[7];  /* how often each came back changed */
    uint32_t irq_clobber_pc;  /* interrupted PC of the first clobber */
    uint32_t irq_clobber_old, irq_clobber_new;
    int      irq_clobber_reg;  /* -1 until set */
    struct { uint64_t cy; uint32_t pc, val; int sz, slave; } wlog[12];
    int      nwlog;
    uint64_t watch_cy;   /* cycle of the first hit on watch_pc */
    uint64_t wwatch_first_cy, wwatch_last_cy;

#define RING_N 4096
    uint32_t ring_pc[RING_N], ring_r0[RING_N], ring_r4[RING_N];
    uint32_t ring_head;
    int      ring_frozen;
    int      ring_any;
    uint64_t ring_skip;
    uint32_t ring_trig_pc;   /* freeze when this PC runs with r0==0 */

    uint32_t irqpc[8], irqpc_r0[8];
    int nirqpc;
    uint32_t r0chg_pc[8], r0chg_next[8];
    int nr0chg;
    struct { uint32_t pc; uint64_t n; } pred[12];
    int      npred;
    uint64_t cr_snap;
    uint64_t cr_changes;
    int      cr_snap_valid;
    struct { uint16_t val; uint32_t pc; } cr2log[12];
    int      ncr2log;
    uint64_t cr1reads;
    uint32_t badcr_pc[8]; uint16_t badcr_val[8]; int nbadcr;

    /* BIOS service calls observed (runner/src/bios.c). */
    struct { uint32_t slot, r4, r5, r6, r7, count; } bioscall[32];
    int      nbioscall;

    /* CD block command log (see cdb_command). */
    struct { uint8_t op; uint16_t cr1, cr2, cr3, cr4; uint32_t count; }
             cdcmd[CDCMD_SLOTS];
    int      ncdcmd;

    /* diagnostics */
    trace_slot trace[TRACE_SLOTS];
    int        ntrace;
    int        trace_enabled;
    uint64_t   unmapped_reads, unmapped_writes;
    uint32_t   watch_pc;      /* diagnostic: count executions at a PC */
    uint64_t   watch_hits;
    uint32_t   watch_regs[16];
    uint32_t   watch_pr;
    unsigned   min_imask;
    uint32_t   min_imask_pc;
    uint32_t   last_open_pc, first_mask_pc;
    uint64_t   last_open_cy;
    unsigned   armed, nmaskings;
#define HOTPC_SLOTS 4096
    struct { uint32_t pc; uint64_t n; } hotpc[HOTPC_SLOTS];
    struct { uint32_t pc; unsigned imask; uint64_t n; } ldcsr[32];
    int      nldcsr;
    uint32_t   wwatch_addr;   /* diagnostic: watch writes to an address */
    uint64_t   wwatch_hits;
    uint32_t   wwatch_last;
    uint32_t   wwatch_pc;
    uint32_t   cd_read_pc;   /* PC of the first CD register read */
    uint32_t   cd_read_addr; /* address of that read */
    uint32_t   watch_gbr;    /* GBR at the watch hit */
    /* SMPC clock change state: 0 idle, 1 scheduled, 2 reset complete/NMI
     * ready. The BIOS must reach SLEEP before the command completes. */
    int        pending_ckchg;
    uint8_t    ckchg_cmd;      /* which CKCHG: 0x0E boot-352, 0x0F game-320 */
    uint64_t   ckchg_due;
    uint32_t   irqall[16];
    uint32_t   irqall_pc;
    int        irqall_valid, irqall_reported;
    unsigned   layer_mask;
    int        layer_lock;   /* caller drives layer_mask; ignore SATURN_LAYERS */
    uint32_t   ring_trig_pr;
    int        pclog_n;
    int        pcrel_logged;
    int        hle_active;   /* BIOS HLE stubs installed (no real BIOS) */
    uint32_t   slave_restarts;
    uint32_t   sring[256];   /* slave PC ring */
    uint32_t   sring_head;
    uint32_t   mring[256];   /* master PC ring */
    uint32_t   mring_head;
    /* VDP1 local coordinate origin. This is a REGISTER, not per-list state: it
     * holds until another Local Coordinates command changes it. Keeping it in
     * the per-list context meant it reset to (0,0) every frame, so a game that
     * sets the origin once drew everything shifted by half a screen. */
    int32_t    vdp1_local_x, vdp1_local_y;
    /* Same story for the clipping registers: set by commands 0x8/0x9, held
     * until changed. -1 in vdp1_usr_x1 means "never set, use the framebuffer". */
    int32_t    vdp1_usr_x0, vdp1_usr_y0, vdp1_usr_x1, vdp1_usr_y1;
    int32_t    vdp1_sys_x1, vdp1_sys_y1;
    int        vdp1_clip_set;
    uint32_t   prot_lo, prot_hi; /* game image guarded from BIOS writes */
    int        vdp1_erase_pending; /* deferred VDP1 erase, see bus.c field boundary */
    uint32_t   regat_pc;         /* SATURN_REGAT: dump regs at this PC, any core */
    uint32_t   regat_n;
    uint32_t   snap_addr;    /* region to freeze when the ring trips */
    uint16_t   snap[128];
    int        snap_taken;
    uint64_t   irqvec_hist[128];
    uint64_t   scu_raise_hist[32];
    uint64_t   scsp_silent_loops;  /* loops taken by an already-silent slot */
    uint64_t   scsp_keyons;        /* key-ON edges taken -- a hard proxy for
                                    * whether sound effects fire at all */
    uint32_t   scsp_peak;          /* loudest |sample| the mixer produced */
    uint64_t   scsp_energy;        /* sum of |L|+|R|; sensitive where peak saturates */
    uint64_t   dsp_sends;          /* slot samples pushed to MIXS (IMXL>0) */
    /* RUNNING peaks. The probe used to read dsp.efreg/dsp.mixs once, at exit,
     * and report that instant as a maximum -- so a run that ended during a
     * quiet moment reported the effect path as dead even when it was working.
     * That single misreading is what made the SCSP DSP look broken. */
    int32_t  dsp_efreg_peak, dsp_mixs_peak;
    unsigned dsp_efsdl_max, dsp_efret_slots;
    /* Delay-line probe: a reverb TAIL lives in the sound-RAM ring buffer, so
     * if MRD/MWT never execute or always read zero there can be no tail and
     * the effect degenerates to roughly the dry signal. */
    uint64_t dsp_mrd, dsp_mwt;
    int32_t  dsp_readval_peak;
    uint64_t   dsp_reg14_nonzero;  /* times a slot's ISEL/IMXL reg was non-zero */
    uint64_t   dsp_mpro_writes;    /* SCSP DSP program words written */
    uint64_t   dsp_coef_writes;
    uint64_t   dsp_madrs_writes;
    uint64_t   scsp_nonsilent;     /* output samples that were not zero */
    /* SCU timers (Ymir scu.cpp). Timer 0 compares a per-LINE counter against
     * T0C; Timer 1 is a delay within the line. We had neither: Timer 0 was
     * faked as "once per field if unmasked" and Timer 1 was never raised at
     * all, so any title pacing on a scanline match got the wrong tick. */
    uint16_t   scu_t0_compare;
    uint16_t   scu_t0_counter;
    uint16_t   scu_t1_reload;
    uint8_t    scu_t1_mode;      /* T1MD bit 8: 1 = only on the T0 line */
    uint8_t    scu_timer_enable; /* T1MD bit 0 */
};

/* ------------------------------------------------------------------ bus */

uint8_t  bus_r8 (saturn *s, uint32_t a);
uint16_t bus_r16(saturn *s, uint32_t a);
uint32_t bus_r32(saturn *s, uint32_t a);
void mem_dump_now(saturn *s);
void mem_dump_at(saturn *s, int at_hit);
void scsp_dsp_reset(saturn *s);
void scsp_dsp_step(saturn *s);
void scsp_dsp_mixs_write(saturn *s, unsigned off, int32_t value);
void scsp_dsp_update_rbp(saturn *s, uint16_t lead);
void scsp_dsp_update_rbl(saturn *s, uint16_t len);
void smpc_persist_load(saturn *s);
void smpc_persist_save(saturn *s);

void     bus_w8 (saturn *s, uint32_t a, uint8_t  v);
void     bus_w16(saturn *s, uint32_t a, uint16_t v);
void     bus_w32(saturn *s, uint32_t a, uint32_t v);
void     scu_dma_trigger(saturn *s, unsigned factor);
void     scu_dsp_tick(saturn *s, uint32_t cycles);
void     scu_dsp_soft_reset(saturn *s);
uint32_t scu_dsp_read_reg(saturn *s, uint32_t off);
void     scu_dsp_write_reg(saturn *s, uint32_t off, uint32_t value);

/* BIOS HLE service traps. */
#define BIOS_HLE_BASE   0x0000C000u
#define BIOS_HLE_COUNT  128u  /* covers 0x06000200-0x060003FF */
#define BIOSCALL_SLOTS  32
void     bios_hle_traps_install(saturn *s);
int      bios_hle_is_trap(saturn *s, uint32_t pc);
int      bios_hle_call(saturn *s, uint32_t pc, uint32_t *r);

/* VDP1 command list execution (runner/src/vdp1.c). */
void     vdp1_execute(saturn *s);
void     vdp1_begin_frame(saturn *s);
void     vdp1_tick(saturn *s, uint32_t cycles);
void     vdp1_soft_reset(saturn *s);
void     vdp1_swap(saturn *s);
void     vdp1_erase(saturn *s);
uint16_t vdp1_read_reg(saturn *s, uint32_t off);
void     vdp1_write_reg(saturn *s, uint32_t off, uint16_t v);

/* Live hardware view (runner/src/debugview.c). */
void     debugview_render(saturn *s, uint32_t *out, int W, int H);

/* VDP2 background rendering (runner/src/vdp2.c). */
void     vdp2_render(saturn *s, uint32_t *out, int w, int h, int force_on);
void     vdp2_display_size(saturn *s, int *w, int *h);

/* SMPC (runner/src/smpc.c). */
void     smpc_command(saturn *s, uint8_t cmd);
void     smpc_tick(saturn *s);
void     smpc_ireg0_write(saturn *s, uint8_t v);
void     smpc_reset(saturn *s);
uint8_t  smpc_pdr_read(saturn *s, int port);

void     saturn_init(saturn *s);

/* Raise an SCU interrupt source (bit number per the SCU interrupt table). */
void     scu_raise(saturn *s, int bit);
void     scu_timer_hblank(saturn *s);
void     scu_timer_vblank_out(saturn *s);

/* Highest-priority deliverable interrupt for this CPU, or -1.
 * Fills *vector and *level when one is deliverable. */
int      scu_pending(saturn *s, uint32_t sr, int *vector, int *level);
void     saturn_report_trace(saturn *s, FILE *out);
void     cdb_report(saturn *s, FILE *out);

/* CD block (runner/src/cdblock.c). */
void     cdb_init(saturn *s, void *disc_handle, void *iso_handle);
int      png_write(const char *path, const uint32_t *argb, int w, int h);

/* Map an IP.BIN area string ("J", "JTU", "U", "E"...) to an SMPC area code. */
uint8_t  saturn_area_from_ip(const char *area);
void     vdp2_cell_debug(saturn *s, int layer, int x, int y);
void     cdb_tick(saturn *s);
void     cdb_periodic_maybe(saturn *s);
void     cdb_execute(saturn *s);
uint16_t cdb_read_dtr(saturn *s);
/* Re-evaluate (HIRQ & HMSK) and assert the CD block's interrupt line if any
 * unmasked flag is set. Ymir calls this on every HIRQ set, every host HIRQ
 * clear and every HIRQMASK write (cdblock.cpp UpdateInterrupts). */
void     cdb_update_interrupts(saturn *s);

/* --------------------------------------------------------------- SH-2 */

void     sh2_reset(sh2 *c, saturn *s, int is_slave, uint32_t pc, uint32_t sp);

/* Execute one instruction (plus its delay slot, if it has one).
 * Returns the number of instructions retired (1 or 2), or 0 if halted. */
int      sh2_step(sh2 *c);

/* Run up to `n` instructions or until halt. Returns instructions retired. */
uint64_t sh2_run(sh2 *c, uint64_t n);
void sh2_report_ophist(FILE *f);

/* ------------------------------------------------ machine clock / scheduler
 *
 * NTSC 320x224: 28.6364 MHz / 15.734 kHz = 1820 cycles per scanline, 263 lines
 * per field. The last ~20% of each line is the horizontal blank. These are the
 * numbers TVSTAT and the FRT input capture were already using ad hoc; they are
 * named here so the scheduler, TVSTAT and the FRT cannot drift apart. */
#define CYC_PER_LINE  1820u
#define HBLANK_START  1456u
#define LINES_TOTAL    263u
#define LINE_VBLANK    224u

/* Free-running timer, per core. The Saturn wires each SH-2's input-capture pin
 * (FTI) to the horizontal blank, which is how a game paces the slave against
 * the raster. */
void     frt_advance(sh2 *c, uint32_t cycles);
void     frt_capture(sh2 *c);
int      frt_pending(sh2 *c, uint32_t sr, int *vector, int *level);
int      dmac_pending(sh2 *c, uint32_t sr, int *vector, int *level);
void     frt_write8(sh2 *c, uint32_t off, uint8_t v);
void     frt_irq_init(void);
int      frt_irq_on(void);
extern int g_frt_irq;
const uint8_t *bus_page(saturn *s, uint32_t a);

/* Run one scanline of machine time: both cores advance by CYC_PER_LINE,
 * interleaved in sub-line quanta, with the H-Blank edge landing at the same
 * point in both. Returns master instructions retired. */
uint64_t saturn_run_line(saturn *s);

/* Run one field: LINES_TOTAL scanlines with V-Blank in/out at the right lines.
 * Returns master instructions retired. */
uint64_t saturn_run_field(saturn *s);

/* ---- sound ---------------------------------------------------------- */
uint32_t scsp_cdda_push(saturn *s, const uint8_t *sector2352);
uint16_t scsp_read  (saturn *s, uint32_t off);
void     scsp_write (saturn *s, uint32_t off, uint16_t v);
void     scsp_render(saturn *s, int16_t *left, int16_t *right);
void     scsp_reset (saturn *s);
void     sound_init (saturn *s);
void     sound_run  (saturn *s, uint32_t sh2_cycles);
void     sound_sync (saturn *s);
void     sound_set_on(saturn *s, int on);
void     sound_clock_change_reset(saturn *s);
uint32_t sound_drain(saturn *s, int16_t *out, uint32_t frames);

#endif /* SATURN_H */
