/* vdp1_quad.c -- VDP1 rasteriser against hand-built command lists.
 *
 * Every VDP1 defect this project hit was diagnosed against a live boot, where
 * texture data, palettes and command lists all move underneath the question.
 * These commands are built by hand, so "what does one sprite put in the
 * framebuffer?" has a fixed answer.
 *
 * The one that motivated this file: palette-coded sprites were written to the
 * framebuffer with bit 15 forced on. Bit 15 is the mixed-mode RGB flag on the
 * way back OUT, so every palette sprite was read back as RGB555 with its
 * palette index in the red channel -- the CD player rendered as red noise.
 * The framebuffer must hold CMDCOLR + texel index, untouched.
 */
#include "saturn.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static saturn S;
static int fails;

static void ck(const char *what, uint32_t got, uint32_t want)
{
    if (got != want) {
        printf("  FAIL %-44s got %04X want %04X\n", what, got, want);
        fails++;
    }
}

static void v16(uint32_t a, uint16_t v)
{
    S.vdp1_vram[a & (VDP1_VRAM_SZ - 1)]       = (uint8_t)(v >> 8);
    S.vdp1_vram[(a + 1) & (VDP1_VRAM_SZ - 1)] = (uint8_t)v;
}

static uint16_t fbw(int x, int y)
{
    const uint8_t *fb = S.vdp1_fb[S.fb_draw];
    uint32_t o = (uint32_t)(y * 512 + x) * 2u;
    return (uint16_t)((fb[o] << 8) | fb[o + 1]);
}

static uint16_t meshw(int x, int y)
{
    const uint8_t *fb = S.vdp1_meshfb[S.fb_draw];
    uint32_t o = (uint32_t)(y * 512 + x) * 2u;
    return (uint16_t)((fb[o] << 8) | fb[o + 1]);
}

/* One command is 16 words at `at`. */
static void cmd(uint32_t at, uint16_t ctrl, uint16_t pmod, uint16_t colr,
                uint16_t srca, uint16_t size,
                int16_t xa, int16_t ya, int16_t xc, int16_t yc)
{
    v16(at + 0x00, ctrl);
    v16(at + 0x02, 0);                  /* link */
    v16(at + 0x04, pmod);
    v16(at + 0x06, colr);
    v16(at + 0x08, srca);
    v16(at + 0x0A, size);
    v16(at + 0x0C, (uint16_t)xa);
    v16(at + 0x0E, (uint16_t)ya);
    v16(at + 0x10, (uint16_t)xc);       /* B.x for distorted */
    v16(at + 0x12, (uint16_t)yc);       /* B.y */
    v16(at + 0x14, (uint16_t)xc);       /* C */
    v16(at + 0x16, (uint16_t)yc);
    v16(at + 0x18, (uint16_t)xa);       /* D */
    v16(at + 0x1A, (uint16_t)yc);
    v16(at + 0x1C, 0);                  /* grda */
    v16(at + 0x1E, 0);
}

/* Four independent vertices, for quads that are not axis-aligned rectangles. */
static void cmd4(uint32_t at, uint16_t ctrl, uint16_t pmod, uint16_t colr,
                 int16_t xa, int16_t ya, int16_t xb, int16_t yb,
                 int16_t xc, int16_t yc, int16_t xd, int16_t yd)
{
    v16(at + 0x00, ctrl);  v16(at + 0x02, 0);
    v16(at + 0x04, pmod);  v16(at + 0x06, colr);
    v16(at + 0x08, 0);     v16(at + 0x0A, 0);
    v16(at + 0x0C, (uint16_t)xa); v16(at + 0x0E, (uint16_t)ya);
    v16(at + 0x10, (uint16_t)xb); v16(at + 0x12, (uint16_t)yb);
    v16(at + 0x14, (uint16_t)xc); v16(at + 0x16, (uint16_t)yc);
    v16(at + 0x18, (uint16_t)xd); v16(at + 0x1A, (uint16_t)yd);
    v16(at + 0x1C, 0);     v16(at + 0x1E, 0);
}

static void run_list(void)
{
    memset(S.vdp1_fb[S.fb_draw], 0, VDP1_FB_SZ);
    memset(S.vdp1_meshfb[S.fb_draw], 0, VDP1_FB_SZ);
    vdp1_execute(&S);
}

int main(void)
{
    printf("VDP1 rasteriser (hand-built command lists)\n");
    memset(&S, 0, sizeof S);
    sh2_reset(&S.master, &S, 0, 0x06004000u, 0x06100000u);
    S.cur = &S.master;

    /* An 8x8 16-colour texture at VRAM 0x10000: row y holds index y+1 in
     * every pixel (two texels per byte), so sampling errors show up as the
     * wrong row value. Index 0 would be transparent; none here. */
    for (int y = 0; y < 8; y++)
        for (int xb = 0; xb < 4; xb++) {
            uint8_t idx = (uint8_t)(y + 1);
            S.vdp1_vram[0x10000 + y * 4 + xb] = (uint8_t)((idx << 4) | idx);
        }

    /* ---- 1. normal sprite, 16-colour BANK (pmod mode 0) ---------------- */
    /* CMDSRCA is address/8; CMDSIZE = (width/8)<<8 | height. */
    cmd(0x0000, 0x0000, 0x0000, 0x0100, 0x10000 / 8, (1 << 8) | 8,
        10, 20, 0, 0);
    v16(0x20, 0x8000);                   /* END */
    run_list();

    /* Bank mode: framebuffer word = CMDCOLR + index. Bit 15 must be CLEAR. */
    ck("bank texel row0 = colr+1", fbw(10, 20), 0x0101);
    ck("bank texel row7 = colr+8", fbw(10, 27), 0x0108);
    ck("bank bit15 clear",         fbw(13, 23) & 0x8000u, 0);
    ck("outside quad untouched",   fbw(30, 30), 0);

    /* ---- 2. transparent texels leave the framebuffer alone ------------- */
    S.vdp1_vram[0x10000 + 0 * 4 + 0] = 0x01;   /* row 0: texel0 idx 0 */
    run_list();
    ck("transparent texel skipped", fbw(10, 20), 0);
    ck("neighbour still drawn",     fbw(11, 20), 0x0101);
    S.vdp1_vram[0x10000 + 0 * 4 + 0] = 0x11;

    /* ---- 3. 16-colour LUT (pmod mode 1): entries pass through verbatim - */
    for (int i = 0; i < 16; i++)
        v16(0x18000 + i * 2, (uint16_t)(0x8000 | (i * 3)));   /* RGB-ish LUT */
    /* CMDCOLR = LUT address / 8. */
    cmd(0x0000, 0x0000, 0x0008, 0x18000 / 8, 0x10000 / 8, (1 << 8) | 8,
        40, 20, 0, 0);
    v16(0x20, 0x8000);
    run_list();
    ck("LUT row0 verbatim", fbw(40, 20), 0x8000 | (1 * 3));
    ck("LUT row7 verbatim", fbw(40, 27), 0x8000 | (8 * 3));

    /* ---- 4. RGB555 direct (pmod mode 5) -------------------------------- */
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            v16(0x11000 + (y * 8 + x) * 2, (uint16_t)(0x8000 | (y << 5) | x));
    cmd(0x0000, 0x0000, 0x0028, 0x0000, 0x11000 / 8, (1 << 8) | 8,
        60, 20, 0, 0);
    v16(0x20, 0x8000);
    run_list();
    ck("RGB texel (0,0)", fbw(60, 20), 0x8000 | 0);
    ck("RGB texel (7,7)", fbw(67, 27), 0x8000 | (7 << 5) | 7);

    /* ---- 5. distorted sprite with corners = normal placement ------------ */
    /* A=(80,20) C=(87,27): degenerate distortion must equal the normal case. */
    cmd(0x0000, 0x0002, 0x0000, 0x0200, 0x10000 / 8, (1 << 8) | 8,
        80, 20, 87, 27);
    v16(0x20, 0x8000);
    run_list();
    ck("distorted row0", fbw(80, 20), 0x0201);
    ck("distorted row7", fbw(80, 27), 0x0208);

    /* ---- 6. flat polygon (cmd 4): CMDCOLR verbatim ---------------------- */
    /* 0x25A5 has bit 15 CLEAR: a rasteriser that ORs the MSB in (the old
     * bug) turns it into 0xA5A5 and fails. The BIOS player erases the screen
     * with a full-frame colr=0x0000 polygon that must stay sprite-transparent. */
    cmd(0x0000, 0x0004, 0x0000, 0x25A5, 0, 0, 100, 20, 107, 27);
    v16(0x20, 0x8000);
    run_list();
    ck("polygon colour verbatim", fbw(103, 23), 0x25A5);

    /* ---- 7. no seam between two quads sharing a slanted edge ------------
     * The span walker used to be `px = lx + (rx-lx)*j/n`, and C integer
     * division truncates TOWARD ZERO -- so the minor-axis rounding was biased
     * one way for a left-to-right span and the other way for a right-to-left
     * one. Two quads meeting at a shared, non-axis-aligned edge then rounded
     * apart and left a one-pixel hole running down the join, which is what
     * "visible gaps in geometry" looked like on screen. Ymir's LineStepper
     * starts its Bresenham accumulator at `adx + 1` -- half a step -- so it
     * rounds to nearest in both directions and the two quads abut.
     *
     * The assertion is deliberately about CONTIGUITY rather than exact pixel
     * positions: whatever the rasteriser picks, a row covered by a strip of
     * touching quads must not contain an unpainted pixel between its first
     * and last painted one. */
    cmd4(0x0000, 0x0004, 0x0000, 0x1111,
         20, 100,  60, 110,  60, 140,  20, 130);      /* left  quad */
    cmd4(0x0020, 0x0004, 0x0000, 0x2222,
         60, 110, 100, 105, 100, 135,  60, 140);      /* right quad, shares B-C */
    v16(0x02, 0x0020 >> 3);                            /* link to the 2nd cmd */
    v16(0x40, 0x8000);                                 /* END */
    run_list();
    v16(0x02, 0);                                      /* restore for later runs */

    {
        /* The precise assertion: on any row where BOTH quads drew, there must
         * be no unpainted pixel between the last left-quad pixel and the first
         * right-quad one. (A plain "is the row contiguous" test would be
         * wrong -- rows above the left quad's top edge have a legitimate wedge
         * of background between the two quads.) */
        /* Only rows spanned by the SHARED edge B(60,110)-C(60,140). Above
         * y=110 the right quad's top edge rises away from the left quad's
         * corner, so background between them there is correct geometry. */
        int holes = 0, rows = 0;
        for (int y = 110; y <= 140; y++) {
            int lLast = -1, rFirst = -1;
            for (int x = 0; x < 512; x++) {
                if (fbw(x, y) == 0x1111) lLast = x;
                if (fbw(x, y) == 0x2222 && rFirst < 0) rFirst = x;
            }
            if (lLast < 0 || rFirst < 0 || rFirst < lLast) continue;
            rows++;
            for (int x = lLast + 1; x < rFirst; x++)
                if (!fbw(x, y)) holes++;
        }
        if (getenv("SEAMMAP")) for (int y = 100; y <= 142; y++) {
            printf("%3d ", y);
            for (int x = 15; x < 110; x++) {
                uint16_t v = fbw(x, y);
                putchar(v == 0x1111 ? 76 : v == 0x2222 ? 82 : v ? 63 : 46);
            }
            putchar(10);
        }
        ck("seam rows covered (>=25)", (uint32_t)(rows >= 25), 1);
        ck("no gap along the shared quad edge", (uint32_t)holes, 0);
    }

    /* ---- 8. hardware mesh is a checkerboard write mask -----------------
     * Canonical Saturn/Ymir output writes the mesh colour on one parity and
     * leaves the existing VDP1 pixel untouched on the other. Transparent mesh
     * blending is an explicit enhancement, not the default renderer mode. */
    cmd4(0x0000, 0x0004, 0x0100, 0xFC00,
         120, 20, 127, 20, 127, 27, 120, 27);
    v16(0x20, 0x8000);
    memset(S.vdp1_fb[S.fb_draw], 0, VDP1_FB_SZ);
    memset(S.vdp1_meshfb[S.fb_draw], 0, VDP1_FB_SZ);
    {
        uint32_t o = (uint32_t)(23 * 512 + 123) * 2u;
        S.vdp1_fb[S.fb_draw][o] = 0x83;
        S.vdp1_fb[S.fb_draw][o + 1] = 0xE0;          /* opaque green */
        o += 2u;
        S.vdp1_fb[S.fb_draw][o] = 0x83;
        S.vdp1_fb[S.fb_draw][o + 1] = 0xE0;
    }
    vdp1_execute(&S);
    ck("mesh writes even checker pixel", fbw(123, 23), 0xFC00);
    ck("mesh preserves odd checker pixel", fbw(124, 23), 0x83E0);
    ck("canonical mesh uses no blend buffer", meshw(123, 23), 0x0000);

    /* ---- 9. user clipping must not replace the system clip -------------
     * Command 8 only updates the user rectangle.  A missing break in the
     * command dispatcher also copied its lower-right corner into the system
     * clip, silently removing later foreground sprites even when their PMOD
     * did not enable user clipping. */
    cmd(0x0000, 0x0009, 0, 0, 0, 0, 0, 0, 200, 200);   /* system clip */
    cmd(0x0020, 0x0008, 0, 0, 0, 0, 10, 10, 20, 20);   /* user clip */
    cmd4(0x0040, 0x0004, 0, 0x3456,
         100, 100, 107, 100, 107, 107, 100, 107);      /* no user clip */
    v16(0x0060, 0x8000);
    run_list();
    ck("user clip leaves system clip intact", fbw(103, 103), 0x3456);

    /* Gouraud preserves the incoming palette/RGB flag even when it changes
     * the five-bit channels. Neutral gray must not turn a palette code into RGB. */
    cmd4(0, 4, 4, 0x2345, 20, 20, 27, 20, 27, 27, 20, 27);
    v16(0x1C, 0x1000);v16(0x20, 0x8000);
    for(unsigned i=0;i<4;i++)v16(0x8000+i*2,0x4210);
    run_list();ck("Gouraud preserves palette MSB clear",fbw(23,23),0x2345);
    v16(6,0xA345);run_list();ck("Gouraud preserves RGB MSB set",fbw(23,23),0xA345);
    for(unsigned i=0;i<4;i++)v16(0x8000+i*2,0x4211);
    v16(6,0x2345);run_list();ck("Gouraud adjusts channels without forcing RGB",fbw(23,23),0x2346);

    if (fails) { printf("FAILED: %d check(s)\n", fails); return 1; }
    printf("PASS: vdp1 rasteriser checks\n");
    return 0;
}
