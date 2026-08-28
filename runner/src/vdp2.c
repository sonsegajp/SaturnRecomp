/* vdp2.c — VDP2 background rendering.
 *
 * VDP2 has no framebuffer: it generates each output pixel on the fly from up to
 * four normal scroll planes (NBG0-3), two rotation planes, and a back screen,
 * then composites the VDP1 framebuffer in by priority. See docs/HARDWARE.md §6.
 *
 * A plane is either CELL format (8x8 cells -> character patterns -> pages ->
 * planes, indexed through colour RAM) or BITMAP format (a flat block of pixels
 * in VRAM). NiGHTS configures NBG0 and NBG1 as 512x256 RGB555 bitmaps --
 * CHCTLA = 0x3332 sets BMEN and a colour count of 3 for both -- so bitmap is
 * the path that matters first.
 */
#include "saturn.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define VR(off)   (s->vdp2_reg[(off) >> 1])

static uint16_t vram16(saturn *s, uint32_t a)
{
    a &= (VDP2_VRAM_SZ - 2);
    return (uint16_t)((s->vdp2_vram[a] << 8) | s->vdp2_vram[a + 1]);
}

/* Colour RAM lookup. RAMCTL bits 13-12 pick the mode: 0/1 are RGB555 tables of
 * 1024/2048 entries, 2 is a 1024-entry RGB888 table. */
static uint32_t cram_colour(saturn *s, uint32_t index)
{
    unsigned mode = (VR(0x0E) >> 12) & 3u;
    /* The wrap is PER MODE, not the size of our CRAM array (Ymir
     * VDP2FetchCRAMColor): mode 0 is 1024 words and wraps at 0x7FE, mode 1 is
     * 2048 words and wraps at 0xFFE, mode 2 is 1024 longwords and wraps at
     * 0xFFC. Masking everything with the full 4 KB let a mode-0 index past
     * 1024 words read colours the hardware would never return -- one object
     * comes out the wrong colour while the rest of the scene looks right. */
    if (mode == 2) {
        uint32_t o = (index * 4u) & 0xFFCu;
        /* Mode-2 entries are 0x00BBGGRR (blue in the HIGH byte), so the
         * channels must swap on the way to ARGB. Straight copy painted the
         * player's blue nebula red and its gold cubes green. */
        return 0xFF000000u | ((uint32_t)s->cram[o + 3] << 16) |
               ((uint32_t)s->cram[o + 2] << 8) | s->cram[o + 1];
    } else {
        uint32_t o = (index * 2u) & (mode == 0 ? 0x7FEu : 0xFFEu);
        uint16_t c = (uint16_t)((s->cram[o] << 8) | s->cram[o + 1]);
        uint32_t r = c & 0x1F, g = (c >> 5) & 0x1F, b = (c >> 10) & 0x1F;
        return 0xFF000000u | (((r << 3) | (r >> 2)) << 16)
                           | (((g << 3) | (g >> 2)) << 8)
                           |  ((b << 3) | (b >> 2));
    }
}

/* The MSB of a colour RAM entry. Sprite colour calculation condition 3 tests
 * it per pixel (Ymir: `pixels.color[x].msb == 1`), so it has to survive the
 * RGB555 -> ARGB8888 conversion, which drops it. */
static int cram_msb(saturn *s, uint32_t index)
{
    unsigned mode = (VR(0x0E) >> 12) & 3u;
    uint32_t o = (mode == 2) ? ((index * 4u) & (CRAM_SIZE - 4))
                             : ((index * 2u) & (CRAM_SIZE - 2));
    return (s->cram[o] >> 7) & 1;      /* big-endian: bit 15 is byte 0 bit 7 */
}

/* btm + (top - btm) * topw / 32, per channel (Ymir Color888CompositeRatio).
 * topw 31 is (nearly) opaque top, 0 is (nearly) pure bottom. */
static uint32_t cc_blend(uint32_t top, uint32_t btm, unsigned topw, int add)
{
    uint32_t r, g, b;
    if (add) {
        r = ((top >> 16) & 0xFF) + ((btm >> 16) & 0xFF);
        g = ((top >>  8) & 0xFF) + ((btm >>  8) & 0xFF);
        b = ( top        & 0xFF) + ( btm        & 0xFF);
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
    } else {
        int tr = (int)((top >> 16) & 0xFF), br = (int)((btm >> 16) & 0xFF);
        int tg = (int)((top >>  8) & 0xFF), bg = (int)((btm >>  8) & 0xFF);
        int tb = (int)( top        & 0xFF), bb = (int)( btm        & 0xFF);
        r = (uint32_t)(br + (((tr - br) * (int)topw) >> 5));
        g = (uint32_t)(bg + (((tg - bg) * (int)topw) >> 5));
        b = (uint32_t)(bb + (((tb - bb) * (int)topw) >> 5));
    }
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static uint32_t rgb555(uint16_t p)
{
    uint32_t r = p & 0x1F, g = (p >> 5) & 0x1F, b = (p >> 10) & 0x1F;
    return 0xFF000000u | (((r << 3) | (r >> 2)) << 16)
                       | (((g << 3) | (g >> 2)) << 8)
                       |  ((b << 3) | (b >> 2));
}

/* Colour offset (VDP2 manual s10.6). CLOFEN enables it per layer, CLOFSL picks
 * offset set A or B, and COAR/COAG/COAB (and the B set) hold a 9-bit signed
 * value added to each channel after lookup, clamped to 0..255.
 *
 * The BIOS writes all eight of these registers and we implemented none of them,
 * so every layer rendered at its unoffset value: measured against a reference
 * capture of the boot logo, mean luma 31.2 against 127.2 -- four times too
 * dark. Bit order per layer is NBG0..3, RBG0, back, sprite.
 */
static void colour_offset(saturn *s, int layer, int *dr, int *dg, int *db)
{
    unsigned en = VR(0x110), sl = VR(0x112);
    unsigned base;
    int v;

    *dr = *dg = *db = 0;
    if (layer < 0 || layer > 6) return;
    if (!(en & (1u << layer))) return;

    base = (sl & (1u << layer)) ? 0x11Au : 0x114u;   /* set B : set A */
    /* Each register holds a 9-bit two's complement value in bits 8-0. */
    v = (int)(VR(base)      & 0x1FFu); *dr = (v & 0x100) ? v - 0x200 : v;
    v = (int)(VR(base + 2u) & 0x1FFu); *dg = (v & 0x100) ? v - 0x200 : v;
    v = (int)(VR(base + 4u) & 0x1FFu); *db = (v & 0x100) ? v - 0x200 : v;
}

static uint32_t apply_offset(uint32_t argb, int dr, int dg, int db)
{
    int r, g, b;
    if (!dr && !dg && !db) return argb;
    r = (int)((argb >> 16) & 0xFF) + dr;
    g = (int)((argb >>  8) & 0xFF) + dg;
    b = (int)( argb        & 0xFF) + db;
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* Per-layer configuration decoded from the register file. */
typedef struct {
    int      enabled;
    int      transparent;   /* BGON TP enable (the register bit is inverted) */
    int      bitmap;
    unsigned colours;     /* 0=16, 1=256, 2=2048, 3=32768, 4=16.7M */
    uint32_t base;        /* VRAM byte offset of the bitmap / pattern data */
    int      bw, bh;      /* bitmap dimensions */
    int      pitch;       /* dots per LINE for addressing (normally == bw) */
    int      lsc_on;      /* line scroll active for this layer */
    const int32_t *ls_x;  /* per-line horizontal scroll, 11.8 fixed */
    const int32_t *ls_y;  /* per-line vertical   scroll, 11.8 fixed */
    const int32_t *ls_z;  /* per-line horizontal zoom, 3.8 fixed */
    int32_t  scroll_fx;   /* SCXIN/SCXDN combined, 11.8 fixed */
    int32_t  scroll_fy;   /* SCYIN/SCYDN combined, 11.8 fixed */
    int32_t  zoom_h;      /* ZMXN coordinate increment, 3.8 fixed (0x100 = 1.0) */
    int32_t  zoom_v;      /* ZMYN coordinate increment, 3.8 fixed */
    int      bw_mask, bh_mask;   /* both are 512/1024 and 256/512: masks    */
    int      scroll_x, scroll_y;
    unsigned palbank, caos;   /* BMPNA palette bank; CRAOFA colour-RAM offset */
    int      sp_bit;           /* BMPNA supplementary special-priority flag */
    int      sp_code;          /* palette colour bits 3-1 at the sampled dot */
} nbg_cfg;

/* ------------------------------------------------ frame interpolation ----
 * Sprites are interpolated by VDP1 (see vdp1_build_interp); the BACKGROUNDS
 * have to move with them or the picture comes apart -- quads gliding at 60 Hz
 * over layers that jump at 30 Hz looks worse than no interpolation at all.
 *
 * Scroll needs no matching: it is the same handful of registers every frame,
 * so the midpoint is just the average of this frame's value and the last one.
 * Values are latched on real frames and averaged on synthesized ones. */
static int32_t interp_sx[5], interp_sy[5];   /* previous real frame */
static int32_t interp_cx[5], interp_cy[5];   /* the one before that  */

static void interp_scroll(saturn *s, int n, int32_t *fx, int32_t *fy)
{
    if (n < 0 || n >= 5) return;
    if (s->vdp1_show_interp && s->vdp1_interp_ready) {
        *fx = (interp_sx[n] + *fx) / 2;
        *fy = (interp_sy[n] + *fy) / 2;
        return;
    }
    /* Real frame: remember where this layer was, for the next midpoint. */
    interp_cx[n] = interp_sx[n]; interp_cy[n] = interp_sy[n];
    interp_sx[n] = *fx;          interp_sy[n] = *fy;
}

static uint32_t vram32(saturn *s, uint32_t a);
static void line_scroll_build(saturn *s, int n, nbg_cfg *c);

static void decode_nbg(saturn *s, int n, nbg_cfg *c)
{
    uint16_t bgon   = VR(0x20);
    uint16_t chctla = VR(0x28);
    uint16_t chctlb = VR(0x2A);
    uint16_t mpofn  = VR(0x3C);
    unsigned shift, mp;
    uint16_t ch;

    memset(c, 0, sizeof(*c));
    c->enabled = (bgon >> n) & 1u;
    /* BGON bits 8-11 are NBG0-3 transparency DISABLE bits. Ymir stores
     * enableTransparency = !bit and only treats colour index zero / a clear
     * RGB MSB as transparent when that flag is enabled. Applying transparency
     * unconditionally punched the US BIOS logo background's zero-valued dots
     * through to the black back screen, producing the stray pixels at x=0. */
    c->transparent = ((bgon >> (8u + (unsigned)n)) & 1u) == 0u;
    if (!c->enabled) return;

    /* CHCTLA holds NBG0 in the low byte and NBG1 in the high byte; CHCTLB holds
     * NBG2 and NBG3. Only NBG0/NBG1 can be bitmaps. */
    if (n < 2) {
        ch = (uint16_t)(chctla >> (n * 8));
        c->bitmap  = (ch >> 1) & 1u;
        c->colours = (ch >> 4) & 7u;
        {
            /* Ymir vdp2_defs.hpp UpdateCHCTL:
             *   kBitmapSizesH[] = {512, 512, 1024, 1024}
             *   kBitmapSizesV[] = {256, 512,  256,  512}
             * so BMSZ bit 1 picks the WIDTH and bit 0 picks the HEIGHT. We had
             * the two bits swapped, which turned a 512x512 bitmap (bmsz 1)
             * into 1024x256 and a 1024x256 one (bmsz 2) into 512x512 -- both
             * the wrong line stride AND the wrong wrap mask. The cell_cfg
             * decode further down already had this right; only this path,
             * which is the one bitmap_pixel actually uses, was wrong. */
            unsigned bmsz = (ch >> 2) & 3u;
            c->bw = (bmsz & 2) ? 1024 : 512;
            c->bh = (bmsz & 1) ? 512  : 256;
        }
        c->bw_mask = c->bw - 1;
        c->bh_mask = c->bh - 1;
        /* SATURN_BMPITCH=<dots> -- DIAGNOSTIC ONLY. Overrides the bitmap line
         * pitch to test whether a mismatch between BMSZ's width and the width
         * the game actually packs is the whole story. Not a fix: if this makes
         * a title render, the real question is which register should have
         * produced that width. */
        {
            static int bp = -2;
            if (bp == -2) {
                const char *e = getenv("SATURN_BMPITCH");
                bp = e ? atoi(e) : -1;
            }
            /* Pitch only -- the wrap mask must stay a power of two, or
             * `x & (pitch-1)` stops being a modulo and shreds the sampling. */
            c->pitch = (bp > 0) ? bp : c->bw;
        }
    } else {
        ch = (uint16_t)(chctlb >> ((n - 2) * 8));
        c->bitmap  = 0;
        c->colours = (ch >> 4) & 7u;
    }

    /* Bitmap palette: BMPNA (0x2C) holds the palette bank per bitmap layer
     * (3 bits at an 8-bit stride), and CRAOFA applies on top exactly as it
     * does for cells. The bitmap path used neither. */
    if (n < 2) {
        unsigned bmpna = (unsigned)(VR(0x2C) >> (n * 8));
        c->palbank = bmpna & 7u;
        c->sp_bit  = (int)((bmpna >> 5) & 1u);
        c->caos    = (unsigned)((VR(0xE4) >> ((unsigned)n * 4u)) & 7u);
    }

    /* Map offset register gives three bits per layer; for a bitmap it selects
     * the 0x20000-byte block the pixels start in. */
    /* MPOFN packs each layer at a FOUR-bit stride (N0 bits 2-0, N1 bits 6-4,
     * N2 bits 10-8, N3 bits 14-12 -- Ymir vdp2_regs.hpp WriteMPOFN). The old
     * n*3 shift sent NBG1's bitmap to NBG2's block: the Set Language panels
     * were in VRAM at 0x20000 the whole time, sampled from 0x40000. */
    shift = (unsigned)n * 4u;
    mp = (mpofn >> shift) & 7u;
    c->base = mp * 0x20000u;

    /* Integer scroll: SCXIN0 at 0x70, then 0x08 per layer. */
    /* Scroll register layout is NOT a uniform stride (Ymir vdp2_regs.hpp):
     * NBG0/NBG1 have fractional parts (SCXIN0 0x70, SCXIN1 0x80), NBG2/NBG3
     * integer only at 0x90/0x94. The old 0x70+n*8 read NBG1's scroll from
     * NBG0's ZOOM register and NBG2's from NBG1's scroll. */
    {
        static const uint8_t scx[4] = { 0x70, 0x80, 0x90, 0x94 };
        static const uint8_t scy[4] = { 0x74, 0x84, 0x92, 0x96 };
        c->scroll_x = (int)(VR(scx[n]) & 0x07FF);
        c->scroll_y = (int)(VR(scy[n]) & 0x07FF);
        /* NBG0/NBG1 carry a fractional part in the next register along
         * (SCXDN0 0x72, SCXDN1 0x82); NBG2/3 are integer only. */
        if (n < 2) {
            c->scroll_fx = (int32_t)(((uint32_t)(VR(scx[n]) & 0x07FFu) << 8)
                                   | (uint32_t)(VR((uint8_t)(scx[n] + 2)) >> 8));
            c->scroll_fy = (int32_t)(((uint32_t)(VR(scy[n]) & 0x07FFu) << 8)
                                   | (uint32_t)(VR((uint8_t)(scy[n] + 2)) >> 8));
        } else {
            c->scroll_fx = (int32_t)((uint32_t)c->scroll_x << 8);
            c->scroll_fy = (int32_t)((uint32_t)c->scroll_y << 8);
        }
        /* Coordinate increment (zoom). ZMXIN/ZMYIN hold the INTEGER part in
         * bits 2-0 and ZMXDN/ZMYDN the fraction in bits 15-8, i.e. a 3.8
         * fixed-point step per screen dot -- Ymir keeps it as scrollIncH and
         * reads it back with bit::extract<8,10>. Only NBG0/NBG1 can zoom.
         * We had NO zoom at all: every layer advanced exactly one source dot
         * per screen dot, so any reduced or magnified background was drawn at
         * 1:1. A register value of 0 means the game never set it; treat that
         * as 1.0 so untouched layers are unaffected. */
        if (n < 2) {
            static const uint8_t zmx[2] = { 0x78, 0x88 };
            static const uint8_t zmy[2] = { 0x7C, 0x8C };
            c->zoom_h = (int32_t)((((uint32_t)VR(zmx[n]) & 7u) << 8)
                                | ((uint32_t)VR((uint8_t)(zmx[n] + 2)) >> 8));
            c->zoom_v = (int32_t)((((uint32_t)VR(zmy[n]) & 7u) << 8)
                                | ((uint32_t)VR((uint8_t)(zmy[n] + 2)) >> 8));
        }
        if (c->zoom_h <= 0) c->zoom_h = 0x100;
        if (c->zoom_v <= 0) c->zoom_v = 0x100;
        interp_scroll(s, n, &c->scroll_fx, &c->scroll_fy);
    }

    line_scroll_build(s, n, c);
}

/* ---- VDP2 line scroll (SCRCTL 0x9A, LSTA0 0xA0/0xA2, LSTA1 0xA4/0xA6) ----
 *
 * NBG0 and NBG1 can take their scroll from a per-line table in VRAM instead of
 * (on top of) the scroll registers. This was not implemented at all, and it is
 * not a niche feature: NiGHTS' FMV is a 320-dot-wide picture packed into a
 * 512-dot bitmap, and the ONLY thing that makes it read correctly is a line
 * scroll table stepping X by -192 every line (512 - 192 = 320). Without it the
 * movie renders as sheared, vertically-doubled bands. Parallax backgrounds use
 * the same feature.
 *
 * Ymir vdp_renderer_sw.cpp VDP2UpdateLineScreenScroll:
 *   - the table pointer ADVANCES as lines consume entries and persists down
 *     the frame; it is reset to LSTAn at the top of each frame,
 *   - a line only consumes an entry when (y & ((1 << interval) - 1)) == 0,
 *   - entries appear in the order X, Y, zoom -- only for the features that are
 *     ENABLED, so the stride is 4, 8 or 12 bytes, not a fixed 12,
 *   - each value is bits 26-8 of the longword: 11 integer bits, 8 fractional.
 * With line scroll Y off, the vertical position instead accumulates one line
 * per line (scrollIncV, 1.0 in 8.8) exactly as normal scrolling does. */
/* 19-bit 11.8 fixed point, integer part signed. */
static int32_t ls_sext19(int32_t v)
{
    v &= 0x7FFFF;
    return (v & 0x40000) ? (v - 0x80000) : v;
}

#define LS_MAXLINES 512
static int32_t ls_tab_x[2][LS_MAXLINES];
static int32_t ls_tab_y[2][LS_MAXLINES];
static int32_t ls_tab_z[2][LS_MAXLINES];   /* per-line horizontal zoom */

static void line_scroll_build(saturn *s, int n, nbg_cfg *c)
{
    uint16_t scrctl;
    unsigned sh, vcsc, lscx, lscy, lzmx, lss;
    uint32_t addr;
    int y, h;

    c->lsc_on = 0;
    if (n >= 2 || !c->enabled) return;

    scrctl = VR(0x9A);
    sh   = (unsigned)n * 8u;            /* NBG0 in bits 0-5, NBG1 in bits 8-13 */
    vcsc = (scrctl >> (sh + 0)) & 1u;
    lscx = (scrctl >> (sh + 1)) & 1u;
    lscy = (scrctl >> (sh + 2)) & 1u;
    lzmx = (scrctl >> (sh + 3)) & 1u;
    lss  = (scrctl >> (sh + 4)) & 3u;
    (void)vcsc;                          /* vertical CELL scroll: separate table */
    if (!lscx && !lscy && !lzmx) return;

    /* LSTAnU bits 2-0 -> address bits 19-17; LSTAnL bits 15-1 -> bits 16-2. */
    {
        uint16_t u = VR((uint8_t)(0xA0 + n * 4));
        uint16_t l = VR((uint8_t)(0xA2 + n * 4));
        addr = ((uint32_t)(u & 7u) << 17) | ((uint32_t)((l >> 1) & 0x7FFFu) << 2);
    }

    vdp2_display_size(s, &y, &h);        /* y is a scratch width here */
    if (h > LS_MAXLINES) h = LS_MAXLINES;

    for (y = 0; y < h; y++) {
        if ((y & ((1 << lss) - 1)) == 0) {
            if (lscx) {
                uint32_t v = vram32(s, addr); addr += 4;
                ls_tab_x[n][y] = (int32_t)((v >> 8) & 0x7FFFFu);
            }
            if (lscy) {
                uint32_t v = vram32(s, addr); addr += 4;
                ls_tab_y[n][y] = (int32_t)((v >> 8) & 0x7FFFFu);
            } else {
                ls_tab_y[n][y] = (int32_t)((uint32_t)y << 8);
            }
            if (lzmx) {
                /* Line zoom X. Ymir: bgState.scrollIncH = bit::extract<8,18>,
                 * i.e. 11 bits -- the same 3.8 coordinate increment the ZMXN
                 * registers carry, but replaced PER LINE. */
                uint32_t v = vram32(s, addr); addr += 4;
                ls_tab_z[n][y] = (int32_t)((v >> 8) & 0x7FFu);
                if (ls_tab_z[n][y] <= 0) ls_tab_z[n][y] = 0x100;
            } else {
                ls_tab_z[n][y] = c->zoom_h;
            }
        } else {
            ls_tab_z[n][y] = y ? ls_tab_z[n][y - 1] : c->zoom_h;
            ls_tab_x[n][y] = y ? ls_tab_x[n][y - 1] : 0;
            ls_tab_y[n][y] = lscy ? (y ? ls_tab_y[n][y - 1] : 0)
                                  : (int32_t)((uint32_t)y << 8);
        }
        if (!lscx) ls_tab_x[n][y] = 0;
    }
    c->ls_x = ls_tab_x[n];
    c->ls_y = ls_tab_y[n];
    c->ls_z = ls_tab_z[n];
    c->lsc_on = 1;
}

/* Sample one pixel of a bitmap layer. Returns 0 if transparent. */
static uint32_t bitmap_pixel(saturn *s, nbg_cfg *c, int x, int y, int *opaque)
{
    uint32_t off;
    *opaque = 0;
    c->sp_code = -1;

    /* bw/bh are 512 or 1024 and 256 or 512, so the mask IS the remainder --
     * and for a negative operand two's complement AND already produces the
     * positive value the old `% then += bw` fixup arrived at. */
    if (c->lsc_on && y >= 0 && y < LS_MAXLINES) {
        /* Line scroll REPLACES the per-frame scroll accumulation and is then
         * offset by the scroll registers (Ymir: fracScrollX + scrollAmountH).
         * X still advances one source dot per screen dot -- horizontal zoom
         * (ZMXN / line zoom) is not applied yet. */
        /* Do NOT mask X into the row here. The table's X and Y together give a
         * constant +320-dot advance per line, so a line whose X puts the right
         * of the screen past dot 511 is MEANT to carry into the next row --
         * that is how a 320-wide picture lives in a 512-wide bitmap. Masking
         * X per row instead wrapped those columns back to the row's start,
         * which showed up as banding down the right of the movie. Fold X into
         * the linear dot offset and let the row arithmetic carry. */
        /* These are 19-bit 11.8 fixed-point values whose 11-bit integer part
         * is SIGNED. NiGHTS' movie sets SCXIN0 = 0x7F0, which is -16, not
         * +2032. While X was masked into the row that distinction did not
         * matter (+2032 == -16 mod 512); once the offset is allowed to carry
         * across rows it does, and treating it as unsigned shifted the picture
         * six whole rows. */
        int32_t fx = ls_sext19(c->ls_x[y] + c->scroll_fx);
        int32_t fy = ls_sext19(c->ls_y[y] + c->scroll_fy);
        /* One source dot per screen dot only when zoom is 1.0; otherwise the
         * X position advances by zoom_h per screen dot (Ymir adds scrollIncH
         * to fracScrollX inside the pixel loop). */
        int32_t dot = ((fy >> 8) & c->bh_mask) * c->pitch
                    + ((fx + x * c->ls_z[y]) >> 8);
        if (dot < 0) dot = 0;
        x = (int)(dot % c->pitch);
        y = (int)((dot / c->pitch) & c->bh_mask);
    } else {
        /* scroll_fx/scroll_fy, not scroll_x << 8: the fraction is part of the
         * position, and dropping it shifts this layer against one whose
         * fraction happened to be zero. */
        x = (int)((c->scroll_fx + x * c->zoom_h) >> 8) & c->bw_mask;
        y = (int)((c->scroll_fy + y * c->zoom_v) >> 8) & c->bh_mask;
    }

    switch (c->colours) {
    case 0: {   /* 16 colour, 4bpp */
        off = c->base + (uint32_t)(y * c->pitch + x) / 2u;
        { uint8_t b = s->vdp2_vram[off & (VDP2_VRAM_SZ - 1)];
          uint32_t i = (x & 1) ? (b & 0x0F) : (b >> 4);
          if (c->transparent && !i) return 0;
          c->sp_code = (int)((i >> 1) & 7u);
          *opaque = 1;
          return cram_colour(s, (c->caos << 8) + (c->palbank << 4) + i); }
    }
    case 1: {   /* 256 colour, 8bpp */
        off = c->base + (uint32_t)(y * c->pitch + x);
        { uint8_t i = s->vdp2_vram[off & (VDP2_VRAM_SZ - 1)];
          if (c->transparent && !i) return 0;
          c->sp_code = (int)((i >> 1) & 7u);
          *opaque = 1;
          return cram_colour(s, (c->caos << 8) + (c->palbank << 8) + i); }
    }
    case 2: {   /* 2048 colour, 16bpp index */
        off = c->base + (uint32_t)(y * c->pitch + x) * 2u;
        { uint16_t i = vram16(s, off) & 0x07FF;
          if (c->transparent && !i) return 0;
          c->sp_code = (int)((i >> 1) & 7u);
          *opaque = 1;
          return cram_colour(s, i); }
    }
    case 3: {   /* 32768 colour RGB555; bit 15 is the transparency flag */
        off = c->base + (uint32_t)(y * c->pitch + x) * 2u;
        { uint16_t p = vram16(s, off);
          if (c->transparent && !(p & 0x8000)) return 0;
          *opaque = 1;
          return rgb555(p); }
    }
    default: { /* 16.7M colour RGB888: longword per dot, BE [flag][B][G][R] */
        off = c->base + (uint32_t)(y * c->pitch + x) * 4u;
        { uint8_t b0 = s->vdp2_vram[(off + 0) & (VDP2_VRAM_SZ - 1)];
          uint8_t b1 = s->vdp2_vram[(off + 1) & (VDP2_VRAM_SZ - 1)];
          uint8_t b2 = s->vdp2_vram[(off + 2) & (VDP2_VRAM_SZ - 1)];
          uint8_t b3 = s->vdp2_vram[(off + 3) & (VDP2_VRAM_SZ - 1)];
          if (c->transparent && !(b0 & 0x80u)) return 0;
          *opaque = 1;
          return 0xFF000000u | ((uint32_t)b3 << 16) | ((uint32_t)b2 << 8) | b1; }
    }
    }
}

/* Cheap rejection for a fully transparent, unzoomed 8bpp bitmap.  Several
 * Sonic FMV/title states leave NBG0 enabled at priority 2 while its complete
 * visible region contains palette index zero; walking the full bitmap sampler
 * for every output pixel then cannot affect the frame.  Non-empty layers
 * normally return almost immediately, so this is biased toward the win. */
/* SATURN_NOVDP2OPT=1 disables the composite cache, the empty-bitmap layer
 * skip and the sprite early-out in one switch, so a rendering artefact can be
 * attributed to them in a single A/B run. */
static int vdp2_opts_off(void)
{
    static int v = -1;
    if (v < 0) v = getenv("SATURN_NOVDP2OPT") != NULL;
    return v;
}

static int bitmap8_visible(saturn *s, const nbg_cfg *c, int w, int h)
{
    if (vdp2_opts_off()) return 1;
    if (!c->transparent) return 1;   /* index zero is an opaque palette entry */
    if (!c->bitmap || c->colours != 1 || c->lsc_on ||
        c->zoom_h != 0x100 || c->zoom_v != 0x100)
        return 1;
    for (int y = 0; y < h; y++) {
        unsigned sy = (unsigned)(y + c->scroll_y) & (unsigned)c->bh_mask;
        for (int x = 0; x < w; x++) {
            unsigned sx = (unsigned)(x + c->scroll_x) & (unsigned)c->bw_mask;
            uint32_t off = c->base + sy * (unsigned)c->pitch + sx;
            if (s->vdp2_vram[off & (VDP2_VRAM_SZ - 1u)]) return 1;
        }
    }
    return 0;
}

/* Back screen: BKTAU/BKTAL point at a colour (or per-line table) in VRAM. */
static uint32_t back_colour(saturn *s, int y)
{
    uint16_t bktau = VR(0xAC), bktal = VR(0xAE);
    uint32_t addr  = ((((uint32_t)bktau & 7u) << 16) | bktal) * 2u;
    /* Diagnostic override, resolved ONCE: this ran per PIXEL, and getenv on
     * Windows walks the environment block -- 71,680 scans per frame made the
     * compositor cost 1.1e9 cycles a frame on its own. */
    static uint32_t forced = 0xFFFFFFFFu;
    if (forced == 0xFFFFFFFFu) {
        const char *e = getenv("SATURN_BACK");
        forced = e ? (0xFF000000u | (uint32_t)strtoul(e, NULL, 0)) : 0xFFFFFFFEu;
    }
    if (forced != 0xFFFFFFFEu) return forced;
    if (bktau & 0x8000) addr += (uint32_t)y * 2u;      /* per-line table */
    return rgb555(vram16(s, addr));
}

/* ------------------------------------------------------------- windows ----
 * Every layer, plus the colour-calculation unit and the rotation-parameter
 * selector, carries a WINDOW SET: up to two rectangular windows (W0/W1) and,
 * for the backgrounds, the sprite window, combined with AND or OR. Ymir's
 * VDP2CalcWindow computes one boolean per pixel and the meaning of `true` is
 * "SUPPRESS": VDP2DrawNormalScrollBG does `if (windowState[x]) priority = 0;`
 * -- "make pixel transparent if inside active window area".
 *
 * None of this existed here. A layer the game had windowed down to a HUD strip
 * was drawn across the whole screen, which is why a status layer sitting at
 * priority 7 buried the entire playfield.
 */
typedef struct {
    int en[3];    /* W0, W1, sprite window */
    int inv[3];
    int logic_and;  /* WCTL bit 7 of the byte: Ymir WindowLogic { Or, And } */
} winset;

/* One byte of a WCTL register describes one window set (Ymir WriteWCTLA..D).
 * The sprite set and the rotation-parameter set have no sprite-window source,
 * because those two bits carry something else there -- pass has_sw = 0. */
static void winset_decode(winset *ws, unsigned byte, int has_sw)
{
    ws->inv[0] = (byte >> 0) & 1; ws->en[0] = (byte >> 1) & 1;
    ws->inv[1] = (byte >> 2) & 1; ws->en[1] = (byte >> 3) & 1;
    ws->inv[2] = has_sw ? (int)((byte >> 4) & 1) : 0;
    ws->en[2]  = has_sw ? (int)((byte >> 5) & 1) : 0;
    ws->logic_and = (byte >> 7) & 1;
}

/* Per-line window mask. `sp_win` is the sprite layer's own window bit for this
 * line (the shadow/sprite-window MSB of each sprite pixel), used only when a
 * set selects the sprite window as a source; NULL when it is not available. */
static void win_calc(saturn *s, const winset *ws, int y, int w,
                     const uint8_t *sp_win, uint8_t *out)
{
    int i, x;
    int logic_or = !ws->logic_and;

    if (!ws->en[0] && !ws->en[1] && !ws->en[2]) {
        memset(out, 0, (size_t)w);         /* no window: nothing suppressed */
        return;
    }
    /* AND starts all-inside, OR starts all-outside (Ymir VDP2CalcWindowLogic). */
    memset(out, logic_or ? 0 : 1, (size_t)w);

    for (i = 0; i < 2; i++) {
        unsigned base = i ? 0xC8u : 0xC0u;
        unsigned lwt  = i ? 0xDCu : 0xD8u;
        int inverted = ws->inv[i];
        int sx, ex, sy, ey;
        int doubleV = (((VR(0x00) >> 6) & 3u) == 2u);   /* single-density interlace */

        if (!ws->en[i]) continue;

        sy = (int)(int16_t)VR(base + 2u) << doubleV;
        ey = (int)(int16_t)VR(base + 6u) << doubleV;
        if (y < sy || y > ey) {
            /* Outside the vertical span: AND+upright suppresses the whole
             * line, OR+inverted admits it, and the other two combinations
             * leave this window with no effect at all. */
            if (!logic_or && !inverted)      memset(out, 1, (size_t)w);
            else if (logic_or && inverted)   memset(out, 1, (size_t)w);
            continue;
        }

        sx = (int)(int16_t)VR(base);
        ex = (int)(int16_t)VR(base + 4u);
        /* Line window table: a per-line pair of X coordinates in VRAM. */
        if (VR(lwt) & 0x8000u) {
            uint32_t a = ((uint32_t)(VR(lwt) & 7u) << 17)
                       | ((uint32_t)(VR(lwt + 2u) & 0xFFFEu) << 1);
            a += (uint32_t)y * 4u;
            sx = (int)(int16_t)vram16(s, a);
            ex = (int)(int16_t)vram16(s, a + 2u);
        }
        /* Games ship out-of-range window coordinates and expect them to mean
         * "empty" or "full line" -- the coordinates are signed. Panzer Dragoon
         * Zwei uses 0000..FFFE for an empty window and FFFE..02C0 for a full
         * one (Ymir has the same special case). */
        if (sx < 0) sx = 0;
        if (ex < 0) { if (sx >= ex) sx = 0x3FF; ex = 0; }
        if (((VR(0x00) >> 0) & 3u) < 2u) { sx >>= 1; ex >>= 1; }  /* normal res */

        if (inverted != logic_or) {
            if (sx < w) {
                if (ex > w - 1) ex = w - 1;
                for (x = sx; x <= ex; x++) out[x] = (uint8_t)logic_or;
            }
        } else {
            if (sx > w) sx = w;
            for (x = 0; x < sx; x++) out[x] = (uint8_t)logic_or;
            for (x = ex + 1; x < w; x++) if (x >= 0) out[x] = (uint8_t)logic_or;
        }
    }

    if (ws->en[2] && sp_win) {
        int inverted = ws->inv[2];
        for (x = 0; x < w; x++) {
            int v = (sp_win[x] != 0) != (inverted != 0);
            if (logic_or) out[x] = (uint8_t)(out[x] | v);
            else          out[x] = (uint8_t)(out[x] & v);
        }
    }
}

void vdp2_display_size(saturn *s, int *w, int *h)
{
    uint16_t tvmd = VR(0x00);
    static const int hw[4] = { 320, 352, 640, 704 };
    static const int vh[4] = { 224, 240, 256, 256 };
    *w = hw[tvmd & 3u];
    *h = vh[(tvmd >> 4) & 3u];
    if (*w <= 0) *w = 320;
    if (*h <= 0) *h = 224;
}

/* Compose one frame into `out` (at least w*h ARGB pixels).
 * force_on renders even when the game has cleared the display-enable bit,
 * which is what makes the panel useful while a title is still initialising. */

/* ---- CELL (tilemap) layers ------------------------------------------------
 *
 * NiGHTS does not in fact put its picture in a bitmap: with the real BIOS it
 * enables NBG0 (a mostly-empty 256-colour bitmap) and RBG0, and RBG0 is a
 * 256-colour CELL layer (CHCTLB = 0x1000, R0BMEN clear). Everything visible
 * lives in cells, which is why the composite stayed black with 128KB of VRAM
 * populated.
 *
 * Layout: screen -> plane (2x2 pages) -> page (64x64 patterns) -> pattern name
 * -> character (8x8 pixels). Map registers give each page's base in units of
 * 0x2000. Rotation IS applied to RBG0 (rot_decode + the per-line/per-pixel
 * transform further down); this comment used to say it was not, long after it
 * had been implemented, and nearly sent a later reader chasing a phantom.
 * What is still missing is the COEFFICIENT TABLE path (KTCTL 0xB4 / KTAOF
 * 0xB6): we implement Ymir's no-coefficient case only. Measured on NiGHTS,
 * KTCTL is written 40 times and is ALWAYS 0x0000, i.e. coefficient tables
 * are disabled, so the simple path is correct for those screens.
 */
typedef struct {
    int      enabled;
    int      transparent;   /* BGON TP enable (the register bit is inverted) */
    unsigned colours;
    uint16_t pnc;          /* pattern name control */
    unsigned caos;         /* colour RAM offset, in units of 256 entries */
    unsigned plsz;         /* plane size: 0=1x1, 1=2x1, 3=2x2 pages */
    uint32_t page[16];     /* byte base of each plane */
    int      grid;         /* planes per row: 2 for NBG, 4 for rotation */
    int      cellsz;       /* 1 = 8x8 characters, 2 = 16x16 */
    int      scroll_x, scroll_y;
    /* Special priority (SFPRMD). `sp_bit` is the last pattern's PR flag, set
     * by cell_pixel as a side effect so the compositor can raise this PIXEL's
     * priority without threading another parameter through the sampler. */
    int      sp_bit;
    int      sp_code;       /* palette colour bits 3-1 for SFPRMD per-dot */
    /* Scroll as the hardware keeps it: 11.8 fixed point, plus the per-dot
     * coordinate increment (zoom) in 3.8. Sampling from the INTEGER part only
     * is what made a layer with a fractional scroll drift against an
     * integer-only one -- NBG0/NBG1 have fractions, NBG2/NBG3 do not, so the
     * two halves of a scene stepped at different moments and the picture
     * wobbled as the camera moved. */
    int32_t  scroll_fx, scroll_fy;
    int32_t  zoom_h, zoom_v;
    int      bitmap;       /* N0BMEN/N1BMEN: layer is a bitmap, not cells */
    unsigned bmw, bmh;     /* bitmap dimensions in pixels */
    unsigned bmpal;        /* BMPNA palette number, units of 256 entries */
    uint32_t bmbase;       /* bitmap byte base in VRAM */

    /* ---- derived, filled in by cell_prep ---------------------------------
     * Every divisor in the screen -> plane -> page -> pattern -> character
     * walk is a power of two (a page is always 512 pixels square, a pattern
     * 8 or 16, a plane 1 or 2 pages each way, the plane grid 2 or 4), and
     * every register field it unpacks is constant for the whole frame.
     * Computed per PIXEL that came to ~8 hardware divisions plus a dozen
     * shifts of register unpacking, which is where the compositor spent most
     * of its time. Hoisted here it costs nothing, and the mask/shift forms
     * below are bit-for-bit the values the divisions produced. */
    unsigned one_word, auxmode, supp;
    unsigned pat_sh, pat_mask;   /* log2 / mask of pattern size in pixels  */
    unsigned ppp_sh;             /* log2 of patterns per page edge         */
    unsigned pw_sh,  ph_sh;      /* log2 of plane size in pixels           */
    unsigned pw_mask, ph_mask;
    unsigned spanw_mask, spanh_mask;   /* whole tiled surface, minus one   */
    unsigned planew_sh;          /* log2 of pages per plane row (0 or 1)   */
    unsigned page_step;          /* bytes per page, cell size folded in    */
    unsigned pn_step;            /* bytes per pattern name (2 or 4)        */
    unsigned cellbytes;          /* bytes per 8x8 character sub-block      */
    unsigned cellsz_sh;          /* log2 of cellsz (0 or 1)                */
    unsigned bmw_mask, bmh_mask;

    /* One-entry pattern-name cache. The compositor walks x left to right, so
     * without it a pattern name is re-fetched and re-unpacked for all 8 (or
     * 16) dots across its width. Keyed on the pattern's own address, so a hit
     * returns exactly what a miss would have computed. */
    uint32_t pn_cached;
    int      pn_valid;
    unsigned pn_charaddr, pn_paladdr;
    uint16_t pn_raw1, pn_raw2; /* raw pattern-name words for cycle-locked probes */
    int      pn_flip;
    int      pn_sp;        /* cached special-priority bit of that pattern */
} cell_cfg;

/* Fill in the derived power-of-two fields above: once per layer per frame,
 * instead of once per pixel per layer. */
static void cell_prep(cell_cfg *c)
{
    /* A zero coordinate increment would multiply every source coordinate to
     * nothing and collapse the layer onto its first pixel, so a decoder that
     * does not set these gets 1:1 sampling rather than a blank screen. RBG0
     * genuinely wants that: the rotation transform supplies the coordinates,
     * and scroll/zoom must stay neutral underneath it. */
    if (c->zoom_h == 0) c->zoom_h = 0x100;
    if (c->zoom_v == 0) c->zoom_v = 0x100;
    /* Likewise for the position: a decoder that filled in only the integer
     * scroll gets the fixed-point form derived from it. (Both being zero is
     * the normal, correct state for RBG0.) */
    if (c->scroll_fx == 0 && c->scroll_x != 0)
        c->scroll_fx = (int32_t)((uint32_t)c->scroll_x << 8);
    if (c->scroll_fy == 0 && c->scroll_y != 0)
        c->scroll_fy = (int32_t)((uint32_t)c->scroll_y << 8);

    unsigned planew, planeh;

    c->pn_valid = 0;
    if (!c->enabled) return;

    c->one_word = (c->pnc >> 15) & 1u;
    c->auxmode  = (c->pnc >> 14) & 1u;
    c->supp     = (unsigned)c->pnc & 0x3FFu;

    c->cellsz_sh = (c->cellsz == 2) ? 1u : 0u;
    c->pat_sh    = 3u + c->cellsz_sh;             /* patsz = 8 or 16       */
    c->pat_mask  = (1u << c->pat_sh) - 1u;
    c->ppp_sh    = 6u - c->cellsz_sh;             /* ppp   = 64 or 32      */

    planew = (c->plsz == 1 || c->plsz == 3) ? 2u : 1u;
    planeh = (c->plsz == 3) ? 2u : 1u;
    c->planew_sh = (planew == 2u) ? 1u : 0u;
    c->pw_sh = 9u + c->planew_sh;                 /* planew * 512          */
    c->ph_sh = 9u + ((planeh == 2u) ? 1u : 0u);   /* planeh * 512          */
    c->pw_mask = (1u << c->pw_sh) - 1u;
    c->ph_mask = (1u << c->ph_sh) - 1u;
    /* grid is 2 for an NBG and 4 for the rotation surface -- both powers of
     * two, so grid * plane size is one as well. */
    c->spanw_mask = ((unsigned)c->grid << c->pw_sh) - 1u;
    c->spanh_mask = ((unsigned)c->grid << c->ph_sh) - 1u;

    /* (one_word ? 0x2000 : 0x4000) / (cellsz == 2 ? 4 : 1). The division is
     * exact, so folding it into the constant is the same number. */
    c->page_step = (c->one_word ? 0x2000u : 0x4000u) >> (c->cellsz_sh * 2u);
    c->pn_step   = c->one_word ? 2u : 4u;

    c->cellbytes = (c->colours == 0) ? 0x20u : (c->colours == 1) ? 0x40u
                 : (c->colours <= 3) ? 0x80u : 0x100u;

    c->bmw_mask = c->bmw ? c->bmw - 1u : 0u;
    c->bmh_mask = c->bmh ? c->bmh - 1u : 0u;
}

/* Plane start address. Straight from the VDP2 manual's map table, and matching
 * Yabause's CalcPlaneAddr:
 *
 *   deca  = planeh + planew - 2      multi = planeh * planew
 *   1 word, 1x1 cell -> ((v & 0x3F) >> deca) * (multi * 0x2000)
 *   1 word, 2x2 cell -> ( v        >> deca) * (multi * 0x0800)
 *   2 word, 1x1 cell -> ((v & 0x1F) >> deca) * (multi * 0x4000)
 *   2 word, 2x2 cell -> ((v & 0x7F) >> deca) * (multi * 0x1000)
 *
 * A page is always 64x64 CELLS (512x512 pixels), so a 2x2-cell character gives
 * 32x32 patterns per page and a quarter-size page in bytes.
 */
static uint32_t plane_addr(uint32_t v, int cellsz, int one_word,
                           unsigned planew, unsigned planeh)
{
    int deca = (int)planew + (int)planeh - 2;
    uint32_t multi = planew * planeh;
    if (one_word) {
        if (cellsz == 1) return ((v & 0x3Fu) >> deca) * (multi * 0x2000u);
        return (v >> deca) * (multi * 0x800u);
    }
    if (cellsz == 1) return ((v & 0x1Fu) >> deca) * (multi * 0x4000u);
    return ((v & 0x7Fu) >> deca) * (multi * 0x1000u);
}

static uint32_t cell_pixel(saturn *s, cell_cfg *c, int x, int y,
                           int *opaque)
{
    unsigned px, py, pagei;
    unsigned charaddr, paladdr, idx, sub;
    int flip = 0, cx, cy, sp = 0;
    uint32_t pnaddr;

    /* RGB dots have no special-function colour code.  Palette paths replace
     * this with bits 3-1 of the raw dot before returning. */
    c->sp_code = -1;

    if (c->bitmap) {
        unsigned bx = (unsigned)(x + c->scroll_x) & c->bmw_mask;
        unsigned by = (unsigned)(y + c->scroll_y) & c->bmh_mask;
        *opaque = 0;
        if (c->colours == 0) {                   /* 16 colours, 4bpp */
            uint8_t b = s->vdp2_vram[(c->bmbase + by * (c->bmw / 2) + bx / 2)
                                     & (VDP2_VRAM_SZ - 1)];
            unsigned i4 = (bx & 1) ? (b & 0x0Fu) : (unsigned)(b >> 4);
            if (c->transparent && !i4) return 0;
            *opaque = 1;
            return cram_colour(s, (c->caos << 8) + (c->bmpal << 8) + i4);
        } else if (c->colours == 1) {            /* 256 colours, 8bpp */
            uint8_t i8 = s->vdp2_vram[(c->bmbase + by * c->bmw + bx)
                                      & (VDP2_VRAM_SZ - 1)];
            if (c->transparent && !i8) return 0;
            *opaque = 1;
            return cram_colour(s, (c->caos << 8) + (c->bmpal << 8) + i8);
        } else if (c->colours == 2) {            /* 2048 colours, 2 bytes/dot */
            uint32_t o = (c->bmbase + (by * c->bmw + bx) * 2u)
                       & (VDP2_VRAM_SZ - 2);
            uint16_t v = (uint16_t)((s->vdp2_vram[o] << 8) | s->vdp2_vram[o + 1]);
            unsigned i11 = v & 0x7FFu;
            if (c->transparent && !i11) return 0;
            *opaque = 1;
            return cram_colour(s, (c->caos << 8) + i11);
        } else if (c->colours == 4) {            /* RGB888, FOUR bytes/dot */
            /* This is the one NiGHTS' movie layer uses (CHCTLA = 0x0042, so
             * N0CHCN = 4). It was falling through to the RGB555 branch, which
             * reads two bytes per dot -- half the real pitch -- so the picture
             * was sampled at the wrong stride and came out as banded stripes
             * that repeated every 128 lines. Ymir: dotAddress advances by
             * sizeof(uint32) and the transparency bit is 31, not 15. */
            uint32_t o = (c->bmbase + (by * c->bmw + bx) * 4u)
                       & (VDP2_VRAM_SZ - 4);
            /* Longword per dot, [flag][B][G][R] -- same order the cell path
             * above uses, which is the one that matched real content. */
            uint8_t b0 = s->vdp2_vram[(o + 0) & (VDP2_VRAM_SZ - 1)];
            uint8_t b1 = s->vdp2_vram[(o + 1) & (VDP2_VRAM_SZ - 1)];
            uint8_t b2 = s->vdp2_vram[(o + 2) & (VDP2_VRAM_SZ - 1)];
            uint8_t b3 = s->vdp2_vram[(o + 3) & (VDP2_VRAM_SZ - 1)];
            if (c->transparent && !(b0 & 0x80u)) return 0;
            *opaque = 1;
            return 0xFF000000u | ((uint32_t)b3 << 16) | ((uint32_t)b2 << 8) | b1;
        } else {                                 /* RGB555 bitmap */
            uint32_t o = (c->bmbase + (by * c->bmw + bx) * 2u)
                       & (VDP2_VRAM_SZ - 2);
            uint16_t v = (uint16_t)((s->vdp2_vram[o] << 8) | s->vdp2_vram[o + 1]);
            if (c->transparent && !(v & 0x8000u)) return 0;
            *opaque = 1;
            return rgb555(v);
        }
    }
    *opaque = 0;

    {
        unsigned inx, iny, plane;
        /* Masking the unsigned value is exactly `% span` for a power-of-two
         * span, including for a negative x + scroll_x (which the cast has
         * already wrapped): two's complement AND is the positive remainder. */
        /* Fixed-point sampling, as the hardware does it: the scroll value is
         * 11.8 and each screen dot advances the source by the 3.8 coordinate
         * increment. Truncating to whole pixels here is what produced the
         * inter-layer wobble. */
        px = (unsigned)((c->scroll_fx + x * c->zoom_h) >> 8) & c->spanw_mask;
        py = (unsigned)((c->scroll_fy + y * c->zoom_v) >> 8) & c->spanh_mask;
        plane = (py >> c->ph_sh) * (unsigned)c->grid + (px >> c->pw_sh);
        if (plane >= 16u) plane = 15u;
        inx = px & c->pw_mask;
        iny = py & c->ph_mask;
        pagei = plane;
        /* page_base already points at the plane; step within it by page */
        pnaddr = c->page[pagei]
               + (((iny >> 9) << c->planew_sh) + (inx >> 9)) * c->page_step;
        px = inx & 511u;
        py = iny & 511u;
    }

    pnaddr += (((py >> c->pat_sh) << c->ppp_sh) + (px >> c->pat_sh)) * c->pn_step;

    if (c->pn_valid && c->pn_cached == pnaddr) {
        charaddr = c->pn_charaddr;
        paladdr  = c->pn_paladdr;
        flip     = c->pn_flip;
        sp       = c->pn_sp;
    } else {
        uint16_t t1;
        unsigned supp = c->supp;
        /* 1-word patterns have no room for it: the flag comes from the
         * SUPPLEMENT in PNCNx bit 9 and so applies to every tile of the layer
         * (Ymir: ch.specPriority = bgParams.supplScrollSpecialPriority). */
        sp = (int)((supp >> 9) & 1u);
        if (c->one_word) {
            t1 = vram16(s, pnaddr);
            c->pn_raw1 = t1;
            c->pn_raw2 = 0;
            paladdr = (c->colours == 0)
                      ? (unsigned)(((t1 & 0xF000u) >> 8) | ((supp & 0xE0u) << 3))
                      : (unsigned)((t1 & 0x7000u) >> 4);
            if (c->auxmode == 0) {
                flip = (t1 & 0x0C00u) >> 10;
                charaddr = (c->cellsz == 1)
                    ? (unsigned)((t1 & 0x3FFu) | ((supp & 0x1Fu) << 10))
                    : (unsigned)(((t1 & 0x3FFu) << 2) | (supp & 0x3u)
                                 | ((supp & 0x1Cu) << 10));
            } else {
                charaddr = (c->cellsz == 1)
                    ? (unsigned)((t1 & 0xFFFu) | ((supp & 0x1Cu) << 10))
                    : (unsigned)(((t1 & 0xFFFu) << 2) | (supp & 0x3u)
                                 | ((supp & 0x10u) << 10));
            }
        } else {
            uint16_t t2;
            t1 = vram16(s, pnaddr);
            t2 = vram16(s, pnaddr + 2);
            c->pn_raw1 = t1;
            c->pn_raw2 = t2;
            charaddr = (unsigned)(t2 & 0x7FFFu);
            flip     = (t1 & 0xC000u) >> 14;
            /* 2-word pattern: the 32-bit word carries special priority at bit
             * 29, i.e. bit 13 of this first half (Ymir Character::specPriority,
             * vdp_renderer_defs.hpp:99). */
            sp       = (int)((t1 >> 13) & 1u);
            paladdr  = (c->colours == 0) ? (unsigned)((t1 & 0x7Fu) << 4)
                                         : (unsigned)((t1 & 0x70u) << 4);
        }
        charaddr *= 0x20u;
        c->pn_cached   = pnaddr;
        c->pn_charaddr = charaddr;
        c->pn_paladdr  = paladdr;
        c->pn_flip     = flip;
        c->pn_sp       = sp;
        c->pn_valid    = 1;
    }

    c->sp_bit = sp;
    cx = (int)(px & c->pat_mask);
    cy = (int)(py & c->pat_mask);
    if (flip & 1) cx = (int)c->pat_mask - cx;    /* horizontal */
    if (flip & 2) cy = (int)c->pat_mask - cy;    /* vertical   */

    sub = (unsigned)(((cy >> 3) << c->cellsz_sh) + (cx >> 3));

    if (c->colours == 0) {                       /* 16 colour, 4bpp */
        uint32_t o = charaddr + sub * c->cellbytes
                   + (unsigned)(cy & 7) * 4u + (unsigned)((cx & 7) >> 1);
        uint8_t b = s->vdp2_vram[o & (VDP2_VRAM_SZ - 1)];
        idx = (cx & 1) ? (b & 0x0Fu) : (unsigned)(b >> 4);
        if (c->transparent && !idx) return 0;
        c->sp_code = (int)((idx >> 1) & 7u);
        *opaque = 1;
        return cram_colour(s, (c->caos << 8) + paladdr + idx);
    } else if (c->colours == 1) {                /* 256 colour, 8bpp */
        uint32_t o = charaddr + sub * c->cellbytes
                   + (unsigned)(cy & 7) * 8u + (unsigned)(cx & 7);
        idx = s->vdp2_vram[o & (VDP2_VRAM_SZ - 1)];
        if (c->transparent && !idx) return 0;
        c->sp_code = (int)((idx >> 1) & 7u);
        *opaque = 1;
        return cram_colour(s, (c->caos << 8) + paladdr + idx);
    } else if (c->colours == 2) {                /* 2048 colour, 16bpp index */
        uint32_t o = charaddr + sub * c->cellbytes
                   + (unsigned)(cy & 7) * 16u + (unsigned)(cx & 7) * 2u;
        uint16_t p16 = vram16(s, o);
        idx = p16 & 0x07FFu;
        if (c->transparent && !idx) return 0;
        c->sp_code = (int)((idx >> 1) & 7u);
        *opaque = 1;
        return cram_colour(s, idx);
    } else if (c->colours == 3) {                /* 32768 colour, RGB555 */
        uint32_t o = charaddr + sub * c->cellbytes
                   + (unsigned)(cy & 7) * 16u + (unsigned)(cx & 7) * 2u;
        uint16_t p16 = vram16(s, o);
        if (c->transparent && !(p16 & 0x8000u)) return 0;
        *opaque = 1;
        return rgb555(p16);
    } else {                                     /* 16.7M colour, RGB888 */
        /* Longword per pixel, big-endian [flag][B][G][R]. Before this branch
         * existed a 16bpp/32bpp cell was walked as 8bpp palette indices at
         * half/quarter of the true stride, which is why Sonic 3D's FMV (NBG0
         * cell layer, N0CHCN=4) composited as scattered dots on black. */
        uint32_t o = charaddr + sub * c->cellbytes
                   + (unsigned)(cy & 7) * 32u + (unsigned)(cx & 7) * 4u;
        uint8_t b0 = s->vdp2_vram[(o + 0) & (VDP2_VRAM_SZ - 1)];
        uint8_t b1 = s->vdp2_vram[(o + 1) & (VDP2_VRAM_SZ - 1)];
        uint8_t b2 = s->vdp2_vram[(o + 2) & (VDP2_VRAM_SZ - 1)];
        uint8_t b3 = s->vdp2_vram[(o + 3) & (VDP2_VRAM_SZ - 1)];
        if (c->transparent && !(b0 & 0x80u)) return 0;
        *opaque = 1;
        return 0xFF000000u | ((uint32_t)b3 << 16) | ((uint32_t)b2 << 8) | b1;
    }
}

/* NBG n in cell mode. */
static void decode_cell_nbg(saturn *s, int n, cell_cfg *c)
{
    uint16_t chctla = VR(0x28), chctlb = VR(0x2A);
    uint16_t mpofn  = VR(0x3C);
    uint16_t ab, cd, ch;
    unsigned mp;
    uint32_t pgsz;

    memset(c, 0, sizeof(*c));
    c->enabled = (VR(0x20) >> n) & 1u;
    c->transparent = ((VR(0x20) >> (8u + (unsigned)n)) & 1u) == 0u;
    if (!c->enabled) return;

    ch = (n < 2) ? (uint16_t)(chctla >> (n * 8))
                 : (uint16_t)(chctlb >> ((n - 2) * 4));
    /* NBG0/1 hold the colour count in bits 4-6; NBG2/3 only get two bits at
     * 1-2. Reading the wrong field made NBG2 render as 16-colour. Bit 0 is the
     * character size for every layer: 1 means 16x16, not 8x8. */
    /* NBG0/1 keep the colour count in bits 4-6 of their byte; NBG2/3 have a
     * single bit at 1 (0 = 16 colours, 1 = 256). Bit 0 is the character size
     * for every layer: 1 means 16x16 pixels. */
    /* [OPEN] The VDP2 manual puts NBG2/3's colour count in bit 1 of their
     * CHCTLB nibble (0 = 16, 1 = 256). The BIOS leaves CHCTLB = 0x0003, which
     * by that reading makes NBG2 256-colour -- and then its layer renders
     * nothing, because the 8bpp stride walks past what is plainly 4bpp cell
     * data. Reading it as 16-colour renders the Set Language screen perfectly
     * (70,478 of 71,680 pixels, legible text). CHCTLB is written three times
     * (0x1022 by the BIOS, 0x0000 from 0x0602E0F8, 0x0003 from 0x06040E8C), so
     * the likeliest explanation is that the 0x0003 write belongs to a later
     * reconfiguration and our end-of-run snapshot is not the state that drew
     * the screen. Until that is pinned, follow the data, not the register.
     * SATURN_N2="colours,cellsz" overrides both for testing. */
    /* CRAOFA gives each NBG a 3-bit colour-RAM offset, applied in units of
     * 256 entries. Ignoring it left 256-colour layers indexing the bottom of
     * CRAM, which is empty -- the pixels were drawn, in black. */
    /* CRAOFA is 0xE4 (Ymir vdp2_regs.hpp); 0xEA is SFPRMD, whose contents
     * happened to be zero here, which is the only reason palettes ever
     * resolved at all. */
    c->caos = (unsigned)((VR(0xE4) >> ((unsigned)n * 4u)) & 7u);
    c->plsz = (unsigned)((VR(0x3A) >> ((unsigned)n * 2u)) & 3u);
    c->colours = (n < 2) ? ((ch >> 4) & 7u) : ((ch >> 1) & 1u);
    c->cellsz  = (ch & 1u) ? 2 : 1;

    /* NBG0/1 bitmap mode (N0BMEN/N1BMEN). The CD player's starfield is a
     * 512x256 256-colour NBG0 bitmap; the cell walk renders it as nothing. */
    if (n < 2 && (ch & 2u)) {
        unsigned sz = (ch >> 2) & 3u;
        uint16_t bmpna = VR(0x2C);
        c->bitmap = 1;
        c->bmw = (sz & 2u) ? 1024u : 512u;
        c->bmh = (sz & 1u) ? 512u : 256u;
        c->bmpal = (unsigned)((bmpna >> (n * 8)) & 7u);
        c->bmbase = (uint32_t)((VR(0x3C) >> (n * 4)) & 7u) * 0x20000u;
    }
    if (n == 2 && getenv("SATURN_N2")) {      /* "colours,cellsz" */
        unsigned a = 0, b = 1;
        sscanf(getenv("SATURN_N2"), "%u,%u", &a, &b);
        c->colours = a; c->cellsz = (int)b;
    }
    c->pnc = VR(0x30 + n * 2);

    ab = VR(0x40 + n * 4);
    cd = VR(0x42 + n * 4);
    mp = (mpofn >> ((unsigned)n * 4u)) & 7u;   /* 4-bit stride, see decode_nbg */
    c->grid = 2;
    /* [MEASURED] The plane base unit stays 0x2000 here. Scaling it by the
     * computed page size (32x32 patterns for 16x16 characters) moved every
     * plane and the BIOS screen went from 70,478 rendered pixels to 3,071. */
    {
        int ow = (c->pnc >> 15) & 1;
        unsigned pw = (c->plsz == 1 || c->plsz == 3) ? 2u : 1u;
        unsigned ph = (c->plsz == 3) ? 2u : 1u;
        c->page[0] = plane_addr((mp << 6) | (ab & 0x3Fu),        c->cellsz, ow, pw, ph);
        c->page[1] = plane_addr((mp << 6) | ((ab >> 8) & 0x3Fu), c->cellsz, ow, pw, ph);
        c->page[2] = plane_addr((mp << 6) | (cd & 0x3Fu),        c->cellsz, ow, pw, ph);
        c->page[3] = plane_addr((mp << 6) | ((cd >> 8) & 0x3Fu), c->cellsz, ow, pw, ph);
    }

    {
        /* See decode_nbg: the scroll registers are not at a uniform stride. */
        static const uint8_t scx[4] = { 0x70, 0x80, 0x90, 0x94 };
        static const uint8_t scy[4] = { 0x74, 0x84, 0x92, 0x96 };
        c->scroll_x = (int)(VR(scx[n]) & 0x07FFu);
        c->scroll_y = (int)(VR(scy[n]) & 0x07FFu);
        /* NBG0/NBG1 carry the fraction in the next register along and can
         * zoom; NBG2/NBG3 are integer-only and always 1:1. */
        if (n < 2) {
            static const uint8_t zmx[2] = { 0x78, 0x88 };
            static const uint8_t zmy[2] = { 0x7C, 0x8C };
            c->scroll_fx = (int32_t)(((uint32_t)(VR(scx[n]) & 0x07FFu) << 8)
                                   | (uint32_t)(VR((uint8_t)(scx[n] + 2)) >> 8));
            c->scroll_fy = (int32_t)(((uint32_t)(VR(scy[n]) & 0x07FFu) << 8)
                                   | (uint32_t)(VR((uint8_t)(scy[n] + 2)) >> 8));
            c->zoom_h = (int32_t)((((uint32_t)VR(zmx[n]) & 7u) << 8)
                                | ((uint32_t)VR((uint8_t)(zmx[n] + 2)) >> 8));
            c->zoom_v = (int32_t)((((uint32_t)VR(zmy[n]) & 7u) << 8)
                                | ((uint32_t)VR((uint8_t)(zmy[n] + 2)) >> 8));
        } else {
            c->scroll_fx = (int32_t)((uint32_t)c->scroll_x << 8);
            c->scroll_fy = (int32_t)((uint32_t)c->scroll_y << 8);
            c->zoom_h = c->zoom_v = 0x100;
        }
        if (c->zoom_h == 0) c->zoom_h = 0x100;   /* never written: 1:1 */
        if (c->zoom_v == 0) c->zoom_v = 0x100;
        interp_scroll(s, n, &c->scroll_fx, &c->scroll_fy);
    }

    cell_prep(c);
}

/* Rotation parameter table A (VDP2 manual; bit layout per Ymir
 * RotationParamTable::ReadFrom). All values fixed point as noted. The
 * transform below is Ymir's no-coefficient path: per line, a starting screen
 * coordinate and per-pixel increment; per pixel, one scale-and-add. */
typedef struct {
    int      present;
    int32_t  Xst, Yst, Zst;        /* 13.10 */
    int32_t  dXst, dYst, dX, dY;   /*  3.10 */
    int32_t  A, B, C, D, E, F;     /*  4.10 */
    int32_t  Px, Py, Pz, Cx, Cy, Cz;
    int32_t  Mx, My;               /* 14.10 */
    int64_t  kx, ky;               /*  8.16 */
    int32_t  Xp, Yp;
    int32_t  incX, incY;
} rotp;

static int32_t sext(uint32_t v, int lo, int hi)
{
    uint32_t width = (uint32_t)(hi - lo + 1);
    uint32_t f = (v >> lo) & ((width >= 32) ? 0xFFFFFFFFu : ((1u << width) - 1u));
    if (f & (1u << (width - 1))) f |= ~(((width >= 32) ? 0xFFFFFFFFu : ((1u << width) - 1u)));
    return (int32_t)f;
}

static uint32_t vram32(saturn *s, uint32_t a)
{
    a &= VDP2_VRAM_SZ - 4;
    return ((uint32_t)s->vdp2_vram[a] << 24) | ((uint32_t)s->vdp2_vram[a+1] << 16) |
           ((uint32_t)s->vdp2_vram[a+2] << 8) | s->vdp2_vram[a+3];
}

static void rot_decode(saturn *s, rotp *r)
{
    uint32_t base = ((((uint32_t)VR(0xBC) & 7u) << 16) | VR(0xBE)) << 1;
    memset(r, 0, sizeof(*r));
    r->present = 1;
    r->Xst  = sext(vram32(s, base + 0x00), 6, 28);
    r->Yst  = sext(vram32(s, base + 0x04), 6, 28);
    r->Zst  = sext(vram32(s, base + 0x08), 6, 28);
    r->dXst = sext(vram32(s, base + 0x0C), 6, 18);
    r->dYst = sext(vram32(s, base + 0x10), 6, 18);
    r->dX   = sext(vram32(s, base + 0x14), 6, 18);
    r->dY   = sext(vram32(s, base + 0x18), 6, 18);
    r->A    = sext(vram32(s, base + 0x1C), 6, 19);
    r->B    = sext(vram32(s, base + 0x20), 6, 19);
    r->C    = sext(vram32(s, base + 0x24), 6, 19);
    r->D    = sext(vram32(s, base + 0x28), 6, 19);
    r->E    = sext(vram32(s, base + 0x2C), 6, 19);
    r->F    = sext(vram32(s, base + 0x30), 6, 19);
    r->Px   = sext(vram32(s, base + 0x34) >> 16, 0, 13);
    r->Py   = sext(vram32(s, base + 0x34), 0, 13);
    r->Pz   = sext(vram32(s, base + 0x38) >> 16, 0, 13);
    r->Cx   = sext(vram32(s, base + 0x3C), 0, 13);
    r->Cy   = sext(vram32(s, base + 0x3E) >> 16, 0, 13);
    r->Cz   = sext(vram32(s, base + 0x40) >> 16, 0, 13);
    r->Mx   = sext(vram32(s, base + 0x44), 6, 29);
    r->My   = sext(vram32(s, base + 0x48), 6, 29);
    r->kx   = sext(vram32(s, base + 0x4C), 0, 23);
    r->ky   = sext(vram32(s, base + 0x50), 0, 23);

    r->Xp = (int32_t)(r->A * (r->Px - r->Cx) + r->B * (r->Py - r->Cy) +
                      r->C * (r->Pz - r->Cz)) + (r->Cx << 10) + r->Mx;
    r->Yp = (int32_t)(r->D * (r->Px - r->Cx) + r->E * (r->Py - r->Cy) +
                      r->F * (r->Pz - r->Cz)) + (r->Cy << 10) + r->My;
    r->incX = (int32_t)(((int64_t)r->A * r->dX + (int64_t)r->B * r->dY) >> 10);
    r->incY = (int32_t)(((int64_t)r->D * r->dX + (int64_t)r->E * r->dY) >> 10);
}

/* RBG0, drawn from rotation plane A's map registers WITH the rotation
 * transform applied (see rot_decode and rp.present below). */
static void decode_cell_rbg0(saturn *s, cell_cfg *c)
{
    uint16_t chctlb = VR(0x2A);
    uint16_t mpofr  = VR(0x3E);
    unsigned mp;

    memset(c, 0, sizeof(*c));
    c->enabled = (VR(0x20) >> 4) & 1u;
    c->transparent = ((VR(0x20) >> 12) & 1u) == 0u;
    if (!c->enabled) return;
    if ((chctlb >> 9) & 1u) { c->enabled = 0; return; }   /* bitmap RBG0 */

    c->colours = (chctlb >> 12) & 7u;
    c->cellsz  = ((chctlb >> 8) & 1u) ? 2 : 1;
    c->caos    = (unsigned)(VR(0xE6) & 7u);      /* CRAOFB: R0CAOS (0xE6, not 0xEC=CCCTL) */
    c->plsz    = (unsigned)((VR(0x3A) >> 8) & 3u);
    c->pnc = VR(0x38);

    /* A rotation plane is tiled from SIXTEEN pages (A..P) in a 4x4 grid, held
     * in MPABRA..MPOPRA at 0x50..0x5E -- two per register. Treating it as the
     * NBG-style 2x2 made every other page land on empty VRAM, which rendered
     * as vertical bands of graphics separated by black. */
    mp = mpofr & 7u;
    c->grid = 4;
    {
        int ow = (c->pnc >> 15) & 1;
        unsigned pw = (c->plsz == 1 || c->plsz == 3) ? 2u : 1u;
        unsigned ph = (c->plsz == 3) ? 2u : 1u;
        for (int i = 0; i < 8; i++) {
            uint16_t r = VR(0x50 + i * 2);
            c->page[i * 2 + 0] = plane_addr((mp << 6) | (r & 0x3Fu),        c->cellsz, ow, pw, ph);
            c->page[i * 2 + 1] = plane_addr((mp << 6) | ((r >> 8) & 0x3Fu), c->cellsz, ow, pw, ph);
        }
    }

    cell_prep(c);
}

/* Dump the character cell behind one screen pixel: pattern name, character
 * number, palette and the raw 8x8 index grid. Answers "is that dither in the
 * data or in our renderer?" without needing a reference emulator. */
void vdp2_cell_debug(saturn *s, int layer, int x, int y)
{
    cell_cfg c;
    unsigned px, py, pagei, patsz, patx, paty, pnb, charnum, pal;
    uint32_t pnaddr, chaddr;
    uint16_t w0;

    if (layer == 4) decode_cell_rbg0(s, &c);
    else            decode_cell_nbg(s, layer, &c);
    if (!c.enabled) { printf("celldbg: layer %d disabled\n", layer); return; }

    pnb   = (c.pnc >> 15) & 1u;
    patsz = 8u * (unsigned)c.cellsz;
    {
        unsigned page_px = 64u * patsz;
        unsigned span = (unsigned)c.grid * page_px;
        px = (unsigned)(x + c.scroll_x) % span;
        py = (unsigned)(y + c.scroll_y) % span;
        pagei = ((py / page_px) * (unsigned)c.grid) + (px / page_px);
        patx  = (px % page_px) / patsz;
        paty  = (py % page_px) / patsz;
    }
    pnaddr = c.page[pagei] + (paty * 64u + patx) * (pnb ? 2u : 4u);
    if (pnb) {
        w0 = vram16(s, pnaddr);
        charnum = (unsigned)(w0 & 0x03FFu) | (((unsigned)c.pnc & 0x1Fu) << 10);
        pal     = (unsigned)((w0 >> 12) & 0x0Fu);
    } else {
        w0      = vram16(s, pnaddr);
        charnum = (unsigned)(vram16(s, pnaddr + 2) & 0x7FFFu);
        pal     = (unsigned)(w0 & 0x7Fu);
    }
    chaddr = charnum * 0x20u;
    printf("celldbg layer %d (%d,%d): pnc=%04X pnb=%u colours=%u cellsz=%d\n",
           layer, x, y, c.pnc, pnb, c.colours, c.cellsz);
    printf("  page[%u]=%05X pn@%05X w0=%04X char=%u pal=%u data@%05X\n",
           pagei, c.page[pagei], pnaddr, w0, charnum, pal, chaddr);
    printf("  caos=%u ramctl=%04X craofa=%04X  idx->colour:\n",
           c.caos, VR(0x0E), VR(0xEA));
    for (unsigned t = 0; t < 6; t++) {
        unsigned ix = (c.caos << 8) | ((pal & 7u) << 8) | (0x42u + t);
        printf("    idx %4u -> %08X\n", ix, cram_colour(s, ix));
    }
    for (int r = 0; r < 8; r++) {
        printf("   ");
        for (int q = 0; q < 8; q++) {
            unsigned idx;
            if (c.colours == 0) {
                uint8_t bb = s->vdp2_vram[(chaddr + r * 4u + (q >> 1))
                                          & (VDP2_VRAM_SZ - 1)];
                idx = (q & 1) ? (bb & 0x0Fu) : (unsigned)(bb >> 4);
            } else {
                idx = s->vdp2_vram[(chaddr + r * 8u + q) & (VDP2_VRAM_SZ - 1)];
            }
            printf(" %02X", idx);
        }
        printf("\n");
    }
}

void vdp2_render(saturn *s, uint32_t *out, int w, int h, int force_on)
{
    nbg_cfg cfg[4];
    cell_cfg ccfg[4], rbg0;
    /* PRINA/PRINB live at 0xF8/0xFA and PRIR at 0xFC (Ymir vdp2_regs.hpp);
     * 0xE0/0xE2 are SPCTL/SDCTL and 0xE4 is CRAOFA, so every priority this
     * renderer ever used was read from an unrelated register. Sprite pixels
     * rank by PRISA (0xF0) instead of unconditionally covering the frame. */
    uint16_t prina = VR(0xF8), prinb = VR(0xFA);
    int prio[4], prio_r0 = VR(0xFC) & 7;
    /* EIGHT sprite priorities, not one. PRISA/PRISB/PRISC/PRISD (0xF0..0xF6)
     * hold two 3-bit priority numbers each, and the sprite TYPE says which
     * bits of the VDP1 framebuffer word index that table PER PIXEL (Ymir
     * vdp_renderer_sw.cpp VDP2FetchSpriteData, then
     * params.priorities[spriteData.priority]). Ranking every sprite pixel by
     * PRISA slot 0 gave the whole cast one priority: Sonic 3D Blast tags its
     * ground sprites with index 0 and Sonic, the rings, the enemies and the
     * HUD with 2 or 3, so slot 0 for everything put the entire cast under the
     * NBG scenery and the level rendered as an empty backdrop. */
    int sp_prio_tab[8], sp_ccr_tab[8];
    /* SATURN_PIXELDBG=cycle:x:y records the exact priority inputs for one
     * composited dot on the first render at or after `cycle`.  This is kept
     * cycle-gated because the ordinary renderer is called once per field and a
     * free-running pixel trace makes the timing-sensitive evidence unreadable. */
    static int pixeldbg_init;
    static int pixeldbg_done;
    static unsigned long long pixeldbg_cycle;
    static int pixeldbg_x = -1, pixeldbg_y = -1;
    if (!pixeldbg_init) {
        const char *e = getenv("SATURN_PIXELDBG");
        pixeldbg_init = 1;
        if (e) sscanf(e, "%llu:%d:%d", &pixeldbg_cycle, &pixeldbg_x, &pixeldbg_y);
    }
    const int pixeldbg_fire = !pixeldbg_done && pixeldbg_x >= 0 && pixeldbg_y >= 0 &&
                              s->master.cycles >= pixeldbg_cycle;
    /* SFPRMD (0x1800EA) -- Special Priority Mode, 2 bits per layer:
     *   0 = per screen     the layer's PRINx priority, as-is
     *   1 = per character  the pattern's PR flag REPLACES the priority's LSB
     *   2 = per dot        same, but selected by the colour code (SFCODE)
     * Ymir VDP2GenerateCharacterPixel:
     *     priority = priorityNumber;
     *     if (PerCharacter) { priority &= ~1; priority |= specPriority; }
     *
     * With only mode 0 implemented, every tile of a layer had to sit either
     * entirely in front of or entirely behind the sprites -- which is why
     * Sonic hid correctly behind trees and cave mouths (tiles on a layer that
     * outranks him) yet showed through loop interiors, whose tiles set PR to
     * outrank him on that SAME layer. Bits 9-8 are RBG0, 7-6 NBG3, 5-4 NBG2,
     * 3-2 NBG1, 1-0 NBG0. */
    unsigned sfprmd = VR(0xEA);
    int sp_mode[5];
    sp_mode[0] = (int)( sfprmd        & 3u);   /* NBG0 */
    sp_mode[1] = (int)((sfprmd >> 2)  & 3u);   /* NBG1 */
    sp_mode[2] = (int)((sfprmd >> 4)  & 3u);   /* NBG2 */
    sp_mode[3] = (int)((sfprmd >> 6)  & 3u);   /* NBG3 */
    sp_mode[4] = (int)((sfprmd >> 8)  & 3u);   /* RBG0 */
    /* Sprite framebuffer decode (SPCTL; layouts per Ymir VDP2FetchSpriteData).
     * VDP1 pixels are NOT raw RGB555: with SPCLMD=1 only values with bit 15
     * set are RGB; everything else is PALETTE data whose low bits index CRAM
     * through CRAOFB's sprite offset. Treating palette-coded pixels as RGB is
     * why the CD player screen rendered as red noise. */
    unsigned sptype  = VR(0xE0) & 0xFu;
    int      spclmd  = (VR(0xE0) >> 5) & 1;
    unsigned spcaos  = (VR(0xE6) >> 4) & 7u;
    sp_prio_tab[0] = VR(0xF0) & 7; sp_prio_tab[1] = (VR(0xF0) >> 8) & 7;
    sp_prio_tab[2] = VR(0xF2) & 7; sp_prio_tab[3] = (VR(0xF2) >> 8) & 7;
    sp_prio_tab[4] = VR(0xF4) & 7; sp_prio_tab[5] = (VR(0xF4) >> 8) & 7;
    sp_prio_tab[6] = VR(0xF6) & 7; sp_prio_tab[7] = (VR(0xF6) >> 8) & 7;
    /* CCRSA..CCRSD index off the same word, one field along. */
    sp_ccr_tab[0] = VR(0x100) & 0x1F; sp_ccr_tab[1] = (VR(0x100) >> 8) & 0x1F;
    sp_ccr_tab[2] = VR(0x102) & 0x1F; sp_ccr_tab[3] = (VR(0x102) >> 8) & 0x1F;
    sp_ccr_tab[4] = VR(0x104) & 0x1F; sp_ccr_tab[5] = (VR(0x104) >> 8) & 0x1F;
    sp_ccr_tab[6] = VR(0x106) & 0x1F; sp_ccr_tab[7] = (VR(0x106) >> 8) & 0x1F;
    {
        /* SATURN_SPDBG=N: repeat the print every N frames. The priority
         * registers are reprogrammed per screen, so one print at boot only
         * ever describes the BIOS. */
        static long long every = -1;
        static unsigned long long nth = 0;
        if (every < 0) {
            const char *e = getenv("SATURN_SPDBG");
            every = e ? atoll(e) : 0;
            if (every == 1) every = 0;      /* =1 keeps the old once-only form */
        }
        static int told;
        if ((!told || (every > 1 && (nth % (unsigned long long)every) == 0)) &&
            getenv("SATURN_SPDBG")) {
            told = 1;
            printf("[spdbg] SPCTL=%04X type=%u spclmd=%d spcaos=%u craofb=%04X "
                   "PRISA=%04X PRISB=%04X PRISC=%04X PRISD=%04X "
                   "PRINA=%04X PRINB=%04X PRIR=%04X\n",
                   VR(0xE0), sptype, spclmd, spcaos, VR(0xE6),
                   VR(0xF0), VR(0xF2), VR(0xF4), VR(0xF6),
                   VR(0xF8), VR(0xFA), VR(0xFC));
            fflush(stdout);
        }
        nth++;
    }
    /* Frame interpolation: on the field that would otherwise re-show the same
     * VDP1 output, the sprite layer is read from the synthesized midpoint
     * instead. The real framebuffers are untouched either way. */
    const uint8_t *fb = (s->vdp1_show_interp && s->vdp1_interp_ready)
                      ? s->vdp1_interp_fb : s->vdp1_fb[s->fb_draw ^ 1];
    /* Mesh flags for the SAME framebuffer the sprite layer is read from. */
    const uint8_t *meshfb = (s->vdp1_show_interp && s->vdp1_interp_ready)
                          ? s->vdp1_interp_mesh : s->vdp1_meshfb[s->fb_draw ^ 1];
    int enabled = (VR(0x00) & 0x8000) != 0;

    /* SATURN_LAYERS is a bitmask: 1=NBG0 2=NBG1 4=NBG2 8=NBG3 16=RBG0
     * 32=sprites. Lets a single layer be isolated to see what it actually
     * draws. `layer_lock` lets the caller drive the mask instead -- that is how
     * the window's layer-dump hotkey renders the same instant six ways. */
    if (!s->layer_lock) {
        static int resolved;
        static unsigned from_env = 0x3Fu;
        if (!resolved) {
            const char *e = getenv("SATURN_LAYERS");
            from_env = e ? (unsigned)strtoul(e, NULL, 0) : 0x3Fu;
            resolved = 1;
        }
        s->layer_mask = from_env;
    }
    for (int n = 0; n < 4; n++) decode_nbg(s, n, &cfg[n]);
    for (int n = 0; n < 4; n++) decode_cell_nbg(s, n, &ccfg[n]);
    decode_cell_rbg0(s, &rbg0);

    /* NOT APPLIED: Ymir's VDP2State::UpdateEnabledBGs forces NBG1/2/3 off
     * according to NBG0's and NBG1's colour formats (RGB888 on NBG0 kills NBG1
     * and NBG3; 2048/RGB555/RGB888 kills NBG2; 2048/RGB555 on NBG1 kills
     * NBG3). Transcribed faithfully, it REGRESSED NiGHTS: the game runs NBG0
     * and NBG1 as RGB555 bitmaps (CHCTLA = 0x3332), which trips the NBG2 and
     * NBG3 rules and took the level's sky layer off the screen.
     *
     * The restriction is real hardware behaviour -- it exists because a wide
     * colour format eats the per-dot VRAM bandwidth the lower layer needs --
     * but Ymir also models VRAM access patterns and cycle allocation, which we
     * do not, so applying only the conclusion and none of the machinery gets
     * the wrong answer. Left out until the access-pattern side exists to
     * justify it. It fixed nothing observable while it was in. */
    prio[0] = prina & 7;  prio[1] = (prina >> 8) & 7;
    prio[2] = prinb & 7;  prio[3] = (prinb >> 8) & 7;

    /* Colour offsets are per LAYER, not per pixel: resolve the whole set once
     * per frame. Layer indices: 0-3 NBG, 4 RBG0, 5 back, 6 sprite. */
    int co[7][3];
    for (int q = 0; q < 7; q++) colour_offset(s, q, &co[q][0], &co[q][1], &co[q][2]);

    /* Colour calculation (CCCTL 0xEC): per-layer enable, blended against
     * whatever is already composed underneath. The ratio registers hold the
     * SECOND screen's weight (Ymir WriteCCRNA: value ^ 31), and the blend is
     * btm + (top - btm) * topw / 32 (vdp_renderer_sw Color888CompositeRatio).
     * This is the Set Language shimmer and the CD player's translucent
     * "Start Application" plate. Additive mode is CCCTL bit 8. */
    uint16_t ccctl = VR(0xEC);
    int cc_add = (ccctl >> 8) & 1;
    int cc_en[7];        /* 0-3 NBG, 4 RBG0, 6 sprite */
    unsigned cc_topw[7];
    cc_en[0] = (ccctl >> 0) & 1;  cc_en[1] = (ccctl >> 1) & 1;
    cc_en[2] = (ccctl >> 2) & 1;  cc_en[3] = (ccctl >> 3) & 1;
    cc_en[4] = (ccctl >> 4) & 1;  cc_en[5] = 0;
    cc_en[6] = (ccctl >> 6) & 1;
    cc_topw[0] = 31u - (VR(0x108) & 0x1Fu);
    cc_topw[1] = 31u - ((VR(0x108) >> 8) & 0x1Fu);
    cc_topw[2] = 31u - (VR(0x10A) & 0x1Fu);
    cc_topw[3] = 31u - ((VR(0x10A) >> 8) & 0x1Fu);
    cc_topw[4] = 31u - (VR(0x10C) & 0x1Fu);
    cc_topw[5] = 31u;
    cc_topw[6] = 31u - (VR(0x100) & 0x1Fu);   /* CCRSA slot 0 for sprites */

    rotp rp; rp.present = 0;
    if (rbg0.enabled) rot_decode(s, &rp);

    /* The list of layers that can contribute a pixel, resolved ONCE per frame.
     * Order within the list no longer matters: the compositor below sorts per
     * pixel by Ymir's key, so this is just "which layers are live".
     *
     * A layer whose priority number is ZERO is not displayed at all -- Ymir
     * VDP2ComposeLine skips any layer with `priority == 0` before it ever
     * reaches the sort. That test used to be applied to NBG0-3 but NOT to
     * RBG0, so a rotation layer parked at priority 0 still painted over the
     * back screen. */
    struct { int rot; int n; int pass; } ord[5];
    int nord = 0;
    if (enabled || force_on) {
        if (rbg0.enabled && prio_r0 != 0 && (s->layer_mask & 0x10u)) {
            ord[nord].rot = 1; ord[nord].n = 4; ord[nord].pass = prio_r0; nord++;
        }
        for (int n = 0; n < 4; n++) {
            if (!cfg[n].enabled || prio[n] == 0) continue;
            if (!(s->layer_mask & (1u << n))) continue;
            if (!bitmap8_visible(s, &cfg[n], w, h)) continue;
            ord[nord].rot = 0; ord[nord].n = n; ord[nord].pass = prio[n]; nord++;
        }
    }

    /* Ymir's compositing sort key, `(priority << 3) | (layer ^ 7)`, with
     * LayerIndex = Sprite 0, RBG0 1, NBG0 2, NBG1 3, NBG2 4, NBG3 5, Back 6
     * (vdp_renderer_sw.hpp). Highest key wins, so on EQUAL priority the lower
     * layer index wins: Sprite, then RBG0, then NBG0..NBG3, then the back
     * screen. Our own layer numbering (0-3 NBG, 4 RBG0, 5 back, 6 sprite) is
     * what co[]/cc_en[]/cc_topw[] are indexed by, so map across rather than
     * renumbering everything.
     *
     * This is where the rotation layer was wrong: the old compositor gave
     * RBG0 the LOWEST rank of the six, so any NBG it tied with buried it. */
    static const uint8_t ymir_lyr[7] = { 2, 3, 4, 5, 1, 6, 0 };
    #define SORT_KEY(lyr, pri) \
        ((uint8_t)(((unsigned)(pri) << 3) | (unsigned)(ymir_lyr[(lyr)] ^ 7u)))

    /* Sprite colour-calculation condition: SPCTL fields, constant per frame. */
    const unsigned sp_cond = (VR(0xE0) >> 12) & 3u;
    const unsigned sp_nval = (VR(0xE0) >> 8) & 7u;
    const int      sp_on   = (s->layer_mask & 0x20u) != 0;
    /* Per sprite type: the colour-data mask, and where the priority and
     * colour-calculation-ratio INDICES sit in the framebuffer word. A shift of
     * -1 means the type has no such field, which resolves to index 0. Types
     * 0-7 read a 16-bit word, 8-15 a byte. (Ymir VDP2FetchSpriteData.) */
    static const struct {
        uint16_t cmask; int8_t dbits, pshift, pbits, cshift, cbits, has_sw;
    } sptab[16] = {
        { 0x7FF, 11, 14, 2, 11, 3, 0 },   /* 0 */
        { 0x7FF, 11, 13, 3, 11, 2, 0 },   /* 1 */
        { 0x7FF, 11, 14, 1, 11, 3, 1 },   /* 2 */
        { 0x7FF, 11, 13, 2, 11, 2, 1 },   /* 3 */
        { 0x3FF, 10, 13, 2, 10, 3, 1 },   /* 4 */
        { 0x7FF, 11, 12, 3, 11, 1, 1 },   /* 5 */
        { 0x3FF, 10, 12, 3, 10, 2, 1 },   /* 6 */
        { 0x1FF,  9, 12, 3,  9, 3, 1 },   /* 7 */
        { 0x07F,  7,  7, 1, -1, 0, 0 },   /* 8 */
        { 0x03F,  6,  7, 1,  6, 1, 0 },   /* 9 */
        { 0x03F,  6,  6, 2, -1, 0, 0 },   /* A */
        { 0x03F,  6, -1, 0,  6, 2, 0 },   /* B */
        { 0x0FF,  8,  7, 1, -1, 0, 0 },   /* C */
        { 0x0FF,  8,  7, 1,  6, 1, 0 },   /* D */
        { 0x0FF,  8,  6, 2, -1, 0, 0 },   /* E */
        { 0x0FF,  8, -1, 0,  6, 2, 0 },   /* F */
    };
    const unsigned sp_cmask  = sptab[sptype].cmask;
    const int      sp_pshift = sptab[sptype].pshift;
    const unsigned sp_pmax   = (1u << sptab[sptype].pbits) - 1u;
    const int      sp_cshift = sptab[sptype].cshift;
    const unsigned sp_rmax   = (1u << sptab[sptype].cbits) - 1u;
    /* The NORMAL SHADOW pattern: colour data all ones except the LSB. A sprite
     * pixel carrying it is not a colour at all -- it halves whatever is under
     * it (Ymir GetSpecialPattern / Color888ShadowMasked). */
    const unsigned sp_shadow_val = (1u << sptab[sptype].dbits) - 2u;
    const int      sp_has_sw     = sptab[sptype].has_sw;
    /* SPCTL bit 4: the sprite MSB means "window", not "shadow". */
    static int sp_trans_cd = -1;
    if (sp_trans_cd < 0) sp_trans_cd = getenv("SATURN_SPTRANS") ? 1 : 0;
    const int      sp_use_win    = (VR(0xE0) >> 4) & 1;
    const int      sp_win_en     = (VR(0xD4) >> 13) & 1;   /* WCTLC */
    const int      sp_win_inv    = (VR(0xD4) >> 12) & 1;

    /* SDCTL (0xE2): which layers ACCEPT a sprite shadow (Ymir WriteSDCTL). */
    int shadow_en[7];
    shadow_en[0] = (VR(0xE2) >> 0) & 1;  shadow_en[1] = (VR(0xE2) >> 1) & 1;
    shadow_en[2] = (VR(0xE2) >> 2) & 1;  shadow_en[3] = (VR(0xE2) >> 3) & 1;
    shadow_en[4] = (VR(0xE2) >> 4) & 1;  shadow_en[5] = (VR(0xE2) >> 5) & 1;
    shadow_en[6] = 0;                    /* sprite-on-sprite uses its own bit */

    /* Window sets. WCTLA low/high = NBG0/NBG1, WCTLB = NBG2/NBG3,
     * WCTLC = RBG0/sprite, WCTLD high = colour calculation. The sprite set has
     * no sprite-window source of its own (those two bits configure the source
     * instead), and neither does the rotation-parameter set. */
    /* Mosaic sizes are shared; enables are per layer. A register value of 0
     * yields sizes of 1, i.e. no mosaic, so titles that never touch MZCTL are
     * unaffected. */
    unsigned mz_reg = VR(0x22);
    unsigned mz_on  = mz_reg & 0x1Fu;
    int      mz_h   = (int)(((mz_reg >> 8) & 0xFu) + 1u);
    int      mz_v   = (int)(((mz_reg >> 12) & 0xFu) + 1u);
    winset ws_bg[5], ws_sp, ws_cc;
    winset_decode(&ws_bg[0], VR(0xD0) & 0xFFu,        1);   /* NBG0 */
    winset_decode(&ws_bg[1], (VR(0xD0) >> 8) & 0xFFu, 1);   /* NBG1 */
    winset_decode(&ws_bg[2], VR(0xD2) & 0xFFu,        1);   /* NBG2 */
    winset_decode(&ws_bg[3], (VR(0xD2) >> 8) & 0xFFu, 1);   /* NBG3 */
    winset_decode(&ws_bg[4], VR(0xD4) & 0xFFu,        1);   /* RBG0 */
    winset_decode(&ws_sp,    (VR(0xD4) >> 8) & 0xFFu, 0);   /* sprite */
    winset_decode(&ws_cc,    (VR(0xD6) >> 8) & 0xFFu, 1);   /* colour calc */

    /* CCCTL bit 9 picks WHICH layer supplies the blend ratio: the top screen
     * or the second (Ymir colorCalcParams.useSecondScreenRatio). */
    const int cc_second_ratio = (ccctl >> 9) & 1;
    const int cc_any = (ccctl & 0x005Fu) != 0;

    /* SATURN_SPDBG also counts who actually WON each pixel. "layer covered N
     * pixels at priority P" is the one number that separates "the sprite was
     * never drawn" from "the sprite was drawn and then buried", and it costs
     * one increment per pixel. */
    unsigned long long win[8] = {0,0,0,0,0,0,0,0};   /* 0-3 NBG, 4 RBG0, 5 back, 6 sprite, 7 sprite-lost */
    const int spdbg_on = getenv("SATURN_SPDBG") != NULL;

    /* Per-line scratch. The sprite layer is decoded BEFORE the backgrounds
     * because a background's window set can select the SPRITE WINDOW as one of
     * its sources, and that source is the sprite pixel's own MSB. Ymir runs the
     * same order: VDP2DrawSpriteLayer, VDP2CalcWindows, the BGs, then
     * VDP2ComposeLine. */
    static uint8_t  win_bg[5][704], win_sp[704], win_cc[704];
    static uint8_t  sp_winbit[704], sp_pri[704], sp_spec[704], sp_ratio[704];
    static uint8_t  sp_msb[704];
    static uint8_t  mesh_pri[704], mesh_spec[704], mesh_ratio[704], mesh_msb[704];
    static uint32_t sp_col[704];
    static uint32_t mesh_col[704];
    static uint32_t bg_cache_col[704 * 512];
    static uint8_t  bg_cache_sort[704 * 512], bg_cache_layer[704 * 512];
    static uint64_t bg_cache_sig;
    static int      bg_cache_valid;
    enum { SP_NORMAL = 0, SP_SHADOW = 1, SP_TRANSPARENT = 2 };

    const int sp_xshift = (w > 512) ? 1 : 0;

    /* A window set with nothing enabled suppresses nothing, and that is the
     * common case -- most frames of most games enable no windows at all. Clear
     * those masks ONCE here rather than re-deriving 224 identical all-zero
     * lines per frame per layer. */
    #define WS_LIVE(ws) ((ws).en[0] || (ws).en[1] || (ws).en[2])
    int wl_bg[5], wl_sp = WS_LIVE(ws_sp), wl_cc = WS_LIVE(ws_cc);
    for (int n = 0; n < 5; n++) {
        wl_bg[n] = WS_LIVE(ws_bg[n]);
        if (!wl_bg[n]) memset(win_bg[n], 0, (size_t)w);
    }
    if (!wl_sp) memset(win_sp, 0, (size_t)w);
    if (!wl_cc) memset(win_cc, 0, (size_t)w);

    const int novdp2opt = vdp2_opts_off();
    int bg_cache_eligible = !cc_any && !spdbg_on && !novdp2opt;
    for (int n = 0; n < 5; n++) if (wl_bg[n]) bg_cache_eligible = 0;
    uint64_t bg_sig = 1469598103934665603ull;
    if (bg_cache_eligible) {
        #define BG_HASH(v) do { bg_sig ^= (uint64_t)(v); bg_sig *= 1099511628211ull; } while (0)
        BG_HASH(s->vdp2_vram_epoch); BG_HASH(s->cram_epoch);
        BG_HASH((unsigned)w); BG_HASH((unsigned)h); BG_HASH(s->layer_mask); BG_HASH(force_on);
        for (unsigned q = 0; q < 0x110u / 2u; q++) {
            if (q == (0x04u >> 1)) continue;
            BG_HASH(s->vdp2_reg[q]);
        }
        #undef BG_HASH
    }
    const int bg_cache_hit = bg_cache_eligible && bg_cache_valid && bg_cache_sig == bg_sig;

    /* A normal sprite above every fixed-priority background is the final
     * pixel whenever colour calculation is disabled.  Sonic's FMV/title
     * frames put roughly 60% of the screen in VDP1 at priority 6, above two
     * VDP2 layers; sampling both hidden backgrounds for every one of those
     * pixels was pure work. */
    uint8_t bg_max_key = SORT_KEY(5, 0);
    for (int k = 0; k < nord; k++) {
        uint8_t key = SORT_KEY(ord[k].n, ord[k].pass);
        if (key > bg_max_key) bg_max_key = key;
    }

    for (int y = 0; y < h; y++) {
        uint32_t backc = back_colour(s, y);
        const int sp_line = sp_on && y < 256;

        /* ---- the sprite line, decoded per Ymir VDP2DrawSpritePixel -------- */
        if (wl_sp) win_calc(s, &ws_sp, y, w, NULL, win_sp);
        memset(sp_pri,    0, (size_t)w);
        memset(sp_spec,   SP_TRANSPARENT, (size_t)w);
        memset(sp_winbit, 0, (size_t)w);
        memset(sp_col,    0, (size_t)w * sizeof(sp_col[0]));
        memset(sp_ratio,  0, (size_t)w);
        memset(sp_msb,    0, (size_t)w);
        memset(mesh_pri,  0, (size_t)w);
        memset(mesh_spec, SP_TRANSPARENT, (size_t)w);
        memset(mesh_ratio,0, (size_t)w);
        memset(mesh_msb,  0, (size_t)w);
        memset(mesh_col,  0, (size_t)w * sizeof(mesh_col[0]));
        for (int x = 0; x < w; x++) {
            uint32_t o;
            uint16_t p, mp;
            unsigned cd;
            int pidx = 0, ridx = 0, sw = 0, spec = SP_TRANSPARENT;
            uint32_t c = 0;

            /* In the 640/704 modes VDP1 still draws a 512-wide framebuffer, so
             * VDP2 reads it at half rate and doubles each dot across (Ymir's
             * `doubleResH`: maxX = HRes >> 1, then CopyPixel to xx + 1).
             * Reading it 1:1 left the right-hand quarter of a hi-res screen
             * with no sprites at all. */
            if (!sp_line || (x >> sp_xshift) >= 512 || win_sp[x]) continue;

            o = (uint32_t)(y * 512 + (x >> sp_xshift)) * 2u;
             p = (uint16_t)((fb[o] << 8) | fb[o + 1]);
             mp = (uint16_t)((meshfb[o] << 8) | meshfb[o + 1]);

             if (pixeldbg_fire && x == pixeldbg_x && y == pixeldbg_y) {
                 printf("[pixeldbg] cycle=%llu xy=%d,%d raw-sprite=%04X raw-mesh=%04X "
                        "SPCTL=%04X type=%u PRIS=%d,%d,%d,%d,%d,%d,%d,%d\n",
                        (unsigned long long)s->master.cycles, x, y, p, mp,
                        VR(0xE0), sptype,
                        sp_prio_tab[0], sp_prio_tab[1], sp_prio_tab[2], sp_prio_tab[3],
                        sp_prio_tab[4], sp_prio_tab[5], sp_prio_tab[6], sp_prio_tab[7]);
             }

            /* The overwhelmingly common empty framebuffer word carries no
             * colour, priority, shadow or window information. */
            if (p == 0) goto decode_mesh;

            if (spclmd && (p & 0x8000u)) {
                /* Direct RGB pixel. Byte-sized types are transparent when the
                 * low 8 bits are clear; the word-sized types that carry a
                 * window bit are transparent when the low 15 are clear and the
                 * sprite window is in use. */
                if (sptype >= 8) { if ((p & 0x00FFu) == 0) goto decode_mesh; }
                else if (sptype >= 2) { if (sp_use_win && (p & 0x7FFFu) == 0) goto decode_mesh; }
                c = rgb555(p);
                spec = SP_NORMAL;
                sp_msb[x] = 1;
            } else {
                cd   = p & sp_cmask;
                pidx = (sp_pshift >= 0) ? (int)((p >> sp_pshift) & sp_pmax) : 0;
                ridx = (sp_cshift >= 0) ? (int)((p >> sp_cshift) & sp_rmax) : 0;
                sw   = sp_has_sw ? (int)((p >> 15) & 1) : 0;
                /* Transparency is the whole low 15 bits being clear, NOT the
                 * colour data alone: a pixel carrying a priority but no colour
                 * still draws palette entry 0 (Ymir GetSpecialPattern). */
                /* SATURN_SPTRANS=cd restores the older rule "colour data
                 * zero is transparent". Ymir's rule is that the whole low 15
                 * bits must be clear, which means a pixel carrying a priority
                 * but no colour still draws palette entry 0 -- and if that
                 * entry is black, sprites gain a black fringe. Which is right
                 * depends on whether VDP1 wrote those pixels at all (SPD), so
                 * keep both until a title settles it. */
                spec = (sp_trans_cd ? (cd == 0)
                                    : ((p & 0x7FFFu) == 0)) ? SP_TRANSPARENT
                     : (cd == sp_shadow_val) ? SP_SHADOW : SP_NORMAL;
                /* A sprite-window pixel contributes NOTHING to the picture; it
                 * only carries its bit into the background window sets. */
                if (sp_use_win && sp_win_en && sw != sp_win_inv) {
                    sp_winbit[x] = 1;
                    goto decode_mesh;
                }
                c = cram_colour(s, (spcaos << 8) + cd);
                if (sp_cond == 3) sp_msb[x] = (uint8_t)cram_msb(s, (spcaos << 8) + cd);
            }

            sp_col[x]   = c;
            sp_ratio[x] = (uint8_t)ridx;
            sp_spec[x]  = (uint8_t)spec;
            sp_winbit[x]= (uint8_t)sw;
            /* Priority 0 = not displayed. A transparent pixel keeps its
             * priority only when it is carrying a shadow/window bit. */
            sp_pri[x]   = (spec == SP_TRANSPARENT && !sw)
                        ? 0 : (uint8_t)sp_prio_tab[pidx];

decode_mesh:
            /* The enhancement's mesh framebuffer is decoded as a second
             * sprite layer.  It deliberately does not replace `p`: the
             * ordinary framebuffer holds the VDP1 object underneath the mesh
             * and must remain available to the compositor. */
            if (mp != 0) {
                int mi = 0, mr = 0, mspec = SP_TRANSPARENT;
                uint32_t mc = 0;
                if (spclmd && (mp & 0x8000u)) {
                    if (sptype < 8 || (mp & 0x00FFu) != 0) {
                        mc = rgb555(mp);
                        mspec = SP_NORMAL;
                        mesh_msb[x] = 1;
                    }
                } else {
                    unsigned mcd = mp & sp_cmask;
                    mi = (sp_pshift >= 0) ? (int)((mp >> sp_pshift) & sp_pmax) : 0;
                    mr = (sp_cshift >= 0) ? (int)((mp >> sp_cshift) & sp_rmax) : 0;
                    mspec = (sp_trans_cd ? (mcd == 0)
                                         : ((mp & 0x7FFFu) == 0)) ? SP_TRANSPARENT
                          : (mcd == sp_shadow_val) ? SP_SHADOW : SP_NORMAL;
                    mc = cram_colour(s, (spcaos << 8) + mcd);
                    if (sp_cond == 3)
                        mesh_msb[x] = (uint8_t)cram_msb(s, (spcaos << 8) + mcd);
                }
                mesh_col[x] = mc;
                mesh_ratio[x] = (uint8_t)mr;
                mesh_spec[x] = (uint8_t)mspec;
                mesh_pri[x] = (mspec == SP_NORMAL) ? (uint8_t)sp_prio_tab[mi] : 0;
            }
        }

        /* ---- window masks, now that the sprite window source exists ------ */
        for (int n = 0; n < 5; n++)
            if (wl_bg[n]) win_calc(s, &ws_bg[n], y, w, sp_winbit, win_bg[n]);
        if (wl_cc) win_calc(s, &ws_cc, y, w, sp_winbit, win_cc);

        /* Per-line rotation state (Ymir vdp_renderer_sw.cpp): the screen-space
         * start of this line and its per-pixel increment. */
        int32_t rXsp = 0, rYsp = 0;
        if (rp.present) {
            int32_t xst = rp.Xst + rp.dXst * y;
            int32_t yst = rp.Yst + rp.dYst * y;
            rXsp = (int32_t)(((int64_t)rp.A * (xst - (rp.Px << 10)) +
                              (int64_t)rp.B * (yst - (rp.Py << 10)) +
                              (int64_t)rp.C * (rp.Zst - (rp.Pz << 10))) >> 10);
            rYsp = (int32_t)(((int64_t)rp.D * (xst - (rp.Px << 10)) +
                              (int64_t)rp.E * (yst - (rp.Py << 10)) +
                              (int64_t)rp.F * (rp.Zst - (rp.Pz << 10))) >> 10);
        }

        for (int x = 0; x < w; x++) {
            /* Ymir keeps a sorted stack of the layers covering this pixel and
             * composes only the TOP two: the top is the picture, the second is
             * what colour calculation blends it against. The old compositor
             * painted every opaque layer bottom-up and blended into a running
             * colour, so a layer that lost on priority was DISCARDED rather
             * than left underneath -- which is why Sonic 3D Blast's "RUSTY
             * RUIN / ACT 2" title card vanished under the translucent cloud
             * layer instead of showing through it. The stack is seeded with
             * the back screen, which sits at priority 0. */
            uint8_t k0 = SORT_KEY(5, 0), k1 = SORT_KEY(5, 0);
            int      l0 = 5, l1 = 5;
            uint32_t c0 = backc, c1 = backc;
            int sp_pushed = 0;
            unsigned outi = (unsigned)y * (unsigned)w + (unsigned)x;

            /* With no blend active, a normal sprite whose key exceeds every
             * possible background key is already the final answer.  Preserve
             * the end-of-pipeline colour offset and diagnostics, then avoid
             * all background address/pattern/palette work. */
            if (!novdp2opt && mesh_pri[x] == 0 &&
                (!bg_cache_eligible || bg_cache_hit) &&
                !cc_any && sp_pri[x] > 0 && sp_spec[x] == SP_NORMAL &&
                SORT_KEY(6, sp_pri[x]) > bg_max_key) {
                uint32_t colour = apply_offset(sp_col[x], co[6][0], co[6][1], co[6][2]);
                if (spdbg_on) win[6]++;
                out[outi] = colour;
                continue;
            }

            if (bg_cache_hit) {
                k0 = bg_cache_sort[outi];
                l0 = bg_cache_layer[outi];
                c0 = bg_cache_col[outi];
            }

            #define PUSH(lyr, pri, col) do {                                   \
                uint8_t _k = SORT_KEY((lyr), (pri));                           \
                if (_k > k0) { k1 = k0; l1 = l0; c1 = c0;                      \
                               k0 = _k; l0 = (lyr); c0 = (col); }              \
                else if (_k > k1) { k1 = _k; l1 = (lyr); c1 = (col); }         \
            } while (0)

            for (int k = 0; !bg_cache_hit && k < nord; k++) {
                int opaque = 0;
                int n = ord[k].n;
                int dot_sp = 0, dot_code = -1;
                uint32_t c;
                if (wl_bg[n] && win_bg[n][x]) continue; /* windowed out */
                if (!ord[k].rot) {
                    /* Mosaic (MZCTL 0x22). Ymir applies it in the pixel loop
                     * via mosaicCounterX/Y: the layer samples at the ORIGIN of
                     * each block and repeats that texel across it. Bits 0-4
                     * enable it per layer (NBG0-3, RBG0); MZSZH (11-8) and
                     * MZSZV (15-12) hold size-1 and are shared by every layer.
                     * We had NO mosaic at all, so a title using it for a
                     * transition or pixelate effect drew at full resolution. */
                    int mx = x, my = y;
                    if (mz_on & (1u << n)) { mx = x - (x % mz_h); my = y - (y % mz_v); }
                    if (cfg[n].bitmap) {
                        c = bitmap_pixel(s, &cfg[n], mx, my, &opaque);
                        dot_sp = cfg[n].sp_bit;
                        dot_code = cfg[n].sp_code;
                    } else {
                        c = cell_pixel(s, &ccfg[n], mx, my, &opaque);
                        dot_sp = ccfg[n].sp_bit;
                        dot_code = ccfg[n].sp_code;
                    }
                } else {
                    int rx = x, ry = y;
                    if (rp.present) {
                        int32_t sx = rXsp + rp.incX * x;
                        int32_t sy = rYsp + rp.incY * x;
                        rx = (int)((((rp.kx * sx) >> 16) + rp.Xp) >> 10);
                        ry = (int)((((rp.ky * sy) >> 16) + rp.Yp) >> 10);
                        /* Screen-over: repeat. The rotation surface is
                         * grid*512 pixels on a side. */
                        rx &= (rbg0.grid * 512) - 1;
                        ry &= (rbg0.grid * 512) - 1;
                    }
                    c = cell_pixel(s, &rbg0, rx, ry, &opaque);
                    dot_sp = rbg0.sp_bit;
                    dot_code = rbg0.sp_code;
                }
                 if (opaque) {
                    /* Per-pixel priority, exactly as VDP2 applies SFPRMD.
                     * Per-character uses the pattern-name special-priority
                     * flag directly.  Per-dot first requires that flag, then
                     * selects SFCODE A/B through SFSEL and tests the bit named
                     * by palette-index bits 3-1.  Treating per-dot as
                     * per-character raised every nontransparent dot in a
                     * marked Sonic 3D Blast tile; loop interiors consequently
                     * covered (or failed to cover) Sonic as one solid square. */
                    int pri = ord[k].pass;
                     if (sp_mode[n] == 1) {
                         pri = (pri & ~1) | dot_sp;
                    } else if (sp_mode[n] == 2) {
                        pri &= ~1;
                        if (dot_sp && dot_code >= 0) {
                            unsigned bank = (VR(0x24) >> n) & 1u; /* SFSEL */
                            unsigned bitn = bank * 8u + (unsigned)dot_code;
                             pri |= (int)((VR(0x26) >> bitn) & 1u); /* SFCODE */
                         }
                     }
                     if (pixeldbg_fire && x == pixeldbg_x && y == pixeldbg_y) {
                         printf("[pixeldbg] layer=%d rot=%d base-pri=%d mode=%d "
                                "special=%d code=%d eff-pri=%d key=%u color=%08X "
                                "pn=%05X raw=%04X,%04X\n",
                                n, ord[k].rot, ord[k].pass, sp_mode[n], dot_sp,
                                dot_code, pri, (unsigned)SORT_KEY(n, pri), c,
                                ord[k].rot ? rbg0.pn_cached : ccfg[n].pn_cached,
                                ord[k].rot ? rbg0.pn_raw1 : ccfg[n].pn_raw1,
                                ord[k].rot ? rbg0.pn_raw2 : ccfg[n].pn_raw2);
                     }
                     PUSH(n, pri, c);
                 } else if (pixeldbg_fire && x == pixeldbg_x && y == pixeldbg_y) {
                     printf("[pixeldbg] layer=%d rot=%d transparent\n", n, ord[k].rot);
                 }
            }
            if (bg_cache_eligible && !bg_cache_hit) {
                bg_cache_sort[outi] = k0;
                bg_cache_layer[outi] = (uint8_t)l0;
                bg_cache_col[outi] = c0;
            }

            /* A sprite pixel joins the stack only when it is a real colour:
             * shadow-pattern and window pixels have an effect but are not
             * layers (Ymir skips `specialType != Normal` when sorting). */
             if (sp_pri[x] > 0 && sp_spec[x] == SP_NORMAL) {
                 PUSH(6, sp_pri[x], sp_col[x]);
                 sp_pushed = 1;
             }
             if (pixeldbg_fire && x == pixeldbg_x && y == pixeldbg_y) {
                 printf("[pixeldbg] sprite-pri=%u sprite-spec=%u sprite-color=%08X "
                        "top-layer=%d top-key=%u second-layer=%d second-key=%u\n",
                        (unsigned)sp_pri[x], (unsigned)sp_spec[x], sp_col[x],
                        l0, (unsigned)k0, l1, (unsigned)k1);
                 pixeldbg_done = 1;
                 fflush(stdout);
             }
             #undef PUSH

            /* Locate the separate transparent-mesh pixel in the normal layer
             * stack.  Mesh wins a tie with the ordinary sprite layer (Ymir's
             * scanline_meshLayers priority comparison uses >=).  A mesh in
             * position 1 is averaged into the second screen before the top
             * layer's colour calculation; a topmost mesh is merged after the
             * normal pipeline below. */
            int mesh_pos = -1;
            if (mesh_pri[x] > 0 && mesh_spec[x] == SP_NORMAL) {
                unsigned p0 = (unsigned)(k0 >> 3);
                unsigned p1 = (unsigned)(k1 >> 3);
                if ((unsigned)mesh_pri[x] >= p0) mesh_pos = 0;
                else if ((unsigned)mesh_pri[x] >= p1) mesh_pos = 1;
                if (mesh_pos == 1) c1 = cc_blend(mesh_col[x], c1, 16u, 0);
            }

            uint32_t colour = c0;

            /* Colour calculation blends the top layer with the SECOND, gated
             * per layer by CCCTL and suppressed inside the colour-calculation
             * window. CCCTL bit 9 says whose ratio to use. */
            if (cc_en[l0] && (!wl_cc || !win_cc[x])) {
                int rl = cc_second_ratio ? l1 : l0;
                unsigned topw = (rl == 6)
                    ? 31u - (unsigned)sp_ccr_tab[sp_ratio[x]]
                    : cc_topw[rl];
                int ok = 1;
                if (l0 == 6) {
                    /* SPCTL bits 12-13 pick the per-pixel condition against
                     * SPCCN (bits 8-10): prio<=N, prio==N, prio>=N, or the
                     * colour's own MSB. */
                    switch (sp_cond) {
                    case 0:  ok = sp_pri[x] <= (int)sp_nval; break;
                    case 1:  ok = sp_pri[x] == (int)sp_nval; break;
                    case 2:  ok = sp_pri[x] >= (int)sp_nval; break;
                    default: ok = sp_msb[x];                 break;
                    }
                }
                if (ok) colour = cc_blend(c0, c1, topw, cc_add);
            }

            /* Sprite shadow: halve the top layer, provided the sprite is not
             * itself buried and the top layer accepts shadows (SDCTL). */
            if (sp_line && sp_pri[x] >= ((k0 >> 3) & 7u)) {
                int normal_shadow = (sp_spec[x] == SP_SHADOW);
                int msb_shadow    = !sp_use_win && sp_winbit[x];
                if (normal_shadow || msb_shadow) {
                    int en = (l0 == 6) ? sp_winbit[x] : shadow_en[l0];
                    if (en) colour = 0xFF000000u | ((colour >> 1) & 0x7F7F7Fu);
                }
            }

            /* The colour offset unit sits at the END of the pipeline and uses
             * the TOP layer's select (Ymir applies it to the composited
             * output, not to each layer on the way in). */
            colour = apply_offset(colour, co[l0][0], co[l0][1], co[l0][2]);

            /* Transparent-mesh enhancement.  Unlike the old flag-only path,
             * `mesh_col` still has the mesh source while `colour` still has
             * the underlying VDP1 sprite.  Apply the mesh sprite's own colour
             * calculation and offset, then average it with the completed
             * normal pipeline, matching Ymir's final mesh pass. */
            if (mesh_pos == 0) {
                uint32_t mc = mesh_col[x];
                if (cc_en[6] && (!wl_cc || !win_cc[x])) {
                    int ok;
                    switch (sp_cond) {
                    case 0:  ok = mesh_pri[x] <= (int)sp_nval; break;
                    case 1:  ok = mesh_pri[x] == (int)sp_nval; break;
                    case 2:  ok = mesh_pri[x] >= (int)sp_nval; break;
                    default: ok = mesh_msb[x];                   break;
                    }
                    if (ok) {
                        unsigned mw = 31u - (unsigned)sp_ccr_tab[mesh_ratio[x]];
                        mc = cc_blend(mc, colour, mw, cc_add);
                    }
                }
                mc = apply_offset(mc, co[6][0], co[6][1], co[6][2]);
                colour = cc_blend(mc, colour, 16u, 0);
                sp_pushed = 1;
            }

            if (spdbg_on) {
                win[l0]++;                      /* 0-3 NBG, 4 RBG0, 5 back, 6 sprite */
                if (sp_pushed && l0 != 6) win[7]++;   /* drawn, then buried */
            }
            out[outi] = colour;
        }
    }

    if (bg_cache_eligible && !bg_cache_hit) {
        bg_cache_sig = bg_sig;
        bg_cache_valid = 1;
    } else if (!bg_cache_eligible) {
        bg_cache_valid = 0;
    }

    if (spdbg_on) {
        static long long every = -1;
        static unsigned long long nth = 0;
        if (every < 0) {
            const char *e = getenv("SATURN_SPDBG");
            every = e ? atoll(e) : 1;
            if (every < 1) every = 1;
        }
        if ((nth % (unsigned long long)every) == 0) {
            printf("[wins] NBG0=%llu(p%d,en%d) NBG1=%llu(p%d,en%d) NBG2=%llu(p%d,en%d) "
                   "NBG3=%llu(p%d,en%d) RBG0=%llu(p%d,en%d) back=%llu | "
                   "sprite drew %llu, lost %llu\n",
                   win[0], prio[0], cfg[0].enabled, win[1], prio[1], cfg[1].enabled,
                   win[2], prio[2], cfg[2].enabled, win[3], prio[3], cfg[3].enabled,
                   win[4], prio_r0, rbg0.enabled, win[5], win[6], win[7]);
            printf("[lyr] CHCTLA=%04X CHCTLB=%04X BGON=%04X MPOFN=%04X PLSZ=%04X CRAOFA=%04X\n",
                   VR(0x28), VR(0x2A), VR(0x20), VR(0x3C), VR(0x3A), VR(0xE4));
            printf("[bmp] TVMD=%04X RAMCTL=%04X | NBG0 bmp=%d col=%u %dx%d base=%05X pal=%u scroll=%d,%d | NBG1 bmp=%d col=%u %dx%d base=%05X\n",
                   VR(0x00), VR(0x0E),
                   cfg[0].bitmap, cfg[0].colours, cfg[0].bw, cfg[0].bh,
                   cfg[0].base, cfg[0].palbank, cfg[0].scroll_x, cfg[0].scroll_y,
                   cfg[1].bitmap, cfg[1].colours, cfg[1].bw, cfg[1].bh,
                   cfg[1].base);
            printf("[cc ] CCCTL=%04X CCRNA=%04X CCRNB=%04X CCRSA=%04X SPCTL=%04X "
                   "PRINA=%04X PRINB=%04X PRISA=%04X PRISB=%04X PRISC=%04X PRISD=%04X "
                   "WCTLA=%04X WCTLB=%04X WCTLC=%04X WCTLD=%04X\n",
                   VR(0xEC), VR(0x108), VR(0x10A), VR(0x100), VR(0xE0),
                   VR(0xF8), VR(0xFA), VR(0xF0), VR(0xF2), VR(0xF4), VR(0xF6),
                   VR(0xD0), VR(0xD2), VR(0xD4), VR(0xD6));
            for (int q = 0; q < 4; q++)
                printf("      NBG%d en=%d bmp=%d col=%u cell=%d pnc=%04X caos=%u plsz=%u "
                       "scroll=%d,%d pages=%05X %05X %05X %05X\n",
                       q, ccfg[q].enabled, ccfg[q].bitmap, ccfg[q].colours,
                       ccfg[q].cellsz, ccfg[q].pnc, ccfg[q].caos, ccfg[q].plsz,
                       ccfg[q].scroll_x, ccfg[q].scroll_y,
                       ccfg[q].page[0], ccfg[q].page[1], ccfg[q].page[2], ccfg[q].page[3]);
            fflush(stdout);
        }
        nth++;
    }
}
