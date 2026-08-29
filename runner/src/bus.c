/* bus.c — Saturn address decode and memory access.
 *
 * Only the decode and the RAM regions are real here. Peripheral registers are
 * backed by storage and logged, so boot code observes stable values and we get
 * a record of exactly which registers a title touches, in order. That record
 * is what drives which peripheral gets implemented next -- rather than
 * implementing all of VDP2 speculatively.
 */
#include "saturn.h"
#include <x86intrin.h>
#include "../../external/sh2-recomp-core/common/sh2_isa.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------- tracing */

static void trace(saturn *s, uint32_t a, int is_write, int size)
{
    if (!s->trace_enabled) return;
    for (int i = 0; i < s->ntrace; i++) {
        if (s->trace[i].addr == a && s->trace[i].is_write == (uint8_t)is_write) {
            s->trace[i].count++;
            return;
        }
    }
    if (s->ntrace < TRACE_SLOTS) {
        s->trace[s->ntrace].addr     = a;
        s->trace[s->ntrace].is_write = (uint8_t)is_write;
        s->trace[s->ntrace].size     = (uint8_t)size;
        s->trace[s->ntrace].count    = 1;
        s->ntrace++;
    }
}

/* ------------------------------------------------------ region helpers */

/* Returns a pointer to `size` bytes of backing storage for bus address `a`,
 * or NULL when the address is not plain RAM. */
/* The core that is actually executing. The write watch reported the MASTER
 * unconditionally, so every write the SLAVE made was attributed to whatever
 * PC the master happened to be sitting at -- which sends you looking for a
 * routine that never ran. */
#define WW_CORE(s) ((s)->cur ? (s)->cur : &(s)->master)

static uint8_t *ram_ptr(saturn *s, uint32_t a, uint32_t size)
{
    /* WRAM-H first: it carries the overwhelming majority of traffic (code,
     * stacks, work RAM), so the hottest region must not sit behind two other
     * range tests. */
    if (a >= 0x06000000u) {                                  /* WRAM-H, x32  */
        uint32_t o = (a - 0x06000000u) & (WRAM_H_SIZE - 1);
        return (o + size <= WRAM_H_SIZE) ? &s->wram_h[o] : NULL;
    }
    if (a < 0x00100000u) {                                   /* boot ROM     */
        /* The 512KB IPL is mapped across a 1MB window, so the upper half is a
         * MIRROR of the lower (Ymir memory.cpp maps IPL over 0x000'0000-
         * 0x00F'FFFF). Returning nothing above 512KB handed out zeros for
         * every mirrored read. */
        uint32_t o = a & (BIOS_SIZE - 1u);
        return (o + size <= BIOS_SIZE) ? &s->bios[o] : NULL;
    }

    if (a >= 0x00200000u && a < 0x00300000u) {               /* WRAM-L       */
        uint32_t o = (a - 0x00200000u) & (WRAM_L_SIZE - 1u);
        return (o + size <= WRAM_L_SIZE) ? &s->wram_l[o] : NULL;
    }
    if (a >= 0x00300000u && a < 0x00400000u)                 /* unpopulated  */
        return NULL;          /* reads answer 0xFF, see bus_r8 */
    if (a >= 0x00180000u && a < 0x00200000u) {               /* backup RAM   */
        uint32_t o = (a - 0x00180000u) & (BUP_SIZE - 1);
        return (o + size <= BUP_SIZE) ? &s->bup[o] : NULL;
    }
    if (a >= 0x05A00000u && a < 0x05B00000u) {               /* sound RAM    */
        uint32_t o = (a - 0x05A00000u) & (SOUND_RAM_SZ - 1);
        return (o + size <= SOUND_RAM_SZ) ? &s->sound_ram[o] : NULL;
    }
    if (a >= 0x05C00000u && a < 0x05C80000u) {               /* VDP1 VRAM    */
        uint32_t o = a - 0x05C00000u;
        return (o + size <= VDP1_VRAM_SZ) ? &s->vdp1_vram[o] : NULL;
    }
    if (a >= 0x05C80000u && a < 0x05D00000u) {               /* VDP1 FB      */
        uint32_t o = (a - 0x05C80000u) & (VDP1_FB_SZ - 1);
        return (o + size <= VDP1_FB_SZ) ? &s->vdp1_fb[s->fb_draw][o] : NULL;
    }
    if (a >= 0x05E00000u && a < 0x05F00000u) {               /* VDP2 VRAM    */
        uint32_t o = (a - 0x05E00000u) & (VDP2_VRAM_SZ - 1);
        return (o + size <= VDP2_VRAM_SZ) ? &s->vdp2_vram[o] : NULL;
    }
    if (a >= 0x05F00000u && a < 0x05F80000u) {               /* VDP2 CRAM    */
        uint32_t o = (a - 0x05F00000u) & (CRAM_SIZE - 1);
        return (o + size <= CRAM_SIZE) ? &s->cram[o] : NULL;
    }
    return NULL;
}

/* --------------------------------------------------------- CD/CS2 decode --
 * Ymir CDBlock::MapMemory and the Saturn address map agree on the physical
 * decode: the CD register file is present in the first 4 KiB of every 32 KiB
 * segment throughout A-Bus CS2 (0x05800000-0x058FFFFF). The 64-byte register
 * file then mirrors throughout each of those blocks.
 *
 * Keep this as the ONE predicate used by reads, writes, DMA (which uses the
 * public bus accessors), unmapped accounting, and trace-region reporting. */
static int cdb_reg_decode(uint32_t a, uint32_t *off)
{
    uint32_t rel;
    a &= 0x07FFFFFFu;
    if (a < 0x05800000u || a >= 0x05900000u) return 0;
    rel = a - 0x05800000u;
    if ((rel & 0x7FFFu) >= 0x1000u) return 0;
    if (off) *off = rel & 0x3Fu;
    return 1;
}

static void cdb_note_read(saturn *s, uint32_t a, uint32_t off, int size)
{
    if (s->rwatch_addr && a == (s->rwatch_addr & 0x07FFFFFFu)) {
        uint32_t pc = WW_CORE(s)->pr;   /* caller, not the reader */
        int seen = 0;
        for (int k = 0; k < s->nrwatch; k++) {
            if (s->rwatch[k].pc == pc) {
                s->rwatch[k].count++;
                seen = 1;
                break;
            }
        }
        if (!seen && s->nrwatch < 16) {
            s->rwatch[s->nrwatch].pc = pc;
            s->rwatch[s->nrwatch].count = 1;
            s->nrwatch++;
        }
    }

    cdb_periodic_maybe(s);
    trace(s, a, 0, size);
    s->cd_read_pc = WW_CORE(s)->pc;
    s->cd_read_addr = a;

    if (off == 0x18u && s->cd.boot_delay == 0) {
        uint64_t snap = ((uint64_t)s->cdb_reg[0x18 >> 1] << 48) |
                        ((uint64_t)s->cdb_reg[0x1C >> 1] << 32) |
                        ((uint64_t)s->cdb_reg[0x20 >> 1] << 16) |
                        ((uint64_t)s->cdb_reg[0x24 >> 1]);
        if (s->cr_snap_valid && snap != s->cr_snap) s->cr_changes++;
        s->cr_snap = snap;
        s->cr_snap_valid = 1;
    }
    if (off == 0x18u) {
        int k = (int)(s->cr1reads % 12);
        s->cr2log[k].val = s->cdb_reg[0x18 >> 1];
        s->cr2log[k].pc  = WW_CORE(s)->pc;
        s->cr1reads++;
        if (s->ncr2log < 12) s->ncr2log++;
    }
}

static uint16_t cdb_reg_read(saturn *s, uint32_t a, uint32_t off, int size)
{
    uint16_t value = 0;
    cdb_note_read(s, a, off, size);

    switch (off) {
    case 0x00u:
    case 0x02u:
        return cdb_read_dtr(s);
    case 0x08u:
    case 0x0Cu:
    case 0x18u:
    case 0x1Cu:
    case 0x20u:
    case 0x24u:
        value = s->cdb_reg[off >> 1];
        break;
    default:
        break;
    }

    /* Reading CR4 completes pickup of the command response. */
    if (off == 0x24u) {
        s->cd.resp_pending = 0;
        s->cd.processing_cmd = 0;
    }
    return value;
}

static void cdb_reg_write(saturn *s, uint32_t a, uint32_t off,
                          uint16_t value, int size)
{
    trace(s, a, 1, size);
    {   /* ring: keep the LAST 24 CD writes, the early ones are just init */
        int k = (int)(s->cdwrites % 24);
        s->cdwlog[k].addr = a;
        s->cdwlog[k].val = value;
        s->cdwlog[k].pc = WW_CORE(s)->pc;
        s->cdwrites++;
        if (s->ncdwlog < 24) s->ncdwlog++;
    }

    switch (off) {
    case 0x00u:
    case 0x02u:
        cdb_write_dtr(s, value);
        return;
    case 0x08u:                         /* HIRQ: write-to-clear */
        if (getenv("SATURN_CDSEQ"))
            printf("[hirq ack] keep=%04X had=%04X pc=%08X\n",
                   value, s->cdb_reg[0x08 >> 1], WW_CORE(s)->pc);
        s->cdb_reg[0x08 >> 1] &= value;
        cdb_update_interrupts(s);
        return;
    case 0x0Cu:                         /* HIRQMASK */
        s->cdb_reg[0x0C >> 1] = value;
        if (getenv("SATURN_CDIRQ"))
            printf("[hmsk] = %04X hirq=%04X pc=%08X cy=%llu\n",
                   value, s->cdb_reg[0x08 >> 1], WW_CORE(s)->pc,
                   (unsigned long long)s->master.cycles);
        cdb_update_interrupts(s);
        return;
    case 0x18u:
        s->cd.cmd_stage[0] = value;
        s->cd.processing_cmd = 1;
        return;
    case 0x1Cu:
        s->cd.cmd_stage[1] = value;
        return;
    case 0x20u:
        s->cd.cmd_stage[2] = value;
        return;
    case 0x24u:
        s->cd.cmd_stage[3] = value;
        cdb_execute(s);
        return;
    default:
        return;
    }
}

static const char *trace_region_name(uint32_t a)
{
    uint32_t off;
    a &= 0x07FFFFFFu;
    if (cdb_reg_decode(a, &off))                         return "CD block";
    if (a >= 0x00100000u && a < 0x00180000u)           return "SMPC";
    if (a >= 0x01000000u && a < 0x01800000u)           return "MINIT";
    if (a >= 0x01800000u && a < 0x02000000u)           return "SINIT";
    if (a >= 0x02000000u && a < 0x05000000u)           return "A-Bus/cart";
    if (a >= 0x05B00000u && a < 0x05C00000u)           return "SCSP";
    if (a >= 0x05D00000u && a < 0x05D80000u)           return "VDP1 regs";
    if (a >= 0x05F80000u && a < 0x05FC0000u)           return "VDP2 regs";
    if (a >= 0x05FE0000u && a < 0x05FF0000u)           return "SCU";
    return "unmapped";
}


/* ------------------------------------------- SH-2 on-chip peripherals ----
 * Bits 31-29 == 111 select the on-chip register file at 0xFFFFFE00-0xFFFFFFFF.
 * This region must be decoded BEFORE the 27-bit bus mask is applied, or the
 * whole peripheral block aliases into nowhere.
 *
 * The DMAC is the part that matters for booting: Saturn titles move nearly
 * everything -- VRAM uploads included -- with the SH-2's own two DMA channels,
 * then spin on CHCR waiting for DE=1,TE=0 to clear.
 *
 * SH7604 DMAC register file:
 *   SAR0 FF80  DAR0 FF84  TCR0 FF88  CHCR0 FF8C
 *   SAR1 FF90  DAR1 FF94  TCR1 FF98  CHCR1 FF9C
 *   VCRDMA0 FFA0  VCRDMA1 FFA8  DMAOR FFB0
 *
 * CHCR bit 0 = DE (enable), bit 1 = TE (transfer end). [PIN] The transfer-size
 * and address-mode field positions below follow the common emulator reading
 * (TS = bits 11:10, SM = 13:12, DM = 15:14) and still need confirming against
 * the SH7604 hardware manual before the cycle model depends on them.
 */
#define OC(off)  (((off) - 0xFE00u) & 0x1FFu)

/* ------------------------------------------------ hardware division unit --
 *
 * SH7604 DIVU, one per core, registers at on-chip 0xFF00-0xFF1F (mirrored at
 * 0xFF20-0xFF3F): DVSR, DVDNT, DVCR, VCRDIV, DVDNTH, DVDNTL and the
 * undocumented result mirrors. Writing DVDNT starts a signed 32/32 divide;
 * writing DVDNTL starts a signed 64/32 with DVDNTH:DVDNTL as the dividend.
 * Quotient lands in DVDNT/DVDNTL, remainder in DVDNTH.
 *
 * This runner stored these as plain bytes, so every BIOS division came back
 * as the raw dividend. The boot logo's particle pipeline divides positions
 * through here constantly; with the quotients never computed, magnitudes
 * compounded until the range classifier's braf jumped mid-loop and the BIOS
 * wedged at 0x06012C88 with the display off.
 *
 * Semantics per Ymir sh2_divu.hpp (verified against the SH7604 manual):
 * division by zero and quotient overflow set DVCR.OVF and leave the partial
 * shift-subtract result unless OVFIE is clear, in which case DVDNTL/DVDNT
 * saturate by sign. Division timing (39 cycles) is not modelled. */
static uint32_t oc_get32(sh2 *cc, uint32_t off)
{
    uint32_t o = OC(off);
    return ((uint32_t)cc->onchip[o] << 24) | ((uint32_t)cc->onchip[o+1] << 16) |
           ((uint32_t)cc->onchip[o+2] << 8) | cc->onchip[o+3];
}
static uint16_t oc_get16(sh2 *cc, uint32_t off)
{
    uint32_t o = OC(off);
    return (uint16_t)(((uint16_t)cc->onchip[o] << 8) | cc->onchip[o + 1]);
}
static void oc_put16(sh2 *cc, uint32_t off, uint16_t v)
{
    uint32_t o = OC(off);
    cc->onchip[o] = (uint8_t)(v >> 8);
    cc->onchip[o + 1] = (uint8_t)v;
}
static void oc_put32(sh2 *cc, uint32_t off, uint32_t v)
{
    uint32_t o = OC(off);
    cc->onchip[o]   = (uint8_t)(v >> 24); cc->onchip[o+1] = (uint8_t)(v >> 16);
    cc->onchip[o+2] = (uint8_t)(v >> 8);  cc->onchip[o+3] = (uint8_t)v;
}

static void divu_calc32(sh2 *cc)
{
    int32_t dividend = (int32_t)oc_get32(cc, 0xFF14u);   /* DVDNTL */
    int32_t divisor  = (int32_t)oc_get32(cc, 0xFF00u);   /* DVSR   */
    uint32_t dvcr    = oc_get32(cc, 0xFF08u);
    uint32_t q, r;

    if (divisor != 0) {
        if (dividend == (int32_t)0x80000000 && divisor == -1) {
            q = 0x80000000u; r = 0;
        } else {
            q = (uint32_t)(dividend / divisor);
            r = (uint32_t)(dividend % divisor);
        }
    } else {
        /* Division by zero: 3 cycles of partial shift-subtract reach the
         * result registers; without the overflow interrupt the quotient is
         * saturated by sign instead. */
        r = (uint32_t)(dividend >> 29);
        if (dvcr & 2u)
            q = ((uint32_t)dividend << 3) | (((uint32_t)~dividend >> 31) & 7u);
        else
            q = (dividend < 0) ? 0x80000000u : 0x7FFFFFFFu;
        oc_put32(cc, 0xFF08u, dvcr | 1u);                /* DVCR.OVF */
    }
    oc_put32(cc, 0xFF04u, q);                            /* DVDNT   */
    oc_put32(cc, 0xFF14u, q);                            /* DVDNTL  */
    oc_put32(cc, 0xFF10u, r);                            /* DVDNTH  */
    oc_put32(cc, 0xFF1Cu, q);                            /* DVDNTUL */
    oc_put32(cc, 0xFF18u, r);                            /* DVDNTUH */
}

static void divu_calc64(sh2 *cc)
{
    int64_t dividend = ((int64_t)(int32_t)oc_get32(cc, 0xFF10u) << 32) |
                        (int64_t)(uint32_t)oc_get32(cc, 0xFF14u);
    int32_t divisor  = (int32_t)oc_get32(cc, 0xFF00u);
    uint32_t dvcr    = oc_get32(cc, 0xFF08u);
    int overflow = 0;
    uint32_t q = 0, r = 0;

    if (divisor == 0) {
        overflow = 1;
    } else if (dividend == -0x80000000LL && (divisor == 1 || divisor == -1)) {
        q = 0x80000000u; r = 0;
    } else if (dividend == INT64_MIN && divisor == -1) {
        overflow = 1;
    } else {
        int64_t quot = dividend / divisor;
        int32_t rem  = (int32_t)(dividend % divisor);
        if (((quot == (int64_t)INT32_MIN && divisor > 0) ||
             (quot == 0x80000000LL && divisor < 0)) && rem == 0) {
            q = (uint32_t)quot; r = (uint32_t)rem;
        } else if (quot < (int64_t)INT32_MIN || quot > (int64_t)INT32_MAX) {
            overflow = 1;
        } else {
            q = (uint32_t)quot; r = (uint32_t)rem;
        }
    }

    if (overflow) {
        /* Three steps of the hardware's shift-subtract loop, per Ymir. */
        int64_t orig = dividend;
        int Q = dividend < 0;
        const int M = divisor < 0;
        for (int i = 0; i < 3; i++) {
            if (Q == M) dividend -= (int64_t)((uint64_t)(uint32_t)divisor << 32);
            else        dividend += (int64_t)((uint64_t)(uint32_t)divisor << 32);
            Q = dividend < 0;
            dividend = (dividend << 1) | (Q == M ? 1 : 0);
        }
        if (dvcr & 2u)
            q = (uint32_t)dividend;
        else
            q = ((int32_t)((uint64_t)orig >> 32) ^ divisor) < 0 ? 0x80000000u
                                                                : 0x7FFFFFFFu;
        r = (uint32_t)((uint64_t)dividend >> 32);
        oc_put32(cc, 0xFF08u, dvcr | 1u);                /* DVCR.OVF */
    }
    oc_put32(cc, 0xFF04u, q);
    oc_put32(cc, 0xFF14u, q);
    oc_put32(cc, 0xFF10u, r);
    oc_put32(cc, 0xFF1Cu, q);
    oc_put32(cc, 0xFF18u, r);
}

/* Mirror fold: 0xFF20-0xFF3F shadows 0xFF00-0xFF1F. */
static uint32_t divu_fold(uint32_t off)
{
    return (off >= 0xFF20u && off < 0xFF40u) ? off - 0x20u : off;
}

/* The on-chip file belongs to whichever core is executing. `cur` is parked on
 * the master for accesses made outside CPU execution (image loading, dumps). */
static uint8_t *oc_bank(saturn *s)
{
    return (s->cur ? s->cur : &s->master)->onchip;
}

static uint32_t oc_r32(saturn *s, uint32_t off)
{
    uint8_t *f = oc_bank(s);
    sh2 *cc = s->cur ? s->cur : &s->master;
    uint32_t o;
    off = divu_fold(off);
    /* The BSC registers are 16 bits wide even for a 32-bit access; their value
     * is returned in the low half of the longword. */
    if (off >= 0xFFE0u && off <= 0xFFF8u && !(off & 3u))
        return oc_get16(cc, off);
    o = OC(off);
    uint32_t v = ((uint32_t)f[o] << 24) | ((uint32_t)f[o+1] << 16) |
                 ((uint32_t)f[o+2] << 8) | f[o+3];
    if (off == 0xFE10u) {                       /* TIER:FTCSR:FRC(hi:lo) */
        sh2 *cc = s->cur ? s->cur : &s->master;
        v = (v & 0xFFFF0000u) | cc->frc;
    }
    return v;
}
static void oc_w32(saturn *s, uint32_t off, uint32_t v)
{
    sh2 *cc = s->cur ? s->cur : &s->master;
    off = divu_fold(off);
    /* Bus-state-controller writes are key-protected. The 0xA55A key occupies
     * the upper half and is not retained; BCR1.MASTER is read-only. */
    if (off >= 0xFFE0u && off <= 0xFFF8u && !(off & 3u)) {
        if ((v >> 16) != 0xA55Au) return;
        switch (off) {
        case 0xFFE0u: {
            uint16_t master = (uint16_t)(oc_get16(cc, off) & 0x8000u);
            oc_put16(cc, off, (uint16_t)(master | (v & 0x1FF7u)));
            break;
        }
        case 0xFFE4u: oc_put16(cc, off, (uint16_t)(v & 0x00FCu)); break;
        case 0xFFE8u: oc_put16(cc, off, (uint16_t)v); break;
        case 0xFFECu: oc_put16(cc, off, (uint16_t)(v & 0xFEFCu)); break;
        default:      oc_put16(cc, off, (uint16_t)v); break;
        }
        return;
    }
    if (off >= 0xFF00u && off < 0xFF20u) {
        oc_put32(cc, off, v);
        if (off == 0xFF04u) { oc_put32(cc, 0xFF14u, v); divu_calc32(cc); }
        if (off == 0xFF14u) divu_calc64(cc);
        return;
    }
    frt_write8(cc, off,      (uint8_t)(v >> 24));
    frt_write8(cc, off + 1u, (uint8_t)(v >> 16));
    frt_write8(cc, off + 2u, (uint8_t)(v >> 8));
    frt_write8(cc, off + 3u, (uint8_t)v);
}

/* Run one DMAC channel to completion. Real hardware interleaves this with CPU
 * execution; we complete it instantly, which is correct in result and wrong in
 * timing. Timing gets refined once the scheduler exists. */
uint64_t g_dma_dst_bytes[128];
uint32_t g_dma_dst_count[128];

static void dmac_run(saturn *s, int ch)
{
    uint32_t base = ch ? 0xFF90u : 0xFF80u;
    uint32_t chcr = oc_r32(s, base + 0x0C);
    uint32_t sar  = oc_r32(s, base + 0x00);
    uint32_t dar  = oc_r32(s, base + 0x04);
    uint32_t tcr  = oc_r32(s, base + 0x08) & 0x00FFFFFFu;
    uint32_t dmaor = oc_r32(s, 0xFFB0);
    unsigned ts = (chcr >> 10) & 3u;
    unsigned sm = (chcr >> 12) & 3u;
    unsigned dm = (chcr >> 14) & 3u;
    uint32_t unit = ts == 0 ? 1u : ts == 1 ? 2u : ts == 2 ? 4u : 16u;
    uint32_t count = tcr ? tcr : 0x1000000u;
    /* In 16-byte transfer mode TCR still counts LONGWORDS -- the DMAC retires
     * one 16-byte burst per FOUR counts -- so the byte length is tcr*4, the
     * same as longword mode, not tcr*16. Fighting Vipers programs
     * SAR=00238000 DAR=060A6000 TCR=00008000 CHCR=00005E01 (TS=3) from
     * 0x060586A0: the correct transfer ends at 060C6000, but counting TCR as
     * 16-byte units ran 4x long to 06126000 -- past the top of WRAM-H and
     * straight over the SGL work area, writing 0000FFFC into the slave command
     * ring tail at 060FFC48 and wedging the slave. */
    /* Ymir sh2.cpp: in QuadLongword mode it subtracts 4 per burst but clamps to
     * zero, so a leftover count of 1..3 still costs one final burst -- that is
     * ceil(count/4), not count/4. */
    uint32_t iters = (unit == 16u) ? ((count + 3u) >> 2) : count;

    if (!(chcr & 1u)) return;              /* DE clear */
    if (!(dmaor & 1u)) return;             /* DME clear: master disable */
    if (chcr & 2u) return;                 /* TE already set */

    uint32_t sar_orig = sar, dar_orig = dar;
    if (s->nocdmalog < 16) {
        s->ocdmalog[s->nocdmalog].src  = sar;
        s->ocdmalog[s->nocdmalog].dst  = dar;
        s->ocdmalog[s->nocdmalog].cnt  = iters * unit;
        s->ocdmalog[s->nocdmalog].chcr = chcr;
        s->nocdmalog++;
    }

    for (uint32_t i = 0; i < iters; i++) {
        switch (unit) {
        case 1:  bus_w8 (s, dar, bus_r8 (s, sar)); break;
        case 2:  bus_w16(s, dar, bus_r16(s, sar)); break;
        case 4:  bus_w32(s, dar, bus_r32(s, sar)); break;
        default:
            for (int k = 0; k < 16; k += 4)
                bus_w32(s, dar + (uint32_t)k, bus_r32(s, sar + (uint32_t)k));
            break;
        }
        if (sm == 1) sar += unit; else if (sm == 2) sar -= unit;
        if (dm == 1) dar += unit; else if (dm == 2) dar -= unit;
    }

    s->dma_transfers++;
    s->dma_bytes += (uint64_t)count * unit;
    /* Destination histogram, bucketed by 1MB of the 27-bit bus address. The
     * first-16 log above cannot show where the BULK of a million DMA'd bytes
     * goes -- and "which region does the movie decoder write into" is exactly
     * the question that matters. */
    {
        extern uint64_t g_dma_dst_bytes[128];
        extern uint32_t g_dma_dst_count[128];
        /* dar has been advanced by the loop above; bucketing on it credits a
         * long transfer to whatever megabyte it ENDED in. Use the start. */
        unsigned bucket = (dar_orig & 0x07FFFFFFu) >> 20;
        g_dma_dst_bytes[bucket & 127u] += (uint64_t)count * unit;
        g_dma_dst_count[bucket & 127u]++;
        /* SATURN_DMAPEEK=<1MB bucket>: sample the SOURCE bytes of transfers
         * aimed at that region. "666 KB reached VDP2 VRAM but the VRAM is
         * still blank" has two very different explanations -- the producer is
         * emitting zeros, or it is emitting real data that lands elsewhere --
         * and only looking at what is being copied tells them apart. */
        {
            static int peek = -2;
            static int shown;
            if (peek == -2) {
                const char *e = getenv("SATURN_DMAPEEK");
                peek = e ? (int)strtoul(e, NULL, 16) : -1;
            }
            static unsigned long long peek_after = 0;
            static int peek_after_init;
            if (!peek_after_init) {
                const char *e2 = getenv("SATURN_DMAPEEKAFTER");
                peek_after = e2 ? strtoull(e2, NULL, 0) : 0;
                peek_after_init = 1;
            }
            if (peek >= 0 && (int)bucket == peek && shown < 12 &&
                s->master.cycles >= peek_after) {
                uint32_t nz = 0, k;
                uint32_t total = count * unit;
                for (k = 0; k < total && k < 4096u; k++)
                    if (bus_r8(s, sar_orig + k)) nz++;
                shown++;
                fprintf(stderr, "[dmapeek] cy=%llu src=%08X dst=%08X %u bytes, "
                        "first8=%02X %02X %02X %02X %02X %02X %02X %02X, "
                        "nonzero %u/%u sampled | chcr=%08X sm=%u dm=%u | DST after: %02X %02X %02X %02X\n",
                        (unsigned long long)s->master.cycles, sar_orig, dar_orig, total,
                        bus_r8(s, sar_orig+0), bus_r8(s, sar_orig+1),
                        bus_r8(s, sar_orig+2), bus_r8(s, sar_orig+3),
                        bus_r8(s, sar_orig+4), bus_r8(s, sar_orig+5),
                        bus_r8(s, sar_orig+6), bus_r8(s, sar_orig+7),
                        nz, total < 4096u ? total : 4096u,
                        chcr, sm, dm,
                        bus_r8(s, dar_orig+0), bus_r8(s, dar_orig+1),
                        bus_r8(s, dar_orig+2), bus_r8(s, dar_orig+3));
            }
        }
    }

    oc_w32(s, base + 0x00, sar);
    oc_w32(s, base + 0x04, dar);
    oc_w32(s, base + 0x08, 0);
    /* Signal completion the way the polling loop expects: TE set, DE clear. */
    oc_w32(s, base + 0x0C, (chcr & ~1u) | 2u);
}

static void scu_dma_run(saturn *s, int level);

/* Start every armed channel whose trigger factor matches `factor`.
 * Mirrors Ymir's SCU::TriggerDMATransfer. Factors: 0 V-Blank IN,
 * 1 V-Blank OUT, 2 H-Blank IN, 3 Timer 0, 4 Timer 1, 5 Sound Request,
 * 6 Sprite Draw End, 7 immediate (started by the enable write instead). */
void scu_dma_trigger(saturn *s, unsigned factor)
{
    int lvl;
    if (factor >= 7u) return;
    for (lvl = 0; lvl < 3; lvl++) {
        uint32_t en = s->scu_reg[(lvl * 0x20u + 0x10u) >> 2];
        uint32_t md = s->scu_reg[(lvl * 0x20u + 0x14u) >> 2];
        if (!(en & 0x100u)) continue;               /* not armed */
        if ((md & 7u) != factor) continue;          /* different event */
        scu_dma_run(s, lvl);
    }
}



/* Range write-watch: log the first N writes landing anywhere in a window, with
 * PC and value. Single-address watches cannot show a state machine; this can. */
/* Does this access touch the watched byte? */
/* WRAM-H answers to EVERY 1 MB mirror from 0x06000000 to 0x07FFFFFF, so
 * 0x0610F3A8 and 0x0600F3A8 are the same byte -- and NiGHTS really does store
 * through the high alias. Watches that compared the literal address therefore
 * saw NOTHING while the memory underneath them changed, which made a genuine
 * write look like spontaneous corruption. Fold every address to its canonical
 * form before any watch compares it. */
static uint32_t canon_addr(uint32_t b)
{
    if (b >= 0x06000000u)
        return 0x06000000u | ((b - 0x06000000u) & (WRAM_H_SIZE - 1u));
    return b;
}

static int wwatch_hit(saturn *s, uint32_t b, int sz)
{
    uint32_t w = canon_addr(s->wwatch_addr & 0x07FFFFFFu);
    b = canon_addr(b);
    return b <= w && w < b + (uint32_t)sz;
}

static void wrange_note(saturn *s, uint32_t b, uint32_t v, int sz)
{
    /* Match on the bytes the access COVERS. Matching only its base address
     * made wide writes invisible: a 32-bit store at 0x06005F9C zeroes
     * 0x06005F9E without ever being reported by a watch on 0x06005F9E. Three
     * separate "written non-zero, reads back zero" mysteries were this. */
    if (!s->wrange_hi) return;
    b = canon_addr(b);                 /* see canon_addr: WRAM-H is mirrored */
    if (b + (uint32_t)sz - 1u < canon_addr(s->wrange_lo) ||
        b > canon_addr(s->wrange_hi)) return;
    /* Print LIVE. The collected log below is only emitted from a diagnostics
     * block that needs another env var, so for a long time SATURN_WRANGE
     * reported nothing at all and "no writes were logged" read as "nothing
     * wrote it" -- which sent a whole investigation down the wrong path.
     * A watch that can silently observe nothing is worse than no watch. */
    {
        static int live = -1;
        static unsigned long long n, cap;
        if (live < 0) {
            const char *e = getenv("SATURN_WRMAX");
            live = getenv("SATURN_WRQUIET") ? 0 : 1;
            cap  = e ? strtoull(e, NULL, 0) : 200ull;
        }
        if (live && ++n <= cap)
            /* clk, not the core's own cycle counter: see the note in
             * sh2_interp.c's [jsr] hook -- per-core counters are reset
             * independently and cannot be ordered against each other. */
            printf("[wr] %08X = %0*X (sz%d) pc=%08X pr=%08X clk=%llu %s\n",
                   b, sz * 2, v, sz, WW_CORE(s)->pc, WW_CORE(s)->pr,
                   (unsigned long long)s->clk,
                   WW_CORE(s) == &s->slave ? "slave" : "master");
    }
    if (s->nwrlog >= 64) return;
    s->wrlog[s->nwrlog].addr = b;
    s->wrlog[s->nwrlog].val  = v;
    s->wrlog[s->nwrlog].pc   = s->master.pc;
    s->wrlog[s->nwrlog].cy   = s->master.cycles;
    s->wrlog[s->nwrlog].sz   = sz;
    s->nwrlog++;
}


/* Range read-watch: which PCs READ a window, and how often. Finding the code
 * that consumes a buffer is the mirror of finding the code that fills it. */
static void rrange_note(saturn *s, uint32_t b)
{
    if (!s->rrange_hi || b < s->rrange_lo || b > s->rrange_hi) return;
    uint32_t pc = s->master.pc;
    for (int k = 0; k < s->nrrlog; k++)
        if (s->rrlog[k].pc == pc) { s->rrlog[k].n++; return; }
    if (s->nrrlog < 16) { s->rrlog[s->nrrlog].pc = pc; s->rrlog[s->nrrlog].n = 1; s->nrrlog++; }
}

static int is_onchip(uint32_t a) { return (a >> 29) == 7u; }

/* Partition 010 (0x40000000-0x5FFFFFFF) is the SH-2 associative cache-purge
 * area. Accesses operate on cache tags and must never reach the external bus.
 * The slave BIOS writes zero through 0x46000240 while polling the physical
 * mailbox at 0x06000240; folding both addresses together erased the master's
 * "2RDY" token on every iteration and trapped the slave in its reset loop. */
static int is_cache_purge(uint32_t a) { return ((a >> 29) & 7u) == 2u; }

/* Partition 011 is the cache address array; partitions 100/110 are the data
 * array.  They are distinct hardware views and cannot be folded onto WRAM. */
static int is_cache_addr(uint32_t a) { return ((a >> 29) & 7u) == 3u; }

/* SH-2 cache data array: address bits 31-29 == 0b110 (0xC0000000), plus the
 * undocumented 0b100 mirror at 0x80000000 (Ymir sh2.cpp dispatches both to
 * Cache::Read/WriteDataArray). It belongs to the CORE, not the machine.
 *
 * Ymir splits the address into index = bits 9-4, way = bits 11-10 and byte =
 * bits 3-0. That is a PERMUTATION of the same 4KB, and for the way this is
 * actually used -- as scratch RAM, written and read back through the same
 * addresses -- any bijection behaves identically, so the low 12 bits index the
 * array directly. It would only diverge for code that correlates cache
 * contents with the cached memory behind them, which needs a real cache model. */
static int is_cachearr(uint32_t a) { return ((a >> 29) & 5u) == 4u; }

static uint8_t *cache_ptr(saturn *s, uint32_t a)
{
    sh2 *c = s->cur ? s->cur : &s->master;
    return &c->cache_data[a & 0xFFFu];
}

static sh2 *cache_core(saturn *s) { return s->cur ? s->cur : &s->master; }

static void cache_purge_addr(saturn *s, uint32_t a)
{
    sh2 *c = cache_core(s);
    unsigned set = (a >> 4) & 63u;
    uint32_t tag = (a >> 10) & 0x7FFFFu;
    for (unsigned way = 0; way < 4; way++)
        if (c->cache_valid[set][way] && c->cache_tag[set][way] == tag)
            c->cache_valid[set][way] = 0;
}

static uint32_t cache_addr_read(saturn *s, uint32_t a)
{
    sh2 *c = cache_core(s);
    unsigned set = (a >> 4) & 63u;
    unsigned way = (c->onchip[0x92] >> 6) & 3u;
    return (c->cache_tag[set][way] << 10) |
           ((uint32_t)(c->cache_lru[set] & 63u) << 4) |
           ((uint32_t)(c->cache_valid[set][way] != 0) << 2);
}

static void cache_addr_write(saturn *s, uint32_t a, uint32_t v)
{
    sh2 *c = cache_core(s);
    unsigned set = (a >> 4) & 63u;
    unsigned way = (c->onchip[0x92] >> 6) & 3u;
    c->cache_tag[set][way] = (a >> 10) & 0x7FFFFu;
    c->cache_valid[set][way] = (uint8_t)((a >> 2) & 1u);
    c->cache_lru[set] = (uint8_t)((v >> 4) & 63u);
    /* A sequential instruction fetch may have cached this line/way. Force
     * its next access through the tag/LRU path so an explicit address-array
     * write is externally visible before same-line fetches resume. */
    c->if_cache_base = 1u;
}

static void cache_purge_all(sh2 *c)
{
    memset(c->cache_valid, 0, sizeof(c->cache_valid));
    memset(c->cache_lru, 0, sizeof(c->cache_lru));
}

/* Host pointer to a whole 4KB page of plain memory, or NULL if any part of it
 * is not ordinary RAM/ROM. The interpreter caches this per core so an opcode
 * fetch is two byte loads instead of a full bus decode. Pointing into the live
 * arrays keeps self-modifying code visible for free. */
const uint8_t *bus_page(saturn *s, uint32_t a)
{
    return ram_ptr(s, a & 0x07FFF000u, 0x1000u);
}

/* --------------------------------------------------------------- reads */

uint8_t bus_r8(saturn *s, uint32_t a)
{
    uint32_t b, cdoff;
    if (is_cache_purge(a)) {
        cache_purge_addr(s, a);
        return (a & 1u) ? 0x23u : 0x12u;
    }
    if (is_cache_addr(a)) {
        uint32_t v = cache_addr_read(s, a);
        return (uint8_t)(v >> ((~a & 1u) * 8u));
    }
    if (is_cachearr(a)) return *cache_ptr(s, a);
    if (is_onchip(a)) {
        uint32_t off = a & 0xFFFFu;
        sh2 *cc = s->cur ? s->cur : &s->master;
        /* FRC reads must see the count as of now, not as of the last scheduler
         * quantum boundary, or a delay loop that spins on it never terminates. */
        if (off == 0xFE12u) return (uint8_t)(cc->frc >> 8);
        if (off == 0xFE13u) return (uint8_t)cc->frc;
        return cc->onchip[OC(off)];
    }
    b = a & 0x07FFFFFFu;
    if (b >= 0x05A00000u && b < 0x05B00000u) sound_sync(s);
    rrange_note(s, b);
    uint8_t *p = ram_ptr(s, b, 1);
    if (p) return *p;

    if (b >= 0x00100000u && b < 0x00180000u) {               /* SMPC */
        uint32_t o = (b - 0x00100000u) & 0x7F;
        trace(s, b, 0, 1);
        /* Port data registers answer live when the host is driving the ports
         * directly (IOSEL); returning the stored byte would report no pad. */
        if (o == 0x75) return smpc_pdr_read(s, 0);
        if (o == 0x77) return smpc_pdr_read(s, 1);
        return s->smpc_reg[o];
    }
    if (b >= 0x05FE0000u && b < 0x05FF0000u) {               /* SCU  */
        uint32_t o = ((b - 0x05FE0000u) & 0xFF) >> 2;
        trace(s, b, 0, 1);
        if ((b & 0xFCu) >= 0x80u && (b & 0xFCu) <= 0x8Cu) {
            uint32_t v = scu_dsp_read_reg(s, b & 0xFCu);
            return (uint8_t)(v >> (8 * (3 - (b & 3))));
        }
        return (uint8_t)(s->scu_reg[o] >> (8 * (3 - (b & 3))));
    }
    if (cdb_reg_decode(b, &cdoff)) {
        /* The CD block is a 16-bit peripheral bus. Ymir dispatches byte I/O
         * directly: an even register address returns the low byte of that
         * register operation, while odd/unimplemented offsets return zero.
         * Do not turn this into a word read plus byte selection: DATATRNS is a
         * FIFO and that substitution changes side effects. */
        return (uint8_t)cdb_reg_read(s, b, cdoff, 1);
    }
    /* VDP byte access reads the containing word and selects a half. CD byte
     * access is handled above because its FIFO semantics are different. */
    if ((b >= 0x05D00000u && b < 0x05D80000u) ||
        (b >= 0x05F80000u && b < 0x05FC0000u)) {
        uint16_t w = bus_r16(s, b & ~1u);
        return (uint8_t)((b & 1u) ? (w & 0xFF) : (w >> 8));
    }

    trace(s, b, 0, 1);
    s->unmapped_reads++;
    /* 0x030'0000-0x03F'FFFF is not populated on a retail machine and floats
     * high (Ymir memory.cpp maps it to a handler returning all ones). */
    if (b >= 0x00300000u && b < 0x00400000u) return 0xFF;
    /* An EMPTY cartridge slot floats high on every line: Ymir's NoCartridge
     * answers 0xFF / 0xFFFF and reports ID 0xFF. Returning 0x00 reads back as
     * a plausible cart -- NiGHTS built an 8MB heap descriptor over
     * 0x04000000-0x047FFFF0 and then called through it. */
    if (b >= 0x02000000u && b < 0x05000000u) return 0xFF;
    return 0;
}

uint16_t bus_r16(saturn *s, uint32_t a)
{
    uint32_t b, cdoff;
    if (is_cache_purge(a)) {
        cache_purge_addr(s, a);
        return (a & 1u) ? 0x1223u : 0x2312u;
    }
    if (is_cache_addr(a)) return (uint16_t)cache_addr_read(s, a);
    if (is_cachearr(a)) {
        const uint8_t *p = cache_ptr(s, a & ~1u);
        return (uint16_t)((p[0] << 8) | p[1]);
    }
    if (is_onchip(a)) {
        sh2 *cc = s->cur ? s->cur : &s->master;
        uint32_t off = a & 0xFFFEu;
        if (off == 0xFE12u) return cc->frc;
        return (uint16_t)((cc->onchip[OC(off)] << 8) | cc->onchip[OC(off)+1]);
    }
    b = a & 0x07FFFFFEu;
    if (b >= 0x05A00000u && b < 0x05B00000u) sound_sync(s);
    rrange_note(s, b);
    uint8_t *p = ram_ptr(s, b, 2);
    if (p) return (uint16_t)((p[0] << 8) | p[1]);

    if (b >= 0x05D00000u && b < 0x05D80000u) {               /* VDP1 regs */
        trace(s, b, 0, 2);
        /* Ymir vdp_state.hpp MapVDP1RegAddress masks the offset with 0x7FFFF
         * and vdp1_regs.hpp returns 0 for anything that is not EDSR, LOPR,
         * COPR or MODR -- the block does not mirror every 32 bytes, and TVMR
         * through ENDR are write-only. We used to alias the whole 512 KB
         * window onto the first 32 bytes and hand back the last value written
         * to each, and we hardwired EDSR to 0x0002 while COPR/LOPR/MODR read
         * back as zero -- so a program asking VDP1 how far it had got, or
         * what its version was, got an answer the machine never gives. */
        return vdp1_read_reg(s, b - 0x05D00000u);
    }
    if (b >= 0x05F80000u && b < 0x05FC0000u) {               /* VDP2 regs */
        uint32_t o = ((b - 0x05F80000u) & 0x1FF) >> 1;
        trace(s, b, 0, 2);
        if (o == (0x04 >> 1)) {
            /* TVSTAT. Bit 3 (V-Blank) is maintained by the frame loop, but
             * bit 2 (H-Blank) was never asserted at all -- and the BIOS's
             * clock-change tail at 0x0000056C spins on exactly that bit before
             * it will return to the caller. Derive it from the cycle counter:
             * NTSC is 15.734 kHz, so 28.6364 MHz / 15734 = 1820 cycles per
             * line, of which the last ~20% is the horizontal blank. */
            /* Derived from the machine clock, never from a core's own cycle
             * count: the master's counter is frozen while the slave runs, so
             * H-Blank would stand still for the whole of the slave's quantum. */
            uint32_t pos = (uint32_t)(s->clk % CYC_PER_LINE);
            uint16_t v = s->vdp2_reg[o];
            if (pos >= HBLANK_START) v |= 0x0004u; else v &= (uint16_t)~0x0004u;
            return v;
        }
        return s->vdp2_reg[o];
    }
    if (cdb_reg_decode(b, &cdoff))
        return cdb_reg_read(s, b, cdoff, 2);
    if (b >= 0x05FE0000u && b < 0x05FF0000u) {               /* SCU */
        uint32_t off = (b - 0x05FE0000u) & 0xFEu;
        uint32_t v;
        trace(s, b, 0, 2);
        if ((off & 0xFCu) >= 0x80u && (off & 0xFCu) <= 0x8Cu)
            v = scu_dsp_read_reg(s, off & 0xFCu);
        else
            v = s->scu_reg[off >> 2];
        return (uint16_t)((off & 2u) ? v : (v >> 16));
    }
    trace(s, b, 0, 2);
    s->unmapped_reads++;
    if (b >= 0x00300000u && b < 0x00400000u) return 0xFFFF;
    if (b >= 0x02000000u && b < 0x05000000u) return 0xFFFF;   /* empty slot */
    return 0;
}

uint32_t bus_r32(saturn *s, uint32_t a)
{
    uint32_t b;
    if (is_cache_purge(a)) {
        cache_purge_addr(s, a);
        return (a & 1u) ? 0x12231223u : 0x23122312u;
    }
    if (is_cache_addr(a)) return cache_addr_read(s, a);
    if (is_cachearr(a)) {
        const uint8_t *p = cache_ptr(s, a & ~3u);
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
               ((uint32_t)p[2] << 8) | p[3];
    }
    if (is_onchip(a)) return oc_r32(s, a & 0xFFFCu);
    b = a & 0x07FFFFFCu;
    if (b >= 0x05A00000u && b < 0x05B00000u) sound_sync(s);
    rrange_note(s, b);
    uint8_t *p = ram_ptr(s, b, 4);
    if (p)
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
               ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];

    if (b >= 0x05FE0000u && b < 0x05FF0000u) {               /* SCU */
        uint32_t o = ((b - 0x05FE0000u) & 0xFF) >> 2;
        trace(s, b, 0, 4);
        if ((b & 0xFCu) >= 0x80u && (b & 0xFCu) <= 0x8Cu)
            return scu_dsp_read_reg(s, b & 0xFCu);
        return s->scu_reg[o];
    }
    /* Fall back to two halfword reads so peripheral windows stay consistent. */
    return ((uint32_t)bus_r16(s, b) << 16) | bus_r16(s, b + 2);
}

/* -------------------------------------------------------------- writes */

/* After the hybrid handoff the BIOS still believes no bootable disc is
 * present, so its boot path copies the CD player into WRAM a SECOND time
 * (measured: the copy loop at 0x00001FE6 runs twice) straight over the game
 * image we loaded at 0x06004000. Once we have handed control to the game, the
 * BIOS has no business writing into its image -- drop those writes. Only
 * writes issued from BIOS ROM are refused; the game writes its own BSS here
 * all the time. */
static int prot_block(saturn *s, uint32_t a)
{
    uint32_t pc;
    if (!s->prot_hi) return 0;
    a &= 0x0FFFFFFFu;
    if (a < s->prot_lo || a >= s->prot_hi) return 0;
    pc = s->master.pc & 0x0FFFFFFFu;
    /* Only the BIOS's WRAM clear loop at 0x000002B0-0x000002B6. Measured with
     * a width-aware write watch: at cy 47,206,272 it does a 32-bit store to
     * 0x06005FA0 that lands on 0x06005FA2 and erases the slave routine the
     * game wrote at cy 44,115,552 -- after which the slave faults on zeros and
     * the master waits on their handshake forever.
     *
     * Blocking BIOS writes in general does NOT work: its services legitimately
     * write into the game's buffers, and guarding them all sends the machine to
     * PC=0x00000002. This is the one loop that has no business touching the
     * image once the game owns the machine. */
    return pc >= 0x000002B0u && pc <= 0x000002B6u;
}

void bus_w8(saturn *s, uint32_t a, uint8_t v)
{
    if (prot_block(s, a)) return;
    uint32_t b, cdoff;
    if (is_cache_purge(a)) { cache_purge_addr(s, a); return; }
    if (is_cache_addr(a)) { cache_addr_write(s, a, v); return; }
    if (is_cachearr(a)) { *cache_ptr(s, a) = v; return; }
    uint8_t *p;
    if (is_onchip(a)) {
        frt_write8(s->cur ? s->cur : &s->master, a & 0xFFFFu, v);
        return;
    }
    b = a & 0x07FFFFFFu;
    if (b >= 0x05A00000u && b < 0x05B00000u) sound_sync(s);
    /* MINIT/SINIT doorbell. The ADDRESS selects the target CPU and the data is
     * irrelevant -- any write in the region rings it. This path used to fire
     * only on an ODD address, while the 16- and 32-bit paths below fire
     * unconditionally, so a byte write to an even doorbell address was
     * silently dropped. Fighting Vipers rings it that way: its slave parks on
     * FTCSR.ICF at 0x0605ABCC waiting for the capture, its master spins at
     * 0x060B850C waiting for the ack, and the whole game deadlocked after a
     * single successful ping. Neither wider path decomposes into byte writes
     * (both return), so there is no double-capture to guard against. */
    if (b >= 0x01000000u && b < 0x02000000u) {
        frt_capture(b < 0x01800000u ? &s->slave : &s->master);
        return;
    }

    wrange_note(s, b, v, 1);
    if (s->wwatch_addr && wwatch_hit(s, b, 1)) {
        s->wwatch_hits++; s->wwatch_last = v;
        s->wwatch_pc = WW_CORE(s)->pc;
        if (getenv("SATURN_WWHO"))
            printf("[wwho] %08X = %02X  pc=%08X pr=%08X cy=%llu %s\n",
                   b, v, WW_CORE(s)->pc, WW_CORE(s)->pr,
                   (unsigned long long)WW_CORE(s)->cycles,
                   WW_CORE(s)->is_slave ? "SLAVE" : "master");
        if (s->wwatch_first_cy == 0) s->wwatch_first_cy = s->master.cycles;
        s->wwatch_last_cy = s->master.cycles;
        if (s->nwlog < 12) {
            s->wlog[s->nwlog].cy    = s->master.cycles;
            s->wlog[s->nwlog].pc    = WW_CORE(s)->pc;
            s->wlog[s->nwlog].slave = WW_CORE(s)->is_slave;
            s->wlog[s->nwlog].val   = v;
            s->wlog[s->nwlog].sz    = 1;
            s->nwlog++;
        }
    }
    if (b < 0x00100000u) return;                             /* ROM is ROM */
    p = ram_ptr(s, b, 1);
    if (p) {
        *p = v;
        if (b >= 0x05E00000u && b < 0x05F00000u) s->vdp2_vram_epoch++;
        if (b >= 0x05F00000u && b < 0x05F80000u) s->cram_epoch++;
        return;
    }

    if (b >= 0x00100000u && b < 0x00180000u) {               /* SMPC */
        uint32_t o = (b - 0x00100000u) & 0x7F;
        trace(s, b, 1, 1);
        /* Let the command scheduler own COMREG so a rejected write cannot
         * overwrite the command that is still in flight. */
        if (o == 0x1F) smpc_command(s, v);
        else s->smpc_reg[o] = v;
        /* IREG0 mid-INTBACK is the continue/break handshake, not a
         * parameter write. */
        if (o == 0x01) smpc_ireg0_write(s, v);
        return;
    }
    if (cdb_reg_decode(b, &cdoff)) {
        /* Direct byte dispatch is required for side-effectful CD registers.
         * A read/modify/write consumes DATATRNS and acknowledges CR4 merely to
         * write one byte, which is not what the peripheral bus does. */
        cdb_reg_write(s, b, cdoff, v, 1);
        return;
    }
    if ((b >= 0x05D00000u && b < 0x05D80000u) ||
        (b >= 0x05F80000u && b < 0x05FC0000u)) {
        uint32_t wa = b & ~1u;
        uint16_t w  = bus_r16(s, wa);
        w = (b & 1u) ? (uint16_t)((w & 0xFF00) | v)
                     : (uint16_t)((w & 0x00FF) | ((uint16_t)v << 8));
        bus_w16(s, wa, w);
        return;
    }

    trace(s, b, 1, 1);
    s->unmapped_writes++;
}

void bus_w16(saturn *s, uint32_t a, uint16_t v)
{
    if (prot_block(s, a)) return;
    uint32_t b, cdoff;
    if (is_cache_purge(a)) { cache_purge_addr(s, a); return; }
    if (is_cache_addr(a)) { cache_addr_write(s, a, v); return; }
    if (is_cachearr(a)) {
        uint8_t *p = cache_ptr(s, a & ~1u);
        p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; return;
    }
    uint8_t *p;
    if (is_onchip(a)) {
        sh2 *cc = s->cur ? s->cur : &s->master;
        uint32_t off = a & 0xFFFEu;
        frt_write8(cc, off,      (uint8_t)(v >> 8));
        frt_write8(cc, off + 1u, (uint8_t)v);
        return;
    }
    b = a & 0x07FFFFFEu;
    if (b >= 0x05A00000u && b < 0x05B00000u) sound_sync(s);

    /* MINIT/SINIT: the doorbell is named for who RINGS it, not who hears it.
     * Writes to MINIT (0x01000000-0x017FFFFF) pulse the SLAVE's FRT
     * input-capture pin; writes to SINIT (0x01800000-0x01FFFFFF) pulse the
     * MASTER's. Ymir derives this from the SH7604 BCR1.MASTER bit --
     * sh2.cpp:399 maps 0x1000000 + IsMaster()*0x800000 to each core's own
     * TriggerFRTInputCapture, so the master owns SINIT and the slave owns
     * MINIT -- and states it outright in docs/dev-notes/system-info/
     * sh2-memory-map.txt:54.
     *
     * We had this inverted, which is why Sonic 3D Blast's FMV hung: SGL's
     * slSynch (master) queues a command and rings MINIT at 0x21000000 to wake
     * the slave's command dispatcher at 0x06070D00. Routed to the master it
     * was a self-ping the master never waits on, so the slave stayed parked on
     * FTCSR.ICF forever, command 4 (list-finalise, 0x06071E68) never ran, the
     * "VDP1 list pending" flag at GBR+168 never got set, and slSynch spun for
     * the rest of the run. It also explains the master-side ICI storm. */
    if (b >= 0x01000000u && b < 0x02000000u) {
        frt_capture(b < 0x01800000u ? &s->slave : &s->master);
        return;
    }

    wrange_note(s, b, v, 2);
    if (s->wwatch_addr && b <= (s->wwatch_addr & 0x07FFFFFFu) &&
        (s->wwatch_addr & 0x07FFFFFFu) < b + 2) {
        s->wwatch_hits++; s->wwatch_last = v;
        s->wwatch_pc = WW_CORE(s)->pc;
        if (getenv("SATURN_WWHO"))
            printf("[wwho] %08X = %08X (sz2) pc=%08X pr=%08X cy=%llu %s\n",
                   b, (uint32_t)v, WW_CORE(s)->pc, WW_CORE(s)->pr,
                   (unsigned long long)WW_CORE(s)->cycles,
                   WW_CORE(s)->is_slave ? "SLAVE" : "master");
        if (s->wwatch_first_cy == 0) s->wwatch_first_cy = s->master.cycles;
        s->wwatch_last_cy = s->master.cycles;   /* last writer wins */
    }
    if (b < 0x00100000u) return;
    p = ram_ptr(s, b, 2);
    if (p) {
        p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
        if (b >= 0x05E00000u && b < 0x05F00000u) s->vdp2_vram_epoch++;
        if (b >= 0x05F00000u && b < 0x05F80000u) s->cram_epoch++;
        return;
    }

    if (b >= 0x05D00000u && b < 0x05D80000u) {               /* VDP1 regs */
        trace(s, b, 1, 2);
        vdp1_write_reg(s, b - 0x05D00000u, v);
        return;
    }
    if (b >= 0x05F80000u && b < 0x05FC0000u) {               /* VDP2 regs */
        uint32_t o = ((b - 0x05F80000u) & 0x1FF) >> 1;
        trace(s, b, 1, 2);
        s->vdp2_reg[o] = v;
        return;
    }
    if (cdb_reg_decode(b, &cdoff)) {
        cdb_reg_write(s, b, cdoff, v, 2);
        return;
    }
    if (b >= 0x05FE0080u && b < 0x05FE0090u) {               /* SCU DSP */
        uint32_t off = (b - 0x05FE0000u) & 0xFEu;
        trace(s, b, 1, 2);
        /* Match Ymir's halfword ports. PPD/PDD halfword writes are ignored;
         * PDA only accepts its low half, while PPAF splits control and PC. */
        if (off == 0x80u) {
            uint32_t ctl = (uint32_t)v << 16;
            scu_dsp_write_reg(s, 0x80u, ctl);
        } else if (off == 0x82u) {
            if (v & 0x8000u) {
                uint32_t ctl = (uint32_t)(v & 0x80FFu);
                scu_dsp_write_reg(s, 0x80u, ctl);
            }
        } else if (off == 0x8Au) {
            scu_dsp_write_reg(s, 0x88u, v & 0xFFu);
        }
        return;
    }
    trace(s, b, 1, 2);
    s->unmapped_writes++;
}

void bus_w32(saturn *s, uint32_t a, uint32_t v)
{
    if (prot_block(s, a)) return;
    uint32_t b;
    if (is_cache_purge(a)) { cache_purge_addr(s, a); return; }
    if (is_cache_addr(a)) { cache_addr_write(s, a, v); return; }
    if (is_cachearr(a)) {
        uint8_t *p = cache_ptr(s, a & ~3u);
        p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
        p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v; return;
    }
    uint8_t *p;
    if (is_onchip(a)) {
        uint32_t off = a & 0xFFFCu;
        oc_w32(s, off, v);
        /* Writing CHCR with DE set kicks the channel. */
        if (off == 0xFF8Cu) dmac_run(s, 0);
        if (off == 0xFF9Cu) dmac_run(s, 1);
        if (off == 0xFFB0u) { dmac_run(s, 0); dmac_run(s, 1); }
        return;
    }
    b = a & 0x07FFFFFCu;
    if (b >= 0x05A00000u && b < 0x05B00000u) sound_sync(s);
    if (b >= 0x01000000u && b < 0x02000000u) {
        frt_capture(b < 0x01800000u ? &s->slave : &s->master);
        return;
    }

    wrange_note(s, b, v, 4);
    if (s->wwatch_addr && b <= (s->wwatch_addr & 0x07FFFFFFu) &&
        (s->wwatch_addr & 0x07FFFFFFu) < b + 4) {
        s->wwatch_hits++; s->wwatch_last = v;
        s->wwatch_pc = WW_CORE(s)->pc;
        if (getenv("SATURN_WWHO"))
            printf("[wwho] %08X = %08X (sz4) pc=%08X pr=%08X cy=%llu %s\n",
                   b, (uint32_t)v, WW_CORE(s)->pc, WW_CORE(s)->pr,
                   (unsigned long long)WW_CORE(s)->cycles,
                   WW_CORE(s)->is_slave ? "SLAVE" : "master");
        if (s->wwatch_first_cy == 0) s->wwatch_first_cy = s->master.cycles;
        s->wwatch_last_cy = s->master.cycles;   /* last writer wins */
    }
    if (b < 0x00100000u) return;
    p = ram_ptr(s, b, 4);
    if (p) {
        p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
        p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
        if (b >= 0x05E00000u && b < 0x05F00000u) s->vdp2_vram_epoch++;
        if (b >= 0x05F00000u && b < 0x05F80000u) s->cram_epoch++;
        return;
    }
    if (b >= 0x05FE0000u && b < 0x05FF0000u) {               /* SCU */
        uint32_t o = ((b - 0x05FE0000u) & 0xFF) >> 2;
        trace(s, b, 1, 4);
        if ((b & 0xFCu) >= 0x80u && (b & 0xFCu) <= 0x8Cu) {
            scu_dsp_write_reg(s, b & 0xFCu, v);
            return;
        }
        if (o == (0xA0 >> 2) && getenv("SATURN_IMSDBG"))
            printf("[ims] %08X from PC %08X cy=%llu%s", v, s->cur ? s->cur->pc : 0,
                   (unsigned long long)(s->master.cycles), "\n");
        s->scu_reg[o] = v;
        /* Timer registers (Ymir scu.cpp WriteReg 0x90/0x94/0x98).
         *   T0C  bits 0-9  = timer 0 compare value
         *   T1S            = timer 1 reload
         *   T1MD bit 0     = timer enable, bit 8 = timer 1 mode */
        if (o == (0x90 >> 2)) s->scu_t0_compare = (uint16_t)(v & 0x3FFu);
        if (o == (0x94 >> 2)) s->scu_t1_reload  = (uint16_t)(v & 0x1FFu);
        if (o == (0x98 >> 2)) {
            s->scu_timer_enable = (uint8_t)(v & 1u);
            s->scu_t1_mode      = (uint8_t)((v >> 8) & 1u);
        }
        /* Writing a channel's enable register with the go bit set starts it.
         * DxEN is at +0x10 within each 0x20-byte block. */
        if (o == 0x04 || o == 0x0C || o == 0x14) {
            /* DxEN: bit 8 enables the channel, bit 0 is the start bit.
             *
             * A channel only starts HERE when its trigger factor is 7
             * (immediate). For every other factor the channel is merely ARMED,
             * and hardware starts it when the chosen event occurs -- V-Blank
             * IN/OUT, H-Blank IN, Timer 0/1, Sound Request or Sprite Draw End
             * (Ymir scu.cpp TriggerDMATransfer, scu_dma.hpp DMATrigger).
             *
             * Running every armed channel immediately, as this used to, is
             * wrong in both directions: it transfers before the game expects
             * it, and it means a channel armed for a repeating event fires
             * exactly once instead of once per frame. Sonic 3D Blast arms
             * levels 0-2 to stream its movie and then issues no further CD
             * command; with no factor dispatch the stream never ran, the
             * player decoded nothing, and the screen stayed black. */
            int lvl = (int)(o / 8);
            uint32_t md = s->scu_reg[(lvl * 0x20u + 0x14u) >> 2];
            /* The factor is DxMD bits 0-2, NOT 16-18 (Ymir scu.cpp:
             * bit::extract<0,2>). Getting this wrong makes every immediate
             * transfer read as factor 0 and stop firing, which breaks the
             * DMA NiGHTS waits on. */
            unsigned factor = md & 7u;
            if ((v & 0x101u) && factor == 7u) scu_dma_run(s, lvl);
        }
        return;
    }
    bus_w16(s, b,     (uint16_t)(v >> 16));
    bus_w16(s, b + 2, (uint16_t)v);
}

/* ----------------------------------------------------------------- init */

void saturn_init(saturn *s)
{
    memset(s, 0, sizeof(*s));
    s->trace_enabled = 1;
    s->min_imask = 99;
    /* The SMPC powers up with the front-panel reset button disabled. */
    s->smpc_resd = 1;
    /* CD block spells "CDBLOCK" in CR1-CR4 after reset. */
    s->cdb_reg[0x18 >> 1] = 0x4344;   /* "CD" */
    s->cdb_reg[0x1C >> 1] = 0x424C;   /* "BL" */
    s->cdb_reg[0x20 >> 1] = 0x4F43;   /* "OC" */
    s->cdb_reg[0x24 >> 1] = 0x4B00;   /* "K"  */

    sound_init(s);

    /* Format the internal backup RAM. A blank BUP is not "empty" to a game --
     * it is UNFORMATTED, and the BUP library refuses to use it: NiGHTS boots
     * to its own "The System Memory is not ready for use." screen and goes no
     * further. The image is interleaved (data in the odd bytes, even bytes
     * read as 0xFF) and the first 64-byte block holds "BackUpRam Format"
     * repeated every 16 bytes (Ymir bup::BackupMemory::Format). */
    {
        static const char hdr[16] = { 'B','a','c','k','U','p','R','a','m',
                                      ' ','F','o','r','m','a','t' };
        /* Even bytes are the unused half of each 16-bit word. Hardware
         * returns 0xFF there (Ymir ReadWord: 0xFF00 | data), and our linear
         * map reproduces that only if they hold 0xFF -- but SATURN_BUPRAW=1
         * leaves them zero to isolate BUP-shape bugs. */
        if (!getenv("SATURN_BUPRAW"))
            for (uint32_t i = 0; i < BUP_SIZE; i += 2) s->bup[i] = 0xFF;
        for (uint32_t i = 0; i < 64; i++) s->bup[i * 2u + 1u] = (uint8_t)hdr[i & 15];
    }
}

void saturn_report_trace(saturn *s, FILE *out)
{
    fprintf(out, "\nperipheral accesses (first %d distinct, in order seen)\n", TRACE_SLOTS);
    fprintf(out, "%-10s %-3s %-3s %-9s %s\n", "ADDRESS", "R/W", "SZ", "COUNT", "REGION");
    for (int i = 0; i < s->ntrace; i++) {
        const char *rn = trace_region_name(s->trace[i].addr);
        fprintf(out, "0x%08X %-3s %-3u %-9u %s\n",
                s->trace[i].addr, s->trace[i].is_write ? "W" : "R",
                s->trace[i].size, s->trace[i].count, rn);
    }
    fprintf(out, "unmapped: %llu reads, %llu writes\n",
            (unsigned long long)s->unmapped_reads,
            (unsigned long long)s->unmapped_writes);
}

/* ----------------------------------------------------- SCU interrupts ----
 * Table per the SCU User's Manual (docs/HARDWARE.md §4.1). Index is the
 * pending/mask bit number; entries are {vector, SH-2 level}.
 *
 * Delivery rule: the source must be pending, not masked in the SCU interrupt
 * mask register (0x25FE00A0, where a SET bit means MASKED), and its level must
 * exceed the SH-2's SR.I field.
 */
static const struct { uint8_t vector; uint8_t level; } scu_irq[16] = {
    { 0x40, 15 },  /* 0  V-Blank-IN      */
    { 0x41, 14 },  /* 1  V-Blank-OUT     */
    { 0x42, 13 },  /* 2  H-Blank-IN      */
    { 0x43, 12 },  /* 3  Timer 0         */
    { 0x44, 11 },  /* 4  Timer 1         */
    { 0x45, 10 },  /* 5  DSP End         */
    { 0x46,  9 },  /* 6  Sound Request   */
    { 0x47,  8 },  /* 7  System Manager  */
    { 0x48,  8 },  /* 8  PAD             */
    { 0x49,  6 },  /* 9  Level-2 DMA End */
    { 0x4A,  6 },  /* 10 Level-1 DMA End */
    { 0x4B,  5 },  /* 11 Level-0 DMA End */
    { 0x4C,  3 },  /* 12 DMA-Illegal     */
    { 0x4D,  2 },  /* 13 Sprite Draw End */
    { 0x00,  0 },
    { 0x00,  0 },
};

/* SCU sources 16-31 are the external (A-Bus) interrupts, vectors 0x50-0x5F.
 * External 00 is the one that matters at boot: the CD block asserts it
 * whenever an unmasked HIRQ flag is set. Without it a title that issues a
 * command and sleeps waiting for completion never wakes up. */
static const struct { uint8_t vector; uint8_t level; } scu_irq_ext[16] = {
    { 0x50, 7 }, { 0x51, 7 }, { 0x52, 7 }, { 0x53, 7 },
    { 0x54, 4 }, { 0x55, 4 }, { 0x56, 4 }, { 0x57, 4 },
    { 0x58, 1 }, { 0x59, 1 }, { 0x5A, 1 }, { 0x5B, 1 },
    { 0x5C, 1 }, { 0x5D, 1 }, { 0x5E, 1 }, { 0x5F, 1 },
};

/* Is the sound side switched off entirely? Cached: this sits in the per-field
 * path and a getenv per frame is pure waste. */
static int snd_disabled(saturn *s)
{
    static int v = -1;
    (void)s;
    if (v < 0) v = getenv("SATURN_NOSOUND") != NULL;
    return v;
}

/* Timer 0 counter reset at V-Blank OUT, then a compare check (Ymir
 * SCU::UpdateVBlank: counter = 0; CheckTimer0()). */
static void scu_check_timer0(saturn *s)
{
    if (s->scu_timer_enable && s->scu_t0_counter == s->scu_t0_compare) {
        scu_raise(s, 3);
        scu_dma_trigger(s, 3);
    }
}

void scu_timer_vblank_out(saturn *s)
{
    s->scu_t0_counter = 0;
    scu_check_timer0(s);
}

/* One H-Blank IN: advance the line counter, check Timer 0, then tick Timer 1.
 * Ymir schedules Timer 1 `reload + 1` cycles into the line; we have no
 * scheduler here, so it fires at the line boundary instead -- the line it
 * lands on is the same, only the intra-line phase differs. */
void scu_timer_hblank(saturn *s)
{
    if (!s->scu_timer_enable) return;
    s->scu_t0_counter = (uint16_t)((s->scu_t0_counter + 1u) & 0x1FFu);
    scu_check_timer0(s);
    if (!s->scu_t1_mode || s->scu_t0_counter == s->scu_t0_compare) {
        scu_raise(s, 4);
        scu_dma_trigger(s, 4);
    }
}

void scu_raise(saturn *s, int bit)
{
    if (bit < 0 || bit > 31) return;
    /* SATURN_NOINT is a bitmask of SCU sources to suppress, for bisecting
     * which interrupt is disturbing the boot. */
    {
        static uint32_t noint = 0xFFFFFFFFu;      /* sentinel: unresolved */
        if (noint == 0xFFFFFFFFu) {
            const char *e = getenv("SATURN_NOINT");
            noint = e ? (uint32_t)strtoul(e, NULL, 0) : 0u;
        }
        if (noint & (1u << bit)) return;
    }
    if (bit < 32) s->scu_raise_hist[bit]++;
    s->scu_ipend |= 1u << bit;
    s->scu_reg[0xA4 >> 2] |= 1u << bit;      /* interrupt status register */
}

/* --------------------------------------------------- free-running timer ----
 *
 * SH7604 FRT, one instance per core (its registers live in that core's on-chip
 * bank). The Saturn ties the input-capture pin FTI to the horizontal blank, so
 * ICF sets once per scanline; that is the only clock a program running on the
 * slave has, and NiGHTS uses exactly it -- the slave sits in a three
 * instruction poll at 0x06005F9E (mov.b @r2,r0 / tst r1,r0 / bt/s, with
 * r2 = 0xFFFFFE11 and r1 = 0x80) waiting for the flag.
 *
 * Previously the flag was synthesised on read from s->master.cycles. That is
 * wrong twice over: it is the wrong core's clock, and that clock does not
 * advance at all while the slave is the one executing -- so for the whole of
 * the slave's quantum the flag read as one frozen value. It is a real latch
 * now: set at the H-Blank edge, cleared by software.
 *
 *   FE10 TIER   FE11 FTCSR  FE12/13 FRC  FE14/15 OCRA/B
 *   FE16 TCR    FE17 TOCR   FE18/19 FICR
 */
#define FTCSR_ICF   0x80u
#define FTCSR_OCFA  0x08u
#define FTCSR_OCFB  0x04u
#define FTCSR_OVF   0x02u
#define FTCSR_CCLRA 0x01u

/* Cached "an enabled FRT interrupt is pending" so the interpreter's fast
 * path can gate on one int instead of re-deriving TIER & FTCSR per
 * instruction boundary. Recomputed wherever either register changes. */
int g_frt_irq;   /* defined here; frt_irq_init() sets it from the environment */

static void frt_update_pend(sh2 *c)
{
    uint8_t t = c->onchip[OC(0xFE10u)], f = c->onchip[OC(0xFE11u)];
    /* Respect the delivery switch here as well as in frt_pending, so the
     * interpreter's fast path is not woken up for an interrupt that will
     * then be declined. */
    c->frt_pend = g_frt_irq && (int)(((t & f) & 0x8Eu) != 0);
}

/* TCR bits 1-0 select the count clock: phi/8, phi/32, phi/128, or the external
 * FTI pin, in which case the counter only moves on capture edges. */
static uint32_t frt_div(const sh2 *c)
{
    switch (c->onchip[OC(0xFE16u)] & 3u) {
    case 0:  return 8u;
    case 1:  return 32u;
    case 2:  return 128u;
    default: return 0u;                  /* external clock: no free running */
    }
}

/* OCRA and OCRB share one address pair; TOCR bit 4 (OCRS) selects which one
 * writes land on. Both are kept in the bank, at FE14 and FE1A. */
static uint16_t frt_ocr(const sh2 *c, int b)
{
    uint32_t o = b ? OC(0xFE1Au) : OC(0xFE14u);
    return (uint16_t)((c->onchip[o] << 8) | c->onchip[o + 1]);
}

/* SATURN_NOFRT restores the pre-scheduler behaviour: FRC never counted and
 * the capture flag was synthesised per read. Purely a bisect switch -- it
 * exists so 'did the timer model break this?' is one run, not a revert. */
static int frt_off = -1;
static int frt_disabled(void)
{
    if (frt_off < 0) frt_off = getenv("SATURN_NOFRT") != NULL;
    return frt_off;
}

void frt_advance(sh2 *c, uint32_t cycles)
{
    uint32_t div = frt_disabled() ? 0u : frt_div(c);
    uint32_t ticks;
    uint8_t *fc;
    uint16_t ocra, ocrb;

    if (!div) return;

    c->frt_pre += cycles;
    ticks = c->frt_pre / div;
    if (!ticks) return;
    c->frt_pre -= ticks * div;

    fc   = &c->onchip[OC(0xFE11u)];
    ocra = frt_ocr(c, 0);
    ocrb = frt_ocr(c, 1);

    /* One tick at a time so a compare match or a wrap inside the span is not
     * stepped over. A scanline is CYC_PER_LINE cycles, so even on the fastest
     * divider this is at most ~228 iterations, and the quantum is far shorter. */
    while (ticks--) {
        uint16_t prev = c->frc;
        c->frc = (uint16_t)(c->frc + 1u);
        if (c->frc < prev) *fc |= FTCSR_OVF;         /* wrapped through zero */
        if (c->frc == ocra) {
            *fc |= FTCSR_OCFA;
            if (*fc & FTCSR_CCLRA) { c->frc = 0; c->frt_pre = 0; }
        }
        if (c->frc == ocrb) *fc |= FTCSR_OCFB;
        frt_update_pend(c);
    }
}

/* FRT input capture. On the Saturn the FTI pins are wired to the MINIT and
 * SINIT mail slots (Ymir sh2.cpp: writes to 0x01000000/0x01800000 trigger the
 * owning core's capture) -- NOT to H-Blank. The per-scanline model raised ICF
 * 263 times a field, which is where the 20M-interrupt storm came from
 * whenever ICI delivery was enabled. */
uint64_t g_capture_count[2];   /* [0] master doorbell, [1] slave doorbell */

void frt_capture(sh2 *c)
{
    uint8_t *fc = &c->onchip[OC(0xFE11u)];
    sh2 *caller = c->sys ? c->sys->cur : NULL;
    uint8_t old = *fc;
    if (frt_disabled()) return;
    g_capture_count[c->is_slave ? 1 : 0]++;
    /* TCR bit 7 (IEDGA) picks the capture edge. Only the leading edge of
     * H-Blank is presented, so a program asking for the trailing edge still
     * gets one capture per line -- half a line early, which nothing measures. */
    c->onchip[OC(0xFE18u)] = (uint8_t)(c->frc >> 8);
    c->onchip[OC(0xFE19u)] = (uint8_t)c->frc;
    *fc |= FTCSR_ICF;
    frt_update_pend(c);
    if (getenv("SATURN_FRTCAP"))
        fprintf(stderr,
                "[frtcap] target=%s caller=%s pc=%08X pr=%08X old=%02X new=%02X "
                "cy=%llu count=%llu\n",
                c->is_slave ? "slave" : "master",
                caller ? (caller->is_slave ? "slave" : "master") : "none",
                caller ? caller->pc : 0u, caller ? caller->pr : 0u,
                old, *fc,
                (unsigned long long)(c->sys ? c->sys->clk : c->cycles),
                (unsigned long long)g_capture_count[c->is_slave ? 1 : 0]);
    /* With the external count clock selected, FTI is also the count input. */
    if ((c->onchip[OC(0xFE16u)] & 3u) == 3u) c->frc = (uint16_t)(c->frc + 1u);
}

/* Writes into the on-chip file. Everything outside the FRT is plain storage;
 * the FRT status register is write-to-clear and FRC is a live counter rather
 * than a byte in the bank, so both need routing. */
void frt_write8(sh2 *c, uint32_t off, uint8_t v)
{
    uint32_t o = OC(off);
    switch (off & 0xFFFFu) {
    case 0xFE10u:
        /* SATURN_TIERLOG: who enables an FRT interrupt, and from where. ICIE
         * (bit 7) with no handler installed is an interrupt storm waiting to
         * happen, so it matters whether the GAME asked for it or we invented
         * it. */
        if (getenv("SATURN_TIERLOG") && ((v ^ c->onchip[o]) & 0x8Eu)) {
            static int n;
            if (n++ < 20)
                fprintf(stderr, "[tier] %s %02X -> %02X at pc=%08X pr=%08X cy=%llu\n",
                        c->is_slave ? "slave" : "master", c->onchip[o], v,
                        c->pc, c->pr, (unsigned long long)c->cycles);
        }
        c->onchip[o] = v;
        frt_update_pend(c);
        return;
    case 0xFE11u:
        /* FTCSR: a flag clears only where software writes 0. Bits written as 1
         * keep what they had, so a read-modify-write that sets an unrelated bit
         * cannot clear a flag that set between the read and the write. */
        {
            uint8_t old = c->onchip[o];
            c->onchip[o] = (uint8_t)((old & v & 0x8Eu) | (v & FTCSR_CCLRA));
            if (getenv("SATURN_FRTCAP") && old != c->onchip[o])
                fprintf(stderr,
                        "[frtclear] core=%s pc=%08X pr=%08X write=%02X old=%02X "
                        "new=%02X cy=%llu\n",
                        c->is_slave ? "slave" : "master", c->pc, c->pr, v,
                        old, c->onchip[o],
                        (unsigned long long)(c->sys ? c->sys->clk : c->cycles));
        }
        frt_update_pend(c);
        return;
    case 0xFE12u:
        c->frc = (uint16_t)((c->frc & 0x00FFu) | ((uint32_t)v << 8));
        c->frt_pre = 0;
        return;
    case 0xFE13u:
        c->frc = (uint16_t)((c->frc & 0xFF00u) | v);
        c->frt_pre = 0;
        return;
    case 0xFE14u: case 0xFE15u:
        if (c->onchip[OC(0xFE17u)] & 0x10u) o = OC(0xFE1Au) + (off & 1u);
        c->onchip[o] = v;
        return;
    case 0xFE92u: /* CCR: cache enable/mode and write-one cache purge. */
        if (getenv("SATURN_CACHELOG"))
            fprintf(stderr,
                    "[cache-ccr] %s %02X -> %02X pc=%08X pr=%08X clk=%llu\n",
                    c->is_slave ? "slave" : "master", c->onchip[o], v,
                    c->pc, c->pr,
                    (unsigned long long)(c->sys ? c->sys->clk : c->cycles));
        if (v & 0x10u) cache_purge_all(c);
        /* CP self-clears; bit 5 is reserved and reads zero. */
        c->onchip[o] = (uint8_t)(v & 0xCFu);
        c->if_tag = 1;
        return;
    default:
        c->onchip[o] = v;
        return;
    }
}

/* Off unless SATURN_FRTIRQ is set. Taking an interrupt on ICF is not something
 * this project has yet seen a title ask for -- NiGHTS POLLS the flag -- and
 * enabling it speculatively cost the BIOS its pixel-exact render: the master
 * took 20.4M interrupts in 50M instructions, all on the vector read out of
 * VCRC, because ICF is level-triggered and nothing was clearing it. Left in,
 * behind a switch, so the next title that does want it has somewhere to start.
 */

/* The comment above records a MEASURED result: delivering ICI storms, because
 * ICF is level-triggered and the handler a title installs for it -- when it
 * installs one at all -- is the BIOS stub that just returns. This line used to
 * read `getenv("SATURN_NOFRTIRQ") == NULL`, i.e. ON by default, the exact
 * opposite of what the comment specifies. Sonic 3D Blast took 464 MILLION ICI
 * interrupts on vector 0x64 and never ran its main loop again, which is why
 * its display stayed switched off after the FMV.
 *
 * Off unless asked for, as documented. SATURN_FRTIRQ turns it back on for the
 * first title that genuinely wants ICI delivery. */
void frt_irq_init(void) { g_frt_irq = getenv("SATURN_FRTIRQ") != NULL; }

/* Cheap query for the interpreter's interrupt fast path. */
int frt_irq_on(void) { return g_frt_irq; }

/* An FRT interrupt for this core, if one is enabled and pending. Vector and
 * level come from the core's own INTC: IPRB bits 11-8, VCRC and VCRD. Unlike
 * the SCU sources this is per-core, and it is the only interrupt the slave
 * SH-2 can take -- the SCU drives the master alone. */
/* SH-2 on-chip DMAC transfer-end interrupt.
 *
 * Ymir raises DMAC0/1_XferEnd when `xferEnded && irqEnable` (sh2.cpp ~1913,
 * condition re-checked at ~2044). We ran the transfer and set TE but never
 * raised anything, so a title that arms CHCR.IE and sleeps waiting for the
 * completion interrupt would wait forever.
 *
 * Level comes from IPRA bits 11-8; the vector from VCRDMA0 (0xFFFFFFA0) or
 * VCRDMA1 (0xFFFFFFA8). Like every other on-chip source this is per core.
 *
 * TE is level-triggered, exactly like the FRT's ICF, so a handler that does
 * not clear it will re-enter forever -- see the ICI storm in bus.c's frt
 * section. That is hardware behaviour, not a bug to paper over here. */
/* Core-explicit on-chip longword read. oc_r32() resolves the bank through
 * s->cur, which is the WRONG core when the scheduler asks the other one
 * whether it has an interrupt pending. */
static uint32_t oc_r32_c(const sh2 *c, uint32_t off)
{
    uint32_t o = OC(off);
    return ((uint32_t)c->onchip[o] << 24) | ((uint32_t)c->onchip[o+1] << 16) |
           ((uint32_t)c->onchip[o+2] << 8) | c->onchip[o+3];
}

int dmac_pending(sh2 *c, uint32_t sr, int *vector, int *level)
{
    uint32_t ipra = ((uint32_t)c->onchip[OC(0xFEE2u)] << 8) | c->onchip[OC(0xFEE3u)];
    int lvl = (int)((ipra >> 8) & 0xFu);
    uint32_t dmaor = oc_r32_c(c, 0xFFB0u);
    int ch;

    if (lvl == 0 || lvl <= (int)((sr >> 4) & 0xFu)) return 0;
    if (!(dmaor & 1u)) return 0;                  /* DME clear */

    for (ch = 0; ch < 2; ch++) {
        uint32_t base = ch ? 0xFF90u : 0xFF80u;
        uint32_t chcr = oc_r32_c(c, base + 0x0Cu);
        if (!(chcr & 0x04u)) continue;            /* IE clear   */
        if (!(chcr & 0x02u)) continue;            /* TE clear   */
        *vector = (int)(oc_r32_c(c, ch ? 0xFFA8u : 0xFFA0u) & 0x7Fu);
        *level  = lvl;
        return 1;
    }
    return 0;
}

int frt_pending(sh2 *c, uint32_t sr, int *vector, int *level)
{
    uint8_t  tier  = c->onchip[OC(0xFE10u)];
    uint8_t  ftcsr = c->onchip[OC(0xFE11u)];
    uint32_t iprb  = ((uint32_t)c->onchip[OC(0xFE60u)] << 8) | c->onchip[OC(0xFE61u)];
    int      lvl   = (int)((iprb >> 8) & 0xFu);
    uint32_t vcrc, vcrd;

    /* The delivery switch. frt_irq_on() existed and was documented but was
     * never called from anywhere, so ICI was delivered unconditionally --
     * 464 million times in one Sonic 3D Blast run, on a BIOS stub handler
     * that cannot clear the level-triggered ICF that caused it. */
    /* The slave reset BIOS sleeps until the master's MINIT pulse raises its
     * input-capture interrupt. Suppressing that interrupt leaves the freshly
     * reset slave asleep forever. Keep the historical opt-in for the master
     * (whose incomplete handlers used to create a storm), but always deliver
     * the slave's only external interrupt source. */
    if (!g_frt_irq && !c->is_slave) return 0;

    if (lvl == 0 || lvl <= (int)((sr >> 4) & 0xFu)) return 0;


    vcrc = ((uint32_t)c->onchip[OC(0xFE66u)] << 8) | c->onchip[OC(0xFE67u)];
    vcrd = ((uint32_t)c->onchip[OC(0xFE68u)] << 8) | c->onchip[OC(0xFE69u)];

    if ((tier & 0x80u) && (ftcsr & FTCSR_ICF)) {                  /* ICI  */
        *vector = (int)((vcrc >> 8) & 0x7Fu); *level = lvl;
        c->sys->frt_irqs++; return 1;
    }
    if (tier & ftcsr & 0x0Cu) {                                   /* OCIA/B */
        *vector = (int)(vcrc & 0x7Fu);        *level = lvl;
        c->sys->frt_irqs++; return 1;
    }
    if ((tier & 0x02u) && (ftcsr & FTCSR_OVF)) {                  /* OVI  */
        *vector = (int)((vcrd >> 8) & 0x7Fu); *level = lvl;
        c->sys->frt_irqs++; return 1;
    }
    return 0;
}

/* ------------------------------------------------------------- scheduler ---
 *
 * The old model ran the master for half a frame, then handed the slave the same
 * count, then raised V-Blank. The two cores were therefore never at the same
 * point in machine time, and every video event landed on a 238000-instruction
 * boundary. A handshake in which the master parks on a flag the slave clears
 * (NiGHTS: master at 0x06032CCE, slave at 0x06005F9E) cannot close under that
 * model -- whichever core is scheduled runs to the end of its slice against a
 * frozen view of the other.
 *
 * Both cores now advance against one clock, in quanta short enough that a poll
 * on one observes the other's store within the same scanline, and video events
 * fall on the line they belong to.
 */

/* Report the first slave fault in full: the ring of PCs it retired, the
 * instruction it died on, and the words either side. Diagnosing the slave from
 * end-of-run state does not work -- the game tears its code down afterwards --
 * so this has to fire at the moment. */
static void slave_halt_report(saturn *s)
{
    static int told;
    uint32_t n, k;
    int q;

    if (told) return;
    told = 1;

    printf("[slave halt] pc=%08X pr=%08X sr=%08X after %llu cycles fault=%s @%08X "
           "r1=%08X r2=%08X r4=%08X op@pc=%04X op@fault=%04X\n",
           s->slave.pc, s->slave.pr, s->slave.sr,
           (unsigned long long)s->slave.cycles,
           s->slave.fault ? s->slave.fault : "(none)",
           s->slave.fault_pc, s->slave.r[1], s->slave.r[2], s->slave.r[4],
           bus_r16(s, s->slave.pc), bus_r16(s, s->slave.fault_pc));

    n = s->sring_head < 160u ? s->sring_head : 160u;
    for (k = n; k > 0; k--) {
        uint32_t a = s->sring[(s->sring_head - k) & 255u];
        char txt[64];
        if (!sh2_format(bus_r16(s, a), a, txt)) txt[0] = 0;
        printf("    slave %08X  %s\n", a, txt);
    }
    for (q = -6; q <= 4; q++) {
        uint32_t a = s->slave.pc + (uint32_t)(q * 2);
        printf("    %08X  %04X\n", a, bus_r16(s, a));
    }
}

/* The slave SH-2. SMPC SSHON brings it out of reset; the master registers its
 * entry/stack pair at 0x06000250/54. */
static void run_slave(saturn *s, uint64_t budget)
{
    if (!s->slave_enabled || (s->hle_active && !s->slave_entry)) return;
    if (!s->slave_started) {
        uint32_t entry, sp;
        if (s->hle_active) {
            entry = s->slave_entry;
            sp = s->slave_sp;
        } else {
            /* SSHON is a real hard reset, not a jump to the mailbox. The slave
             * reads the same reset vectors as the master, executes the BIOS
             * CPU-identification/setup path, and the BIOS trampoline then
             * consumes 0x06000250/54. Skipping straight to slave_entry left
             * registers and coordination state uninitialised and let the
             * master overwrite a live slave overlay during Sonic's title
             * transition. This matches Ymir's EnableAndResetSlaveSH2 path. */
            entry = bus_r32(s, 0x00000000u);
            sp = bus_r32(s, 0x00000004u);
        }
        sh2_reset(&s->slave, s, 1, entry, sp);
        s->slave_started = 1;
    }
    if (!s->slave.halted) {
        s->cur = &s->slave;
        sh2_run(&s->slave, budget);
        s->cur = &s->master;
        if (s->slave.halted) slave_halt_report(s);
    } else if (s->slave.cycles == 0) {
        /* Died on its first instruction: the master had not finished storing
         * the routine yet. Re-arm from the registered entry. Restarting a slave
         * that has actually run is wrong -- the game tears that region down when
         * it is finished with it, and re-entering just faults forever. */
        s->slave_started = 0;
        s->slave.halted  = 0;
        s->slave_restarts++;
    }
}

static int prof_on = -1;
static int profiling(void)
{
    if (prof_on < 0) prof_on = getenv("SATURN_PROF") != NULL;
    return prof_on;
}

/* VDP1 samples the erase/swap request at the field-change point in the left
 * border of the first visible scanline.  This is deliberately separate from
 * V-Blank-IN: the US BIOS writes FBCR=3 exactly at V-Blank-OUT and restores
 * FBCR=0 only 256 clocks later.  Sampling at V-Blank-IN therefore sees the
 * old value and skips the short manual swap which starts the IPL shard list. */
static void vdp1_field_change(saturn *s)
{
    uint64_t tv = profiling() ? __rdtsc() : 0;
    int erase = 0, swap = 0;

    /* Ymir vdp.cpp BeginHPhaseLeftBorder and the VDP1 timing notes make one
     * decision per field from FCM/FCT plus the sticky "FBCR was written"
     * latch. */
    if (!(s->vdp1_reg[1] & 2u)) {       /* FCM = 0: automatic */
        erase = 1;
        swap = 1;
    } else if (s->vdp1_fbparams) {      /* FCM = 1: manual */
        if (s->vdp1_reg[1] & 1u) swap = 1;
        else                     erase = 1;
    }

    { static int lg = -1; static unsigned long long n;
      if (lg < 0) lg = getenv("SATURN_FBLOG") ? 1 : 0;
      if (lg && ++n <= 100000u)
          printf("[fbdec] FBCR=%04X FCM=%d FCT=%d written=%d -> "
                 "erase=%d swap=%d clk=%llu\n",
                 s->vdp1_reg[1], (s->vdp1_reg[1] >> 1) & 1,
                 s->vdp1_reg[1] & 1, s->vdp1_fbparams, erase, swap,
                 (unsigned long long)s->clk); }
    s->vdp1_fbparams = 0;

    /* Ymir performs the display erase after VDP2 consumes the field.  With
     * this whole-field compositor, retaining it until the next field change
     * clears the buffer which has finished being displayed without erasing
     * the just-rendered draw target. */
    if (s->vdp1_erase_pending) {
        s->vdp1_erase_pending = 0;
        vdp1_erase(s);
    }
    s->vdp1_show_interp = swap ? 1 : 0;
    if (swap)  vdp1_swap(s);
    if (erase) s->vdp1_erase_pending = 1;
    if (swap && (s->vdp1_reg[2] & 2u)) vdp1_begin_frame(s);

    if (profiling()) s->prof_video += __rdtsc() - tv;
}

uint64_t saturn_run_line(saturn *s)
{
    static uint32_t quantum_cached;
    uint32_t quantum;
    if (!quantum_cached) {
        const char *e = getenv("SATURN_SLICE");
        /* 128, not 512. The optimisation pass raised this to 512 claiming the
         * master/slave rendezvous "passes through a full-line slice" -- it
         * does not: at 512 Sonic 3D Blast's TrueMotion FMV never leaves its
         * load phase (measured: black from 1.04e9 on; at 128 the movie plays).
         * The MINIT/SINIT + FRT-ICI handshake needs the finer interleave.
         * SATURN_SLICE overrides for experiments. */
        quantum_cached = e ? (uint32_t)strtoul(e, NULL, 0) : 128u;
        if (!quantum_cached) quantum_cached = 128u;
    }
    quantum = quantum_cached;
    uint64_t done = 0;
    uint32_t pos  = 0;
    int      hb   = 0;
    int      field_change = 0;

    if (quantum == 0 || quantum > CYC_PER_LINE) quantum = CYC_PER_LINE;

    while (pos < CYC_PER_LINE) {
        uint32_t n = CYC_PER_LINE - pos;
        if (n > quantum) n = quantum;
        /* Never step over the H-Blank edge: it must fall at the same clock for
         * both cores, or they disagree about where in the line they are. */
        if (!hb && pos < HBLANK_START && pos + n > HBLANK_START)
            n = HBLANK_START - pos;
        /* Do not step over VDP1's field-change sampling point.  The BIOS's
         * FBCR=3 pulse is only 256 clocks wide, so a coarser scheduler slice
         * must still expose the state halfway through that pulse. */
        if (s->line == 0 && !field_change && pos < 128u && pos + n > 128u)
            n = 128u - pos;

        if (profiling()) {
            uint64_t t0 = __rdtsc(), t1, t2;
            s->cur = &s->master;
            if (!s->master.halted) done += sh2_run(&s->master, n);
            t1 = __rdtsc();
            run_slave(s, n);
            t2 = __rdtsc();
            s->prof_master += t1 - t0;
            s->prof_slave  += t2 - t1;
        } else {
            s->cur = &s->master;
            if (!s->master.halted) done += sh2_run(&s->master, n);
            run_slave(s, n);
        }

        pos    += n;
        s->clk += n;
        smpc_tick(s);
        frt_advance(&s->master, n);
        if (s->slave_started) frt_advance(&s->slave, n);
        scu_dsp_tick(s, n);
        if (profiling()) {
            uint64_t tv1 = __rdtsc();
            vdp1_tick(s, n);
            s->prof_vdp1 += __rdtsc() - tv1;
        } else {
            vdp1_tick(s, n);
        }
        if (s->line == 0 && !field_change && pos >= 128u) {
            field_change = 1;
            vdp1_field_change(s);
        }
        /* The sound side advances against the same clock as everything else,
         * so a driver that paces itself off SCSP timers stays in step with
         * the video field it is scoring. The optimisation pass hoisted this
         * to once per scanline ("identical totals"); totals are identical but
         * the PHASE is not, and the 68000/SH-2 handshakes interleave within a
         * line. Kept per-slice until the movie-stall bisect clears it. */
        if (profiling()) {
            uint64_t ts = __rdtsc();
            sound_run(s, n);
            s->prof_other += __rdtsc() - ts;
        } else {
            sound_run(s, n);
        }
        if (!hb && pos >= HBLANK_START) {
            hb = 1;
            s->vdp2_reg[0x04 >> 1] |= 0x0004u;          /* TVSTAT HBLANK */
            scu_dma_trigger(s, 2);
            /* Input capture is MINIT/SINIT-driven (see frt_capture); the
             * per-scanline calls that lived here were the interrupt storm. */
            /* Resolved once. This was a getenv per SCANLINE -- ~15,800 walks
             * of the environment block every emulated second, on the hottest
             * loop in the scheduler. The environment cannot change under us. */
            {
                static int nohbi = -1;
                if (nohbi < 0) nohbi = getenv("SATURN_NOHBI") != NULL;
                if (!nohbi) scu_raise(s, 2);                   /* H-Blank-IN */
            }
        }
        if (s->master.halted) break;
    }
    s->vdp2_reg[0x04 >> 1] &= (uint16_t)~0x0004u;
    return done;
}

uint64_t saturn_run_field(saturn *s)
{
    uint64_t done = 0;
    int visible_started = 0;

    /* SATURN_POKE: apply any timed writes whose cycle has arrived. */
    if (s->npoke) {
        int q;
        for (q = 0; q < s->npoke; q++) {
            if (s->poke[q].cy && s->master.cycles >= s->poke[q].cy) {
                bus_w32(s, s->poke[q].addr, s->poke[q].val);
                printf("[poke] %08X = %08X at cy %llu\n", s->poke[q].addr,
                       s->poke[q].val, (unsigned long long)s->master.cycles);
                /* A 4th field makes it repeat: a per-frame "ready" flag has
                 * to be re-set every field, not once. */
                if (s->poke[q].period) s->poke[q].cy += s->poke[q].period;
                else                   s->poke[q].cy = 0;
            }
        }
    }

    /* A frontend frame ends at the instant VDP2 enters vertical blanking.
     * That is the point at which the just-scanned field is complete and is
     * exactly where Ymir's RunFrame returns.  The old loop always ran the
     * entire following V-Blank interval too, then composited from whichever
     * VDP2 registers the game happened to leave immediately before line 0.
     * Sonic 3D Blast changes SFPRMD in its V-Blank-OUT handler; consequently
     * the VDP1 framebuffer for one field was paired with the previous field's
     * priority mode and Sonic appeared above foreground tiles for a frame.
     *
     * Keep the scanline persistent across calls.  When this call reaches
     * V-Blank-IN after visible scanout, assert the edge and return before any
     * SH-2 executes the blanking handler.  The boundary-done latch lets the
     * next call resume on that same scanline without raising the edge twice. */
    for (;;) {
        if (!s->vblank_boundary_done && s->line == 0) {
            s->vdp2_reg[0x04 >> 1] &= (uint16_t)~0x0008u;   /* TVSTAT VBLANK=0 */
            scu_raise(s, 1);                                /* V-Blank-OUT     */
            scu_dma_trigger(s, 1);
            scu_timer_vblank_out(s);       /* T0 counter resets here */
            visible_started = 1;
            s->vblank_boundary_done = 1;
        } else if (!s->vblank_boundary_done && s->line == LINE_VBLANK) {
            s->vdp2_reg[0x04 >> 1] |= 0x0008u;              /* TVSTAT VBLANK=1 */
            /* TVSTAT bit 1 (ODD), the field flag, switched at V-Blank IN --
             * exactly where Ymir does it (vdp.cpp ~803). TVMD bits 7-6 (LSMD)
             * select the interlace mode: when interlaced the flag alternates
             * each field, and when it is NOT interlaced the flag is held at 1
             * permanently. It is NOT "0 because there are no fields".
             *
             * This bit was never maintained at all, so it read 0 forever.
             * Sonic 3D Blast's movie player spins on precisely it --
             *     0606339E  mov.w @r2,r0     (r2 = 0x25F80004, TVSTAT)
             *     060633A0  tst  #2,r0
             *     060633A2  bt   0x606339e
             * -- looping while the bit is clear, so in progressive mode, where
             * hardware pins it to 1, the player waited forever and submitted
             * empty VDP1 lists. */
            if ((s->vdp2_reg[0x00 >> 1] >> 6) & 3u)
                s->vdp2_reg[0x04 >> 1] ^= 0x0002u;          /* interlaced      */
            else
                s->vdp2_reg[0x04 >> 1] |= 0x0002u;          /* progressive: 1  */
            scu_raise(s, 0);                                /* V-Blank-IN      */
            scu_dma_trigger(s, 0);
            if (profiling()) {
                uint64_t tc = __rdtsc();
                cdb_tick(s);
                s->prof_other += __rdtsc() - tc;
            } else {
                cdb_tick(s);
            }
            /* Sound-driver handshake HLE. The BIOS (and the Sega sound
             * driver protocol generally) posts a command byte at sound RAM
             * 0x700 and busy-waits for the MC68000 to clear it -- inside its
             * V-Blank-OUT handler, with every other interrupt masked, so an
             * unacknowledged command wedges the whole boot (measured: the
             * SEGA logo froze at frame 58 of a 130-frame gate with the
             * handler spinning at 0x06012FD2). There is no 68000 here;
             * acknowledge once per field, which is far slower than the real
             * chip and safely in-order. */
            /* Ymir has no stand-in here at all: its 68000 runs the driver and
             * the driver clears the byte. Ours does too, when it is running
             * and the driver is one it can execute. This is the FALLBACK for
             * when it is not -- held in reset, disabled, or halted on an
             * opcode we don't cover. Clearing unconditionally would race a
             * working driver's own state machine. */
            if (s->sound_ram[0x700] &&
                (!s->sound_on || s->sound_cpu.halted || snd_disabled(s)))
                s->sound_ram[0x700] = 0;
            s->vblank_boundary_done = 1;
            if (visible_started) break;
        }
        /* One H-Blank IN per line. This has to sit OUTSIDE the
         * line==0 / line==LINE_VBLANK branches: Timer 0 compares against a
         * per-LINE counter, so ticking it once per field (as the old Timer 0
         * hack did) can never match any compare except 0. */
        scu_timer_hblank(s);
        done += saturn_run_line(s);
        if (s->master.halted) break;
        s->line++;
        if (s->line >= LINES_TOTAL) s->line = 0;
        s->vblank_boundary_done = 0;
    }
    /* SATURN_V2PEEK=N -- report VDP2 VRAM occupancy every N fields. An
     * end-of-run dump cannot tell "the game never uploaded anything" from
     * "it uploaded and then cleared it again", and those need opposite fixes. */
    {
        static long long peek = -2;
        if (peek == -2) {
            const char *e = getenv("SATURN_V2PEEK");
            peek = e ? atoll(e) : -1;
            if (peek == 0) peek = 1;
        }
        if (peek > 0 && (s->frames % (uint64_t)peek) == 0) {
            uint32_t i, nz = 0, v1 = 0;
            for (i = 0; i < VDP2_VRAM_SZ; i++) if (s->vdp2_vram[i]) nz++;
            for (i = 0; i < VDP1_VRAM_SZ; i++) if (s->vdp1_vram[i]) v1++;
            printf("[v2peek] f%llu v2=%5.1f%% v1=%5.1f%%  mpc=%08X sr=%03X "
                   "spc=%08X tvmd=%04X bgon=%04X halt=%d\n",
                   (unsigned long long)s->frames,
                   100.0 * nz / (double)VDP2_VRAM_SZ,
                   100.0 * v1 / (double)VDP1_VRAM_SZ,
                   s->master.pc, s->master.sr & 0xFFFu, s->slave.pc,
                   s->vdp2_reg[0], s->vdp2_reg[0x20 >> 1], s->master.halted);
            fflush(stdout);
        }
    }
    s->frames++;
    return done;
}

int scu_pending(saturn *s, uint32_t sr, int *vector, int *level)
{
    uint32_t mask = s->scu_reg[0xA0 >> 2];   /* 1 = masked */
    int cpu_level = (int)((sr >> 4) & 0xF);

    for (int b = 0; b < 14; b++) {
        if (!(s->scu_ipend & (1u << b))) continue;
        if (mask & (1u << b))             continue;
        if (scu_irq[b].level <= cpu_level) continue;
        *vector = scu_irq[b].vector;
        *level  = scu_irq[b].level;
        return b;
    }

    /* External interrupts. The SCU masks the whole A-Bus group with a single
     * bit (16) rather than one bit per source. */
    if (!(mask & (1u << 16))) {
        for (int b = 16; b < 32; b++) {
            const int e = b - 16;
            if (!(s->scu_ipend & (1u << b)))       continue;
            if (scu_irq_ext[e].level <= cpu_level) continue;
            *vector = scu_irq_ext[e].vector;
            *level  = scu_irq_ext[e].level;
            return b;
        }
    }
    return -1;
}

/* -------------------------------------------------------------- CD block --
 * Not implemented yet -- this records the command stream so the real
 * implementation can be written against what the BIOS and game actually ask
 * for, rather than against the whole 41-command surface speculatively.
 *
 * Command packet is CR1..CR4; the opcode is the high byte of CR1.
 */
void cdb_report(saturn *s, FILE *out)
{
    static const struct { uint8_t op; const char *name; } known[] = {
        { 0x00, "GetStatus" },          { 0x01, "GetHardwareInfo" },
        { 0x02, "GetTOC" },             { 0x03, "GetSessionInfo" },
        { 0x04, "InitializeCDSystem" }, { 0x06, "EndDataTransfer" },
        { 0x10, "PlayDisc" },           { 0x11, "SeekDisc" },
        { 0x30, "SetDeviceConnection" },{ 0x48, "ResetSelector" },
        { 0x60, "SetSectorLength" },    { 0x61, "GetSectorData" },
        { 0x62, "GetSectorData" },      { 0x63, "DeleteSectorData" },
        { 0x67, "GetCopyError" },       { 0x70, "ChangeDirectory" },
        { 0x71, "ReadDirectory" },      { 0x72, "GetFileSystemScope" },
        { 0x73, "GetFileInfo" },        { 0x74, "ReadFile" },
        { 0x75, "AbortFile" },          { 0xE0, "AuthenticateDevice" },
        { 0xE1, "IsDeviceAuthenticated" }, { 0x00, NULL }
    };
    if (!s->ncdcmd) { fprintf(out, "\nCD block: no commands issued\n"); return; }
    fprintf(out, "\nCD block commands issued (in order first seen)\n");
    fprintf(out, "%-5s %-24s %-7s %s\n", "OP", "NAME", "COUNT", "CR1..CR4 (first)");
    for (int i = 0; i < s->ncdcmd; i++) {
        const char *nm = "?";
        for (int k = 0; known[k].name; k++)
            if (known[k].op == s->cdcmd[i].op) { nm = known[k].name; break; }
        fprintf(out, "0x%02X  %-24s %-7u %04X %04X %04X %04X\n",
                s->cdcmd[i].op, nm, s->cdcmd[i].count,
                s->cdcmd[i].cr1, s->cdcmd[i].cr2,
                s->cdcmd[i].cr3, s->cdcmd[i].cr4);
    }
}

/* ---------------------------------------------------------- SCU DMA ----
 * Three channels at 0x25FE0000 (L0), 0x25FE0020 (L1), 0x25FE0040 (L2), each a
 * 24-byte block: read addr, write addr, byte count, address-add values,
 * enable, mode. See docs/HARDWARE.md §4.2.
 *
 * This is distinct from, and far more used than, the SH-2's own on-chip DMAC:
 * NiGHTS programs level 0 twice per frame to push its VDP1 command list and
 * VDP2 tables across the B-Bus. With it unimplemented the game runs its whole
 * main loop and draws nothing, because nothing it "uploads" ever arrives.
 *
 * Timing note: transfers complete instantly here. Correct in result, wrong in
 * duration; the scheduler will refine that later.
 */
static uint32_t scu_wr_stride(uint32_t ad)
{
    switch (ad & 7u) {
    case 0: return 0;   case 1: return 2;   case 2: return 4;   case 3: return 8;
    case 4: return 16;  case 5: return 32;  case 6: return 64;  default: return 128;
    }
}

static void scu_dma_copy(saturn *s, uint32_t src, uint32_t dst,
                         uint32_t bytes, uint32_t rd_step, uint32_t wr_step)
{
    /* The SCU moves a CONTIGUOUS byte stream. The source is read through a
     * 32-bit buffer whose address only advances once that buffer is consumed,
     * and the destination is written unit by unit with the offset advancing by
     * the unit size (Ymir scu.cpp doRead / the dst walker). The address-
     * increment fields say whether an address advances AT ALL -- zero means a
     * fixed FIFO-style port -- not how far apart the data lands.
     *
     * Advancing dst by wr_step while writing four bytes made every write
     * overlap its successor, so each destination longword kept the LOW half of
     * the previous read. NiGHTS DMAs a 288-byte VDP2 shadow register table to
     * 0x25F80000 every frame; the overlap shifted it by one word, TVMD came
     * out 0x0000 instead of 0x8101, and the display bit never got set -- a
     * black screen with a perfectly healthy game running behind it. */
    const int fixed_src = (rd_step == 0);
    const int fixed_dst = (wr_step == 0);
    /* B-Bus targets (VDP1/VDP2/SCSP registers) accept 16-bit units. */
    const uint32_t unit = (wr_step == 2u) ? 2u : 4u;

    while (bytes >= unit) {
        if (unit == 2u) bus_w16(s, dst, bus_r16(s, src));
        else            bus_w32(s, dst, bus_r32(s, src));
        if (!fixed_src) src += unit;
        if (!fixed_dst) dst += unit;
        bytes -= unit;
    }
    while (bytes >= 2) {
        bus_w16(s, dst, bus_r16(s, src));
        if (!fixed_src) src += 2;
        if (!fixed_dst) dst += 2;
        bytes -= 2;
    }
    if (bytes) bus_w8(s, dst, bus_r8(s, src));
}

static void scu_dma_run(saturn *s, int level)
{
    uint32_t base = (uint32_t)level * 8u;     /* 0x20 bytes / 4 = 8 longwords */
    uint32_t rd   = s->scu_reg[base + 0] & 0x07FFFFFFu;
    uint32_t wr   = s->scu_reg[base + 1] & 0x07FFFFFFu;
    uint32_t cnt  = s->scu_reg[base + 2];
    uint32_t ad   = s->scu_reg[base + 3];
    uint32_t md   = s->scu_reg[base + 5];
    uint32_t rd_step = (ad & 0x100u) ? 4u : 0u;
    uint32_t wr_step = scu_wr_stride(ad);

    if (s->ndmalog < 16) {
        s->dmalog[s->ndmalog].src = rd;  s->dmalog[s->ndmalog].dst = wr;
        s->dmalog[s->ndmalog].cnt = cnt; s->dmalog[s->ndmalog].md  = md;
        s->dmalog[s->ndmalog].ad  = ad;  s->ndmalog++;
    }

    /* Level 0 carries a 20-bit count; levels 1-2 only 12. */
    cnt &= (level == 0) ? 0x000FFFFFu : 0x00000FFFu;
    if (cnt == 0) cnt = (level == 0) ? 0x00100000u : 0x00001000u;

    /* DxMD bit 24 selects indirect mode. Bits 0-2 are the start factor --
     * testing bit 0 for "indirect" misreads every start-factor-1 transfer as a
     * table walk, which runs away into the guard limit. */
    if (md & 0x01000000u) {
        /* Indirect: `wr` points at a table of {count, write addr, read addr}
         * triplets, terminated by bit 31 of the read address. */
        uint32_t tbl = wr;
        for (int guard = 0; guard < 4096; guard++) {
            uint32_t n = bus_r32(s, tbl + 0);
            uint32_t w = bus_r32(s, tbl + 4);
            uint32_t r = bus_r32(s, tbl + 8);
            uint32_t last = r & 0x80000000u;
            r &= 0x07FFFFFFu;
            w &= 0x07FFFFFFu;
            n &= 0x000FFFFFu;
            if (n) scu_dma_copy(s, r, w, n, rd_step ? 4u : 0u,
                                wr_step ? wr_step : 4u);
            s->scu_dma_transfers++;
            s->scu_dma_bytes += n;
            tbl += 12;
            if (last) break;
        }
    } else {
        scu_dma_copy(s, rd, wr, cnt, rd_step ? 4u : 0u, wr_step ? wr_step : 4u);
        s->scu_dma_transfers++;
        s->scu_dma_bytes += cnt;
    }

    /* Completion: clear enable, flag it in the DMA status register, and raise
     * the matching Level-N DMA End interrupt. */
    s->scu_reg[base + 4] = 0;
    s->scu_reg[0x7C >> 2] = 0;                 /* all channels idle */
    scu_raise(s, level == 0 ? 11 : level == 1 ? 10 : 9);
}

/* SATURN_MEMDUMP=addr,len,file, performed NOW rather than at exit.
 * Split out of the end-of-run path because a routine that is spinning in an
 * overlay gets OVERWRITTEN before the run ends: dumping 0x06012C40 at exit
 * read back as all zeroes, so the loop burning 75% of the CPU could not be
 * disassembled at all. Pair with SATURN_DUMPAT=<pc> to capture the moment. */
/* `at_hit` marks a SATURN_DUMPAT capture taken WHILE a PC executes, as opposed
 * to the ordinary end-of-run SATURN_MEMDUMP. They used to share a filename and
 * message, so a dump of a transient overlay looked identical to the exit dump
 * where that overlay is long gone -- which produced two confident but wrong
 * conclusions about code "reading as zeros". Hit captures get their own file. */
void mem_dump_at(saturn *s, int at_hit)
{
    char mb[256];
    unsigned long ma = 0, ml = 0;
    char mf[128] = "memdump.bin";
    char *c1, *c2;
    if (!getenv("SATURN_MEMDUMP")) return;
    strncpy(mb, getenv("SATURN_MEMDUMP"), sizeof(mb) - 1);
    mb[sizeof(mb) - 1] = 0;
    c1 = strchr(mb, ',');
    if (!c1) return;
    *c1++ = 0;
    c2 = strchr(c1, ',');
    if (c2) { *c2++ = 0; strncpy(mf, c2, sizeof(mf) - 1); mf[sizeof(mf)-1] = 0; }
    if (at_hit) {
        size_t n2 = strlen(mf);
        if (n2 + 4 < sizeof(mf)) memcpy(mf + n2, ".at", 4);
    }
    ma = strtoul(mb, NULL, 16);
    ml = strtoul(c1, NULL, 0);
    if (!ml) return;
    {
        FILE *mfp = fopen(mf, "wb");
        if (mfp) {
            for (unsigned long k = 0; k < ml; k++)
                fputc(bus_r8(s, (uint32_t)(ma + k)), mfp);
            fclose(mfp);
            printf("%s: %lu bytes from 0x%08lX -> %s\n",
                   at_hit ? "memdump-AT-HIT" : "memdump-at-exit",
                   ml, ma, mf);
        }
    }
}

void mem_dump_now(saturn *s) { mem_dump_at(s, 0); }
