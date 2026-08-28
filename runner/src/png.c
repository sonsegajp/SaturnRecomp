/* png.c -- minimal PNG writer for framebuffer captures.
 *
 * Deliberately dependency-free: a zlib stream made of STORED (uncompressed)
 * deflate blocks, so we need no compressor. Big files, but these are 320x224
 * screenshots and the point is to be able to look at what the machine drew.
 */
#include "saturn.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static uint32_t crc_tab[256];
static int      crc_ready;

static void crc_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : (c >> 1);
        crc_tab[n] = c;
    }
    crc_ready = 1;
}

static uint32_t crc32_buf(const uint8_t *p, size_t n, uint32_t c)
{
    if (!crc_ready) crc_init();
    for (size_t i = 0; i < n; i++) c = crc_tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}

static void be32(uint8_t *d, uint32_t v)
{
    d[0] = (uint8_t)(v >> 24); d[1] = (uint8_t)(v >> 16);
    d[2] = (uint8_t)(v >> 8);  d[3] = (uint8_t)v;
}

static void chunk(FILE *f, const char *tag, const uint8_t *data, uint32_t n)
{
    uint8_t hdr[8], crcb[4];
    uint32_t c;
    be32(hdr, n);
    memcpy(hdr + 4, tag, 4);
    fwrite(hdr, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    c = crc32_buf((const uint8_t *)tag, 4, 0xFFFFFFFFu);
    if (n) c = crc32_buf(data, n, c);
    be32(crcb, c ^ 0xFFFFFFFFu);
    fwrite(crcb, 1, 4, f);
}

int png_write(const char *path, const uint32_t *argb, int w, int h)
{
    FILE *f = fopen(path, "wb");
    uint8_t ihdr[13];
    uint8_t *raw, *z;
    size_t rawn, zn, pos = 0, off = 0;
    uint32_t a = 1, b = 0;
    static const uint8_t sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };

    if (!f) return -1;
    fwrite(sig, 1, 8, f);

    be32(ihdr, (uint32_t)w); be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);

    /* Raw scanlines: filter byte 0 then RGB triples. */
    rawn = (size_t)h * (1 + (size_t)w * 3);
    raw = (uint8_t *)malloc(rawn);
    if (!raw) { fclose(f); return -1; }
    for (int y = 0; y < h; y++) {
        raw[pos++] = 0;
        for (int x = 0; x < w; x++) {
            uint32_t p = argb[y * w + x];
            raw[pos++] = (uint8_t)(p >> 16);
            raw[pos++] = (uint8_t)(p >> 8);
            raw[pos++] = (uint8_t)p;
        }
    }
    for (size_t i = 0; i < rawn; i++) {
        a = (a + raw[i]) % 65521u;
        b = (b + a) % 65521u;
    }

    /* zlib: 2-byte header, stored blocks of <=65535, 4-byte adler. */
    zn = 2 + ((rawn + 65534) / 65535) * 5 + rawn + 4;
    z = (uint8_t *)malloc(zn);
    if (!z) { free(raw); fclose(f); return -1; }
    z[0] = 0x78; z[1] = 0x01;
    pos = 2;
    while (off < rawn) {
        size_t n = rawn - off; if (n > 65535) n = 65535;
        z[pos++] = (off + n >= rawn) ? 1 : 0;
        z[pos++] = (uint8_t)(n & 0xFF);
        z[pos++] = (uint8_t)(n >> 8);
        z[pos++] = (uint8_t)(~n & 0xFF);
        z[pos++] = (uint8_t)((~n >> 8) & 0xFF);
        memcpy(z + pos, raw + off, n);
        pos += n; off += n;
    }
    be32(z + pos, (b << 16) | a); pos += 4;
    chunk(f, "IDAT", z, (uint32_t)pos);
    chunk(f, "IEND", NULL, 0);

    free(raw); free(z); fclose(f);
    return 0;
}
