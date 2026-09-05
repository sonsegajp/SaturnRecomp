/* vdp2_cell.c -- render a KNOWN cell layout and check the pixels.
 *
 * Every VDP2 bug this project has chased was diagnosed against a live boot,
 * where the register state and VRAM contents move underneath you. This test
 * builds both by hand, so "is the cell renderer correct?" has an answer that
 * does not depend on how far the BIOS got.
 *
 * Reference model (Sega VDP2 manual s4.6-4.8, cross-checked against Yabause
 * CalcPlaneAddr / ReadPatternData):
 *   plane addr : 1 word 1x1 -> (v & 0x3F) * 0x2000     2x2 -> v * 0x800
 *   page       : always 64x64 CELLS (512x512 px)
 *   1-word PN  : aux0 -> char = (w & 0x3FF) | (supp & 0x1F) << 10, flip w bits 11:10
 *                aux0 2x2 -> char = ((w & 0x3FF) << 2) | (supp & 3) | (supp & 0x1C) << 10
 *   palette    : 16col -> ((w & 0xF000) >> 8) | ((supp & 0xE0) << 3)
 *                256col-> (w & 0x7000) >> 4
 *   char byte  : char * 0x20
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
        printf("  FAIL %-34s got %08X want %08X\n", what, got, want);
        fails++;
    }
}

static void wreg(uint32_t off, uint16_t v) { S.vdp2_reg[off >> 1] = v; }

static void wcram555(unsigned idx, uint16_t rgb)
{
    S.cram[idx * 2 + 0] = (uint8_t)(rgb >> 8);
    S.cram[idx * 2 + 1] = (uint8_t)rgb;
}

int main(void)
{
    static uint32_t fb[352 * 256];
    int w, h;

    memset(&S, 0, sizeof S);

    /* Display on, 320x224, CRAM mode 0 (RGB555 1024 entries). */
    wreg(0x00, 0x8000);
    wreg(0x0E, 0x0000);

    /* NBG0 only: cell format, 16 colour, 1x1 characters. */
    wreg(0x20, 0x0001);        /* BGON: NBG0 */
    wreg(0x28, 0x0000);        /* CHCTLA: N0 cell, 1x1, 16 colour */
    wreg(0x30, 0x8000);        /* PNCN0: 1-word, aux 0, no supplement */
    wreg(0x3A, 0x0000);        /* PLSZ: 1x1 page */
    wreg(0x3C, 0x0000);        /* MPOFN */
    wreg(0x40, 0x0002);        /* MPABN0: plane A = map value 2 */
    wreg(0xF8, 0x0001);        /* PRINA (0xF8 per Ymir/Mednafen): NBG0 priority 1 */

    /* Plane A therefore starts at 2 * 0x2000 = 0x4000. */
    {
        uint32_t page = 0x4000;
        /* Pattern (0,0): character 5, palette 3. */
        S.vdp2_vram[page + 0] = 0x30;      /* palette 3 in bits 15-12 */
        S.vdp2_vram[page + 1] = 0x05;      /* character number 5 */

        /* Character 5 lives at 5 * 0x20 = 0xA0. 4bpp: two dots per byte.
         * Fill row 0 with index 1,2,1,2..., rest with index 0 (transparent). */
        for (int i = 0; i < 4; i++)
            S.vdp2_vram[0xA0 + i] = 0x12;
    }

    /* Palette 3, 16 colour -> CRAM base = 3 * 16 = 48. */
    wcram555(48 + 1, 0x7C00);              /* index 1 -> blue  (B=31) */
    wcram555(48 + 2, 0x001F);              /* index 2 -> red   (R=31) */

    vdp2_display_size(&S, &w, &h);
    ck("display width", (uint32_t)w, 320);
    ck("display height", (uint32_t)h, 224);

    vdp2_render(&S, fb, w, h, 0);

    /* Row 0 of the top-left character: alternating blue / red, then nothing. */
    ck("pixel(0,0) blue",  fb[0] & 0xFFFFFF, 0x0000FF);
    ck("pixel(1,0) red",   fb[1] & 0xFFFFFF, 0xFF0000);
    ck("pixel(2,0) blue",  fb[2] & 0xFFFFFF, 0x0000FF);
    ck("pixel(7,0) red",   fb[7] & 0xFFFFFF, 0xFF0000);
    /* Row 1 is index 0 everywhere -> transparent -> back screen (black). */
    ck("pixel(0,1) clear", fb[w] & 0xFFFFFF, 0x000000);
    /* Next character over is pattern (1,0), which we left as 0 -> char 0. */
    ck("pixel(8,0) clear", fb[8] & 0xFFFFFF, 0x000000);

    /* ---- line scroll on a cell-format NBG (VDP2 manual s4.10) -----------
     * Resort Island enables line X, line Y, and line zoom on a cell-format
     * NBG0.  The bitmap sampler consumed the decoded table while the cell
     * sampler ignored it, exposing unused map cells as repeating columns.
     * A +1-dot line-X entry must shift pixel zero from blue to red. */
    wreg(0x9A, 0x0002);              /* SCRCTL: NBG0 line-scroll X */
    wreg(0xA0, 0x0000);              /* LSTA0U/L: table at 0x10000 */
    wreg(0xA2, 0x8000);
    S.vdp2_vram[0x10000] = 0x00;     /* 0x00010000 -> 1.0 in 11.8 */
    S.vdp2_vram[0x10001] = 0x01;
    S.vdp2_vram[0x10002] = 0x00;
    S.vdp2_vram[0x10003] = 0x00;
    vdp2_render(&S, fb, w, h, 0);
    ck("cell NBG applies line-scroll X", fb[0] & 0xFFFFFF, 0xFF0000);
    wreg(0x9A, 0x0000);

    /* ---- colour offset (VDP2 manual s10.6) --------------------------------
     * CLOFEN enables it per layer, CLOFSL picks offset set A or B, and
     * COAR/COAG/COAB hold a 9-bit signed value added to each channel after
     * lookup, clamped to 0..255. The BIOS writes all eight of these registers
     * and we implemented none of them, so every layer rendered unoffset --
     * measured against a reference capture of the boot logo, mean luma 31.2
     * against 127.2, four times too dark.
     *
     * pixel(0,0) is NBG0 and reads 0x0000FF, so one probe exercises both
     * clamps: red sits at the bottom of its range and blue at the top. */
    wreg(0x110, 0x0001);              /* CLOFEN: NBG0            */
    wreg(0x112, 0x0000);              /* CLOFSL: offset set A    */
    wreg(0x114, 0x0040);              /* COAR = +64              */
    wreg(0x116, 0x0040);              /* COAG = +64              */
    wreg(0x118, 0x0040);              /* COAB = +64              */
    vdp2_render(&S, fb, w, h, 0);
    ck("offset A adds to R",      (fb[0] >> 16) & 0xFF, 64);
    ck("offset A adds to G",      (fb[0] >> 8)  & 0xFF, 64);
    ck("offset A clamps B high",   fb[0]        & 0xFF, 255);

    wreg(0x114, 0x01C0);              /* -64 in 9-bit two's complement */
    wreg(0x116, 0x01C0);
    wreg(0x118, 0x01C0);
    vdp2_render(&S, fb, w, h, 0);
    ck("negative offset clamps R low", (fb[0] >> 16) & 0xFF, 0);
    ck("negative offset subtracts B",   fb[0]        & 0xFF, 191);

    wreg(0x112, 0x0001);              /* CLOFSL: route NBG0 to set B */
    wreg(0x11A, 0x0020); wreg(0x11C, 0x0020); wreg(0x11E, 0x0020);
    vdp2_render(&S, fb, w, h, 0);
    ck("CLOFSL selects offset set B", (fb[0] >> 16) & 0xFF, 32);

    wreg(0x110, 0x0002);              /* enable NBG1 only: NBG0 untouched */
    vdp2_render(&S, fb, w, h, 0);
    ck("layer with enable bit clear", fb[0] & 0xFFFFFF, 0x0000FF);
    wreg(0x110, 0x0000);

    /* ---- transparent mesh over an existing VDP1 sprite -----------------
     * The mesh colour lives in a separate framebuffer.  Compositing opaque
     * blue over opaque green must preserve both and produce cyan; blending
     * the mesh against VDP2's black back screen instead produces dark blue. */
    memset(S.vdp1_fb[1], 0, VDP1_FB_SZ);
    memset(S.vdp1_meshfb[1], 0, VDP1_FB_SZ);
    S.vdp1_fb[1][0] = 0x83; S.vdp1_fb[1][1] = 0xE0;       /* green */
    S.vdp1_meshfb[1][0] = 0xFC; S.vdp1_meshfb[1][1] = 0x00; /* blue */
    wreg(0x20, 0x0000);                 /* backgrounds off */
    wreg(0xE0, 0x0020);                 /* SPCLMD: direct RGB */
    wreg(0xF0, 0x0003);                 /* sprite priority 3 */
    vdp2_render(&S, fb, w, h, 0);
    ck("mesh blends with VDP1 underneath", fb[0] & 0xFFFFFF, 0x007F7F);

    /* ---- per-character special priority masks an equal-priority sprite ---
     * Sonic 3D Blast uses this exact arrangement for foreground scenery:
     * normal NBG0 dots sit at priority 2, marked characters raise it to 3,
     * and a VDP1 actor lowered to priority 2 is hidden only by those marked
     * characters.  On equal priority the sprite wins, so both halves matter. */
    memset(S.vdp1_fb[1], 0, VDP1_FB_SZ);
    memset(S.vdp1_meshfb[1], 0, VDP1_FB_SZ);
    S.vdp1_fb[1][0] = 0x80; S.vdp1_fb[1][1] = 0x1F; /* direct-RGB red */
    wreg(0x20, 0x0001);                 /* NBG0 on */
    wreg(0x28, 0x0000);                 /* 16-colour cell */
    wreg(0x30, 0x8200);                 /* 1-word PN + supplementary PR bit */
    wreg(0xEA, 0x0001);                 /* NBG0 priority per character */
    wreg(0xF8, 0x0002);                 /* NBG0 base priority 2 */
    wreg(0xE0, 0x0020);                 /* mixed sprite format */
    wreg(0xF0, 0x0002);                 /* direct RGB uses sprite slot 0 = 2 */
    vdp2_render(&S, fb, w, h, 0);
    ck("special-priority BG masks sprite", fb[0] & 0xFFFFFF, 0x0000FF);

    wreg(0x30, 0x8000);                 /* same character, PR bit clear */
    vdp2_render(&S, fb, w, h, 0);
    ck("equal priority lets sprite win", fb[0] & 0xFFFFFF, 0xFF0000);

    if (fails == 0) printf("PASS  vdp2 cell renderer (%d checks)\n", 18);
    else            printf("FAIL  vdp2 cell renderer: %d\n", fails);
    return fails ? 1 : 0;
}
