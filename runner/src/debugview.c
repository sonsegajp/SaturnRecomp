/* debugview.c — what the video hardware currently holds, drawn so you can see it.
 *
 * Until the VDP2 background renderer exists, a plain composite of the VDP1
 * framebuffer is a black rectangle and tells you nothing. This panel instead
 * shows the machine's actual graphics state side by side:
 *
 *   - the VDP1 draw framebuffer (what sprites/polygons have been rasterised)
 *   - VDP2 colour RAM, all 2048 entries, as a palette grid
 *   - VDP2 VRAM as a tile sheet, so you can watch graphics arrive
 *   - a text readout of the layer registers and live counters
 *
 * It is a diagnostic, but it is the honest picture: when the game finally
 * uploads real tiles and enables display, they appear here first.
 */
#include "saturn.h"
#include <string.h>

/* 5x7 bitmap font, digits + uppercase + a few symbols. One byte per column. */
static const unsigned char FONT[96][5] = {
    ['-'-32]={0x08,0x08,0x08,0x08,0x08}, [' '-32]={0,0,0,0,0},
    ['.'-32]={0x00,0x60,0x60,0x00,0x00}, [':'-32]={0x00,0x36,0x36,0x00,0x00},
    ['/'-32]={0x60,0x18,0x06,0x01,0x00}, ['x'-32]={0x44,0x28,0x10,0x28,0x44},
    ['0'-32]={0x3E,0x51,0x49,0x45,0x3E}, ['1'-32]={0x00,0x42,0x7F,0x40,0x00},
    ['2'-32]={0x42,0x61,0x51,0x49,0x46}, ['3'-32]={0x21,0x41,0x45,0x4B,0x31},
    ['4'-32]={0x18,0x14,0x12,0x7F,0x10}, ['5'-32]={0x27,0x45,0x45,0x45,0x39},
    ['6'-32]={0x3C,0x4A,0x49,0x49,0x30}, ['7'-32]={0x01,0x71,0x09,0x05,0x03},
    ['8'-32]={0x36,0x49,0x49,0x49,0x36}, ['9'-32]={0x06,0x49,0x49,0x29,0x1E},
    ['A'-32]={0x7E,0x11,0x11,0x11,0x7E}, ['B'-32]={0x7F,0x49,0x49,0x49,0x36},
    ['C'-32]={0x3E,0x41,0x41,0x41,0x22}, ['D'-32]={0x7F,0x41,0x41,0x22,0x1C},
    ['E'-32]={0x7F,0x49,0x49,0x49,0x41}, ['F'-32]={0x7F,0x09,0x09,0x09,0x01},
    ['G'-32]={0x3E,0x41,0x49,0x49,0x7A}, ['H'-32]={0x7F,0x08,0x08,0x08,0x7F},
    ['I'-32]={0x00,0x41,0x7F,0x41,0x00}, ['J'-32]={0x20,0x40,0x41,0x3F,0x01},
    ['K'-32]={0x7F,0x08,0x14,0x22,0x41}, ['L'-32]={0x7F,0x40,0x40,0x40,0x40},
    ['M'-32]={0x7F,0x02,0x0C,0x02,0x7F}, ['N'-32]={0x7F,0x04,0x08,0x10,0x7F},
    ['O'-32]={0x3E,0x41,0x41,0x41,0x3E}, ['P'-32]={0x7F,0x09,0x09,0x09,0x06},
    ['Q'-32]={0x3E,0x41,0x51,0x21,0x5E}, ['R'-32]={0x7F,0x09,0x19,0x29,0x46},
    ['S'-32]={0x46,0x49,0x49,0x49,0x31}, ['T'-32]={0x01,0x01,0x7F,0x01,0x01},
    ['U'-32]={0x3F,0x40,0x40,0x40,0x3F}, ['V'-32]={0x1F,0x20,0x40,0x20,0x1F},
    ['W'-32]={0x7F,0x20,0x18,0x20,0x7F}, ['X'-32]={0x63,0x14,0x08,0x14,0x63},
    ['Y'-32]={0x03,0x04,0x78,0x04,0x03}, ['Z'-32]={0x61,0x51,0x49,0x45,0x43},
};

static void px(uint32_t *out, int W, int H, int x, int y, uint32_t c)
{
    if (x >= 0 && y >= 0 && x < W && y < H) out[y * W + x] = c;
}

static void text(uint32_t *out, int W, int H, int x, int y,
                 const char *str, uint32_t c)
{
    for (; *str; str++) {
        unsigned ch = (unsigned char)*str;
        if (ch >= 32 && ch < 128) {
            const unsigned char *g = FONT[ch - 32];
            for (int col = 0; col < 5; col++)
                for (int row = 0; row < 7; row++)
                    if (g[col] & (1u << row)) px(out, W, H, x + col, y + row, c);
        }
        x += 6;
    }
}

static void hex32(char *b, uint32_t v, int digits)
{
    static const char *H = "0123456789ABCDEF";
    for (int i = 0; i < digits; i++)
        b[digits - 1 - i] = H[(v >> (i * 4)) & 0xF];
    b[digits] = 0;
}

static void dec(char *b, uint64_t v)
{
    char t[24];
    int n = 0;
    if (!v) { b[0] = '0'; b[1] = 0; return; }
    while (v && n < 23) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (int i = 0; i < n; i++) b[i] = t[n - 1 - i];
    b[n] = 0;
}

static uint32_t rgb555(uint16_t p)
{
    uint32_t r = (p) & 0x1F, g = (p >> 5) & 0x1F, b = (p >> 10) & 0x1F;
    return 0xFF000000u | (((r << 3) | (r >> 2)) << 16)
                       | (((g << 3) | (g >> 2)) << 8)
                       |  ((b << 3) | (b >> 2));
}

/* Render the whole panel into `out` (W x H, ARGB8888). */
void debugview_render(saturn *s, uint32_t *out, int W, int H)
{
    char buf[80], num[24];

    for (int i = 0; i < W * H; i++) out[i] = 0xFF101014u;

    /* ---- composited VDP2 + VDP1 output, top-left, at display size ---- */
    {
        static uint32_t frame[704 * 512];
        int dw, dh;
        vdp2_display_size(s, &dw, &dh);
        if (dw > 512) dw = 512;
        if (dh > 256) dh = 256;
        /* force_on: render even with the display-enable bit clear, so the
         * panel shows what the hardware holds while a title is still setting
         * itself up. The label below says which state it is really in. */
        vdp2_render(s, frame, dw, dh, 1);
        for (int y = 0; y < dh; y++)
            for (int x = 0; x < dw; x++)
                px(out, W, H, x, y + 12, frame[y * dw + x]);
        text(out, W, H, 2, 2, "COMPOSITE  VDP2 + VDP1", 0xFF80C0FF);
    }

    /* ---- VDP2 colour RAM, 2048 entries as a 64x32 grid of 4x4 swatches ---- */
    {
        int ox = 524, oy = 12;
        text(out, W, H, ox, 2, "VDP2 COLOUR RAM", 0xFF80C0FF);
        for (int i = 0; i < 2048; i++) {
            uint16_t c = (uint16_t)((s->cram[i * 2] << 8) | s->cram[i * 2 + 1]);
            int cx = ox + (i % 64) * 4, cy = oy + (i / 64) * 4;
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++)
                    px(out, W, H, cx + x, cy + y, rgb555(c));
        }
    }

    /* ---- VDP2 VRAM as an 8bpp tile sheet, so uploads are visible ---- */
    {
        int ox = 524, oy = 160;
        text(out, W, H, ox, 150, "VDP2 VRAM", 0xFF80C0FF);
        for (int ty = 0; ty < 110; ty++)
            for (int tx = 0; tx < 256; tx++) {
                uint32_t o = (uint32_t)(ty * 256 + tx) * 8u;
                uint8_t  v = s->vdp2_vram[o & (VDP2_VRAM_SZ - 1)];
                uint32_t c = v ? (0xFF000000u | (v * 0x010101u)) : 0xFF181820u;
                px(out, W, H, ox + tx, oy + ty, c);
            }
    }

    /* ---- live state readout ---- */
    {
        int x = 2, y = 276;
        uint16_t tvmd = s->vdp2_reg[0], bgon = s->vdp2_reg[0x20 >> 1];
        unsigned long nz = 0;
        for (unsigned k = 0; k < VDP2_VRAM_SZ; k++) if (s->vdp2_vram[k]) nz++;

        strcpy(buf, "DISPLAY "); strcat(buf, (tvmd & 0x8000) ? "ON" : "OFF");
        text(out, W, H, x, y, buf, (tvmd & 0x8000) ? 0xFF60FF60 : 0xFFFF6060);

        strcpy(buf, "TVMD "); hex32(num, tvmd, 4); strcat(buf, num);
        strcat(buf, "  BGON "); hex32(num, bgon, 4); strcat(buf, num);
        text(out, W, H, x, y + 10, buf, 0xFFC0C0C0);

        strcpy(buf, "NBG ");
        for (int i = 0; i < 4; i++) { num[0] = (char)('0' + i); num[1] = 0;
            strcat(buf, (bgon & (1u << i)) ? num : "-"); strcat(buf, " "); }
        text(out, W, H, x, y + 20, buf, 0xFFC0C0C0);

        strcpy(buf, "VRAM BYTES "); dec(num, nz); strcat(buf, num);
        text(out, W, H, x, y + 30, buf, 0xFFC0C0C0);

        strcpy(buf, "VDP1 LISTS "); dec(num, s->vdp1_lists); strcat(buf, num);
        strcat(buf, "  CMDS "); dec(num, s->vdp1_commands); strcat(buf, num);
        text(out, W, H, x, y + 40, buf, 0xFFC0C0C0);

        strcpy(buf, "VDP1 PIXELS "); dec(num, s->vdp1_pixels); strcat(buf, num);
        text(out, W, H, x, y + 50, buf, 0xFFC0C0C0);

        strcpy(buf, "SCU DMA "); dec(num, s->scu_dma_transfers); strcat(buf, num);
        strcat(buf, " BYTES "); dec(num, s->scu_dma_bytes); strcat(buf, num);
        text(out, W, H, x, y + 60, buf, 0xFFC0C0C0);

        strcpy(buf, "IRQS "); dec(num, s->irqs_taken); strcat(buf, num);
        text(out, W, H, x, y + 70, buf, 0xFFC0C0C0);

        strcpy(buf, "CD CMDS "); dec(num, (uint64_t)s->ncdcmd); strcat(buf, num);
        strcat(buf, "  BIOS CALLS "); dec(num, (uint64_t)s->nbioscall);
        strcat(buf, num);
        text(out, W, H, x, y + 80, buf, 0xFFC0C0C0);
    }
}
