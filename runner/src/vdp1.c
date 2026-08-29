/* vdp1.c — VDP1 command list execution and rasterisation.
 *
 * VDP1 is not a "3D chip". It walks a linked list of 32-byte command tables in
 * its own VRAM and rasterises each one into the draw framebuffer. Everything
 * the Saturn draws in 3D is a *distorted sprite*: a textured quadrilateral with
 * four independent corners. See docs/HARDWARE.md §5.
 *
 * Coverage here: local/system/user coordinate commands, polygon and polyline
 * and line, and all three sprite forms, with colour bank / CLUT / RGB texture
 * modes, end-code transparency, half-brightness and mesh. Gouraud interpolation
 * is applied per-vertex across the quad.
 *
 * The rasteriser walks the quad the way the hardware does: it steps along the
 * two "vertical" edges A->D and B->C together, and for each step draws the
 * horizontal span between the two interpolated points, sampling the texture on
 * a matching parametric grid. That reproduces the Saturn's characteristic
 * forward texture mapping -- including its warping on non-planar quads -- which
 * a triangle rasteriser would not.
 */
#include "saturn.h"
#include "vulkan_renderer.h"
#include <string.h>
#include <stdlib.h>

/* Command table field offsets. */
#define CMDCTRL 0x00
#define CMDLINK 0x02
#define CMDPMOD 0x04
#define CMDCOLR 0x06
#define CMDSRCA 0x08
#define CMDSIZE 0x0A
#define CMDXA   0x0C
#define CMDYA   0x0E
#define CMDXB   0x10
#define CMDYB   0x12
#define CMDXC   0x14
#define CMDYC   0x16
#define CMDXD   0x18
#define CMDYD   0x1A
#define CMDGRDA 0x1C

#define FB_W 512
#define FB_H 256

typedef struct {
    saturn  *s;
    uint8_t *fb;
    uint8_t *meshfb;                  /* one flag per pixel, see saturn.h */
    int32_t  local_x, local_y;
    int32_t  sys_x1, sys_y1;          /* system clip, lower-right */
    int32_t  usr_x0, usr_y0, usr_x1, usr_y1;
    uint32_t commands, pixels;
    uint32_t typecnt[16], skipcnt;
    uint32_t typepix[16];   /* pixels each command type actually wrote */
    uint32_t cur_addr;      /* command being executed, for SATURN_V1PIX */
    uint16_t cur_ctrl, cur_pmod, cur_colr;
    uint32_t cur_srca;
} vctx;

/* Hardware-started command lists are resumed by vdp1_tick() from the machine
 * scheduler.  SaturnRecomp has one Saturn instance per process, so keeping the
 * private raster context here avoids exposing renderer internals in saturn.h. */
static struct {
    saturn *s;
    vctx c;
    uint32_t addr, ret_addr;
    int guard;
    uint64_t wait;
    int active;
} async_v1;

/* Optional window-renderer sink. Keeping the binding here means every core
 * and raster unit test continues to build without Vulkan. There is one Saturn
 * per process today, matching async_v1's existing lifetime. */
static struct {
    saturn *s;
    saturn_vdp1_gpu_sink sink;
} gpu_v1;

void vdp1_gpu_bind(saturn *s, const saturn_vdp1_gpu_sink *sink)
{
    if (!sink) {
        if (!s || gpu_v1.s == s) memset(&gpu_v1, 0, sizeof gpu_v1);
        return;
    }
    gpu_v1.s = s;
    gpu_v1.sink = *sink;
}

int vdp1_gpu_is_bound(const saturn *s)
{
    return gpu_v1.s == s && gpu_v1.sink.enqueue != NULL;
}

static int gpu_emit(vctx *c, const saturn_vk_vdp1_op *op)
{
    /* Interpolation deliberately renders into private CPU buffers and must
     * never become a real hardware draw. */
    if (c->s->vdp1_fb_override || !vdp1_gpu_is_bound(c->s)) return 0;
    return gpu_v1.sink.enqueue(gpu_v1.sink.userdata, op);
}

static void vctx_init(vctx *c, saturn *s)
{
    memset(c, 0, sizeof(*c));
    c->s = s;
    c->fb = s->vdp1_fb_override ? s->vdp1_fb_override : s->vdp1_fb[s->fb_draw];
    c->meshfb = s->vdp1_mesh_override ? s->vdp1_mesh_override
                                      : s->vdp1_meshfb[s->fb_draw];
    c->sys_x1 = FB_W - 1;
    c->sys_y1 = FB_H - 1;
    c->local_x = s->vdp1_local_x;
    c->local_y = s->vdp1_local_y;
    c->usr_x1 = FB_W - 1;
    c->usr_y1 = FB_H - 1;
}

static uint16_t vram16(saturn *s, uint32_t a)
{
    a &= (VDP1_VRAM_SZ - 1);
    return (uint16_t)((s->vdp1_vram[a] << 8) | s->vdp1_vram[a + 1]);
}

/* Used only by the interpolation pass, which patches vertex words in place
 * and restores them before returning. */
static void vram_w16(saturn *s, uint32_t a, uint16_t v)
{
    a &= (VDP1_VRAM_SZ - 1);
    s->vdp1_vram[a]     = (uint8_t)(v >> 8);
    s->vdp1_vram[a + 1] = (uint8_t)v;
}

/* Sign-extend a VDP1 13-bit coordinate. */
static int32_t coord(uint16_t v)
{
    int32_t x = v & 0x1FFF;
    if (v & 0x1000) x -= 0x2000;
    return x;
}

/* Sign-extend an arbitrary value into VDP1's 13-bit coordinate space. Edge
 * deltas go through here: the hardware wraps them, it does not widen them. */
static int32_t se13(int32_t v)
{
    v &= 0x1FFF;
    if (v & 0x1000) v -= 0x2000;
    return v;
}

/* Texture coordinate DDA, ported from Ymir vdp1_steppers.hpp TextureStepper.
 *
 * We used to map a pixel to a texel with a truncating divide
 * (`tx = j * tw / (n + 1)`). That biases every sample the same direction
 * instead of distributing the error, so textures sample the wrong texel and
 * skew across a stretched quad. The hardware runs a Bresenham DDA with an
 * accumulator, and -- the part that is easy to miss -- sets it up DIFFERENTLY
 * for minification (more texels than pixels) than for magnification: the
 * minifying branch bumps the numerator, the magnifying branch drops the
 * denominator so the last pixel lands exactly on the last texel.
 *
 * `length` is the number of pixels to cover; start/end are the first and last
 * texel. Flipping swaps start and end so the DDA runs backwards, which is not
 * the same as mirroring the result afterwards once rounding is involved. */
typedef struct { int32_t num, den, accum, value, inc; } texstep;

static void ts_setup(texstep *t, int32_t length, int32_t start, int32_t end)
{
    int32_t delta = end - start;
    int32_t ad    = delta < 0 ? -delta : delta;

    t->value = start;
    t->inc   = delta >= 0 ? 1 : -1;
    t->num   = ad;
    t->den   = length;
    if (length <= ad) {                 /* minification */
        t->num += 1;
        t->accum = ad - (length << 1);
        if (delta >= 0) t->accum += 1;
    } else {                            /* magnification */
        t->den -= 1;
        t->accum = length - (length << 1);
        if (delta < 0) t->accum += 1;
    }
    t->num <<= 1;
    t->den <<= 1;
}

/* Drain any pending texel steps, then advance one pixel -- the order Ymir's
 * software renderer uses (vdp_renderer_sw.cpp:1510). */
static void ts_advance(texstep *t)
{
    while (t->accum >= 0) { t->value += t->inc; t->accum -= t->den; }
    t->accum += t->num;
}

/* Quad edge walker, ported from Ymir vdp1_steppers.hpp `Edge`.
 *
 * We used to find a row's endpoints with a truncating lerp
 * (`lx = xa + (xd - xa) * i / steps`). That is not what the hardware does and
 * the error does not stay small: truncation biases every row the same way, so
 * a quad whose edges are not axis-aligned skews, and long quads stretch.
 *
 * The hardware runs a nested Bresenham: an outer accumulator decides whether
 * this pixel advances the edge at all (the edge is walked over `delta` steps,
 * not over its own length), and inner X and Y accumulators each decide whether
 * that axis moves. `accumTarget` is -1 when the corresponding delta is
 * negative, which is what makes stepping symmetric in both directions.
 *
 * Ymir additionally shifts every counter left by 32-13 so they wrap at 13 bits
 * through int32 overflow. That is deliberately NOT reproduced here: signed
 * overflow is undefined in C, and the wrap only matters for coordinates far
 * outside the framebuffer. The stepping arithmetic below is otherwise
 * identical. */
typedef struct {
    int32_t x, y, xInc, yInc;
    int32_t xNum, yNum, xDen, yDen, xAccum, yAccum, xAccumTarget, yAccumTarget;
    int32_t num, den, accum, accumTarget;
} edgestep;

static void edge_setup(edgestep *e, int32_t x1, int32_t y1,
                       int32_t x2, int32_t y2, int32_t delta)
{
    int32_t dx  = se13(x2 - x1), dy = se13(y2 - y1);
    int32_t adx = dx < 0 ? -dx : dx;
    int32_t ady = dy < 0 ? -dy : dy;
    int32_t dmaj = adx > ady ? adx : ady;

    e->x = x1; e->y = y1;
    e->xInc = dx >= 0 ? 1 : -1;
    e->yInc = dy >= 0 ? 1 : -1;
    e->xNum = adx << 1;
    e->yNum = ady << 1;
    e->xDen = e->yDen = dmaj << 1;
    e->xAccum = e->yAccum = ~dmaj;
    e->xAccumTarget = dy < 0 ? -1 : 0;
    e->yAccumTarget = dx < 0 ? -1 : 0;
    e->num   = dmaj << 1;
    e->den   = delta << 1;
    e->accum = ~delta;
    e->accumTarget = adx >= ady ? e->yAccumTarget : e->xAccumTarget;
}

static void edge_step(edgestep *e)
{
    e->accum += e->num;
    if (e->accum >= e->accumTarget) {
        e->accum -= e->den;
        e->xAccum += e->xNum;
        if (e->xAccum >= e->xAccumTarget) { e->xAccum -= e->xDen; e->x += e->xInc; }
        e->yAccum += e->yNum;
        if (e->yAccum >= e->yAccumTarget) { e->yAccum -= e->yDen; e->y += e->yInc; }
    }
}

/* Span walker, ported from Ymir vdp1_steppers.hpp `LineStepper`.
 *
 * The span used to be a plain linear interpolation, `px = lx + (rx-lx)*j/n`.
 * C integer division truncates TOWARD ZERO, so the minor-axis error is biased
 * down for a left-to-right span and up for a right-to-left one -- the two
 * halves of a quad strip round opposite ways and stop abutting, which is what
 * the visible seams between adjacent polygons were. The hardware runs a
 * Bresenham whose accumulator starts at `adx + 1`, i.e. half a step, so it
 * rounds to nearest regardless of direction.
 *
 * The counters are held as uint32 and compared as int32: Ymir shifts them left
 * by 32-13 so they wrap the way the hardware's 13-bit counters do, and doing
 * that arithmetic in a signed type would be undefined. */
#define LS_SHIFT 19

typedef struct {
    int32_t  x, y;
    int32_t  xMajInc, yMajInc, xMinInc, yMinInc;
    int32_t  aaXInc, aaYInc;      /* corner pixel that fills a diagonal step */
    uint32_t num, den, accum, accumTarget;
} linestep;

static void line_setup(linestep *L, int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    int32_t dx = x2 - x1, dy = y2 - y1;
    int32_t adx = dx < 0 ? -dx : dx;
    int32_t ady = dy < 0 ? -dy : dy;
    int32_t at;

    L->x = x1; L->y = y1;
    if (adx >= ady) {                        /* x-major */
        L->xMajInc = dx >= 0 ? 1 : -1; L->yMajInc = 0;
        L->xMinInc = 0;                L->yMinInc = dy >= 0 ? 1 : -1;
    } else {                                 /* y-major: major/minor swap */
        int32_t t;
        L->xMajInc = 0;                L->yMajInc = dy >= 0 ? 1 : -1;
        L->xMinInc = dx >= 0 ? 1 : -1; L->yMinInc = 0;
        t = dx;  dx  = dy;  dy  = t;
        t = adx; adx = ady; ady = t;
    }

    /* Ymir: `if (!antiAlias && dx < 0) ++m_accumTarget;` -- and the TEXTURED
     * SPAN inside a quad is built with antiAlias TRUE
     * (vdp_renderer_sw.cpp:1240 `LineStepper line{coord1, coord2, true}`), so
     * the bump is NOT applied there. We applied it unconditionally, which made
     * every right-to-left span (dx < 0 after the major/minor swap) step its
     * minor axis one position earlier than the hardware. Two adjacent quads of
     * opposite winding then disagree by a pixel along their shared edge, which
     * shows up as a seam. Only the anti-aliased DrawLine/DrawPolyLine commands
     * want the bump, and we do not route them through here. */
    at = 0;
    (void)dx;

    {
        /* The anti-aliased span (Ymir LineStepper with antiAlias = true) also
         * shifts both counters down by one and remembers a CORNER offset. On
         * the step where the minor axis moves, the hardware plots a second
         * pixel at that corner; without it a diagonal span is a bare staircase
         * and every quad drawn at an angle is riddled with single-pixel holes
         * -- which is what every 3-D model in every title looked like. */
        int32_t acc = (int32_t)(adx + 1) - 1;
        int32_t tgt = at - 1;
        int samesign = (x1 > x2) == (y1 > y2);
        if (L->xMajInc != 0) {                 /* x-major */
            L->aaXInc = samesign ? 0 : -L->xMajInc;
            L->aaYInc = samesign ? -L->yMinInc : 0;
        } else {                               /* y-major */
            L->aaXInc = samesign ? 0 : -L->xMinInc;
            L->aaYInc = samesign ? -L->yMajInc : 0;
        }
        L->num         = (uint32_t)(ady << 1) << LS_SHIFT;
        L->den         = (uint32_t)(adx << 1) << LS_SHIFT;
        L->accum       = (uint32_t)acc        << LS_SHIFT;
        L->accumTarget = (uint32_t)tgt        << LS_SHIFT;
        L->accum      += L->num;
    }

    /* Ymir pre-backs-up one major step because its loop steps before plotting. */
    L->x -= L->xMajInc;
    L->y -= L->yMajInc;
}

/* Returns non-zero when the minor axis stepped, i.e. when the caller owes the
 * span one extra pixel at (aaXInc, aaYInc) from the NEW position. */
static int line_step(linestep *L)
{
    L->x += L->xMajInc;
    L->y += L->yMajInc;
    L->accum -= L->num;
    if ((int32_t)L->accum <= (int32_t)L->accumTarget) {
        L->accum += L->den;
        L->x += L->xMinInc;
        L->y += L->yMinInc;
        return 1;
    }
    return 0;
}

/* NOT a correctness gap -- an earlier version of this comment said it was, and
 * that was a misreading of Ymir. Ymir's QuadStepper does classify a quad as
 * degenerate (cross products of consecutive edge pairs, `(signSum & ~4) == 0`),
 * but `plottedSegmentsMax = IsDegenerate() ? 2 : 1` does NOT plot two spans per
 * step. Read the loop at vdp_renderer_sw.cpp:1505-1530: it is an EARLY-EXIT
 * guard. A quad may be clipped out part-way through and come back into view, so
 * the renderer counts contiguous runs of plotted lines and only `break`s once it
 * has seen `plottedSegmentsMax` of them -- one run for a regular quad, two for a
 * degenerate one, which can reappear. It never draws anything extra.
 *
 * Our span loop runs `for (i = 0; i <= steps; i++)` with no early break, so it
 * already plots every step Ymir would and then some. Adding IsDegenerate() here
 * would be a pure PERFORMANCE optimisation (stop stepping sooner), never a fix
 * for missing geometry. Do not "fix" this expecting gaps to close. */

/* SATURN_V1PIX=x,y -- name every command that writes that pixel, in order.
 * Bounding boxes cannot answer "what actually drew this?" when a dozen quads
 * overlap; this can, and it is the difference between fixing the right command
 * and patching a plausible-looking wrong one. */
static void pixprobe(vctx *c, int32_t x, int32_t y, uint16_t colour)
{
    static int px = -2, py = -2;
    if (px == -2) {
        const char *e = getenv("SATURN_V1PIX");
        px = py = -1;
        if (e) { int a, b; if (sscanf(e, "%d,%d", &a, &b) == 2) { px = a; py = b; } }
    }
    if (px < 0 || x != px || y != py) return;
    printf("[v1pix] %d,%d <- cmd %05X COMM=%u ctrl=%04X pmod=%04X colr=%04X "
           "srca=%05X  colour=%04X\n", x, y, c->cur_addr, c->cur_ctrl & 0xFu,
           c->cur_ctrl, c->cur_pmod, c->cur_colr, c->cur_srca, colour);
}

static void put(vctx *c, int32_t x, int32_t y, uint16_t colour, uint16_t pmod)
{
    uint32_t o;
    int mesh = (pmod & 0x0100u) != 0;
    static int stipple = -1;

    if (x < 0 || y < 0 || x >= FB_W || y >= FB_H) return;
    if (x > c->sys_x1 || y > c->sys_y1) return;

    /* User clipping. CMDPMOD bit 10 arms it and bit 9 picks the sense: 0 draws
     * INSIDE the window set by command 0x8, 1 draws OUTSIDE it. The window was
     * being latched and then never consulted, so every command that asked to
     * be clipped drew over the whole framebuffer instead (Ymir
     * VDP1IsPixelClipped: reject when "is outside" != clippingMode). */
    if (pmod & 0x0400u) {
        int outside = (x < c->usr_x0 || x > c->usr_x1 ||
                       y < c->usr_y0 || y > c->usr_y1);
        if (outside != (int)((pmod >> 9) & 1u)) return;
    }

    /* Mesh: the Saturn's stipple transparency. Not a blend -- it simply skips
     * every other pixel on a checkerboard, which is why it looks the way it
     * does against VDP2 layers.
     *
     * SATURN_NOMESH=1 draws those pixels anyway. That is not accurate (real
     * hardware really does stipple), but it is the one measurement that tells
     * a MESHED polygon apart from a polygon the rasteriser is punching holes
     * in: if the dots survive with this set, they are not mesh. */
    /* SATURN_MESHSTIPPLE=1 restores the hardware behaviour exactly.
     * By default the skipped pixel is DRAWN and flagged instead, and the
     * compositor blends it 50/50 with whatever is behind -- the same
     * enhancement Ymir offers as `transparentMeshes`. Stipple is what the
     * hardware does, but it was designed to smear into a blend on a CRT and
     * reads as a checkerboard of holes at sharp pixel scale. */
    if (stipple < 0) stipple = getenv("SATURN_MESHSTIPPLE") != NULL;
    if (mesh && stipple && (((x + y) & 1) != 0)) return;

    o = ((uint32_t)y * FB_W + (uint32_t)x) * 2u;

    /* Colour calculation. CMDPMOD bits 1-0 are a 2-bit MODE, and bit 2
     * (gouraud) is separate and has already been folded into `colour` by the
     * caller -- so this switches on `pmod & 3`, never `pmod & 7`.
     *
     * Ymir vdp_renderer_sw.cpp:1130. Mode 1 is the one worth spelling out: it
     * is SHADOW, which dims whatever is already in the framebuffer and does
     * not write the source colour at all. We previously treated mode 1 as
     * "halve the source" and had no shadow at all, and a later attempt to read
     * bits 1-0 as two independent flags made mode 3 halve the source *and*
     * blend it. Both are wrong; the hardware picks exactly one of four. */
    switch (pmod & 3u) {
    case 0:                                  /* Replace                     */
        break;
    case 1: {                                /* Shadow: halve DESTINATION   */
        uint16_t un = (uint16_t)((c->fb[o] << 8) | c->fb[o + 1]);
        if (!(un & 0x8000u)) return;         /* transparent dst: nothing     */
        colour = (uint16_t)(0x8000u
               | (((un        & 0x1Fu) >> 1))
               | (((un >>  5) & 0x1Fu) >> 1) << 5
               | (((un >> 10) & 0x1Fu) >> 1) << 10);
        break;
    }
    case 2: {                                /* Half-luminance of SOURCE    */
        uint16_t r = (uint16_t)( colour        & 0x1Fu);
        uint16_t g = (uint16_t)((colour >>  5) & 0x1Fu);
        uint16_t b = (uint16_t)((colour >> 10) & 0x1Fu);
        /* msb comes from the source, it is not forced on. */
        colour = (uint16_t)((colour & 0x8000u)
               | (r >> 1) | ((g >> 1) << 5) | ((b >> 1) << 10));
        break;
    }
    default: {                               /* Half-transparency           */
        uint16_t un = (uint16_t)((c->fb[o] << 8) | c->fb[o + 1]);
        if (un & 0x8000u) {
            uint16_t r = (uint16_t)((( colour        & 0x1Fu) + ( un        & 0x1Fu)) >> 1);
            uint16_t g = (uint16_t)((((colour >>  5) & 0x1Fu) + ((un >>  5) & 0x1Fu)) >> 1);
            uint16_t b = (uint16_t)((((colour >> 10) & 0x1Fu) + ((un >> 10) & 0x1Fu)) >> 1);
            colour = (uint16_t)(0x8000u | r | (g << 5) | (b << 10));
        }
        break;
    }
    }

    pixprobe(c, x, y, colour);
    if (mesh && !stipple) {
        /* Ymir's enhancement renders mesh commands to a second colour
         * framebuffer.  Keeping the normal framebuffer untouched is crucial:
         * it contains the VDP1 pixel that must be visible through a shield,
         * smoke cloud, etc.  The compositor merges the two later. */
        c->meshfb[o]     = (uint8_t)(colour >> 8);
        c->meshfb[o + 1] = (uint8_t)colour;
    } else {
        c->fb[o]     = (uint8_t)(colour >> 8);
        c->fb[o + 1] = (uint8_t)colour;
        /* A later ordinary sprite erases an earlier mesh at this pixel. */
        c->meshfb[o] = c->meshfb[o + 1] = 0;
    }
    c->pixels++;
}

/* The overwhelmingly common game path is an ordinary replace-mode pixel:
 * no user clipping, mesh, shadow, half-luminance or transparency blend. Keep
 * the exact bounds/system-clip checks and framebuffer effects, but select this
 * path once per command instead of re-decoding CMDPMOD for every pixel. */
static inline void put_replace(vctx *c, int32_t x, int32_t y, uint16_t colour)
{
    uint32_t o;

    if ((uint32_t)x >= FB_W || (uint32_t)y >= FB_H) return;
    if (x > c->sys_x1 || y > c->sys_y1) return;

    o = ((uint32_t)y * FB_W + (uint32_t)x) * 2u;
    pixprobe(c, x, y, colour);
    c->fb[o]     = (uint8_t)(colour >> 8);
    c->fb[o + 1] = (uint8_t)colour;
    c->meshfb[o] = c->meshfb[o + 1] = 0;
    c->pixels++;
}

/* Resolve one texel to an RGB555 value. Returns 0 if the texel is transparent. */
static uint16_t texel(vctx *c, uint32_t chr, uint32_t texpos, uint32_t tx,
                      uint16_t colr, uint16_t pmod, int *transparent)
{
    saturn *s = c->s;
    /* CMDPMOD: colour mode is bits 5-3; bits 2-0 are the colour-calculation
     * field (gouraud etc.) and bit 6 is SPD ("draw index 0"). Reading the
     * mode from bits 2-0 misdecoded every sprite whose colour-calc bits were
     * set -- the gouraud-shaded boot-logo sphere and half the CD player's
     * icons rendered through the wrong texel decoder. */
    unsigned mode = (pmod >> 3) & 7u;
    unsigned spd  = (pmod >> 6) & 1u;
    unsigned ecd  = (pmod >> 7) & 1u;   /* End Code Disable */
    uint32_t idx = 0;
    uint16_t v = 0;

    *transparent = 0;

    switch (mode) {
    case 0:   /* 16 colour, colour BANK (mode 0 is bank, 1 is LUT -- these
               * were swapped, and bank pixels also had bit 15 forced on.
               * Bit 15 is the mixed-mode RGB flag on the way back OUT of the
               * framebuffer, so every palette sprite was read back as RGB555
               * with its palette index in the red channel: the CD player's
               * icons and starfield rendered as red noise. The framebuffer
               * holds the PALETTE CODE (CMDCOLR + index) untouched; VDP2's
               * sprite-type decode does the rest. */
    case 1: { /* 16 colour, lookup table */
        uint32_t byte = chr + texpos / 2u;
        uint8_t  b    = s->vdp1_vram[byte & (VDP1_VRAM_SZ - 1)];
        idx = (tx & 1) ? (b & 0x0F) : (b >> 4);
        /* END CODE. Per colour mode the all-ones value terminates the texture
         * line, and the pixel itself is skipped exactly like a transparent one
         * (Ymir `processEndCode`, then `if (hasEndCode || transparent) skip`).
         * CMDPMOD bit 7 (ECD) disables it. We drew end codes as real colours --
         * palette entry 15/255, usually black -- which is what put black blocks
         * around sprites that use them, NiGHTS' sparkles among them. */
        if (idx == 0x0Fu && !ecd) { *transparent = 1; return 0; }
        if (idx == 0 && !spd) { *transparent = 1; return 0; }
        if (mode == 1) {
            /* CLUT: 16 entries at CMDCOLR*8; entries pass through verbatim
             * (they may be RGB values or palette codes). */
            v = vram16(s, ((uint32_t)colr << 3) + idx * 2u);
        } else {
            /* Bank mode ORs the index into a bank whose low bits the hardware
             * MASKS OFF -- it does not ADD (Ymir VDP1PlotTexturedQuad
             * precomputes colorBank &= 0xFFF0/0xFFC0/0xFF80/0xFF00, then
             * VDP1PlotTexturedLine does `color |= colorBank`). Adding lets a
             * CMDCOLR with dirty low bits carry into the palette-code,
             * colour-calculation and PRIORITY fields of the framebuffer word. */
            v = (uint16_t)((colr & 0xFFF0u) | idx);
        }
        break;
    }
    case 2:   /* 64 colour  */
    case 3:   /* 128 colour */
    case 4: { /* 256 colour */
        static const uint16_t bankmask[3] = { 0xFFC0u, 0xFF80u, 0xFF00u };
        static const uint16_t idxmask[3]  = { 0x003Fu, 0x007Fu, 0x00FFu };
        uint32_t byte = chr + texpos;
        idx = s->vdp1_vram[byte & (VDP1_VRAM_SZ - 1)];
        if (idx == 0xFFu && !ecd) { *transparent = 1; return 0; }   /* end code */
        if (idx == 0 && !spd) { *transparent = 1; return 0; }
        v = (uint16_t)((colr & bankmask[mode - 2]) | (idx & idxmask[mode - 2]));
        break;
    }
    default: { /* 5: RGB555 direct */
        uint32_t byte = chr + texpos * 2u;
        v = vram16(s, byte);
        if (v == 0x7FFFu && !ecd) { *transparent = 1; return 0; }   /* end code */
        if (!(v & 0x8000u) && !spd) { *transparent = 1; return 0; }
        break;
    }
    }
    return v;
}

/* Interpolate a gouraud colour across the quad, if enabled. */
static uint16_t shade(vctx *c, uint16_t base, uint32_t grda, int use_g,
                      int32_t u, int32_t v, int32_t umax, int32_t vmax)
{
    uint16_t g0, g1, g2, g3;
    int32_t r, g, b;
    int32_t ru, rv;

    if (!use_g || !grda) return base;
    if (umax <= 0) umax = 1;
    if (vmax <= 0) vmax = 1;

    g0 = vram16(c->s, grda * 8u + 0);   /* A */
    g1 = vram16(c->s, grda * 8u + 2);   /* B */
    g2 = vram16(c->s, grda * 8u + 4);   /* C */
    g3 = vram16(c->s, grda * 8u + 6);   /* D */

    ru = (u * 32) / umax;
    rv = (v * 32) / vmax;

#define CH(x, sh) (int32_t)(((x) >> (sh)) & 0x1F)
    /* Bilinear across the four vertex colours. */
    r = ((CH(g0,0)*(32-ru) + CH(g1,0)*ru) * (32-rv) +
         (CH(g3,0)*(32-ru) + CH(g2,0)*ru) * rv) / 1024;
    g = ((CH(g0,5)*(32-ru) + CH(g1,5)*ru) * (32-rv) +
         (CH(g3,5)*(32-ru) + CH(g2,5)*ru) * rv) / 1024;
    b = ((CH(g0,10)*(32-ru) + CH(g1,10)*ru) * (32-rv) +
         (CH(g3,10)*(32-ru) + CH(g2,10)*ru) * rv) / 1024;
#undef CH

    /* Gouraud on the Saturn offsets the base colour around mid-grey. */
    {
        int32_t br = (int32_t)( base        & 0x1F) + r - 16;
        int32_t bg = (int32_t)((base >>  5) & 0x1F) + g - 16;
        int32_t bb = (int32_t)((base >> 10) & 0x1F) + b - 16;
        if (br < 0)  br = 0;
        if (br > 31) br = 31;
        if (bg < 0)  bg = 0;
        if (bg > 31) bg = 31;
        if (bb < 0)  bb = 0;
        if (bb > 31) bb = 31;
        return (uint16_t)(0x8000 | br | (bg << 5) | (bb << 10));
    }
}

/* Rasterise a quad, optionally textured. A/B/C/D are in draw-space. */
static void quad(vctx *c, int32_t xa, int32_t ya, int32_t xb, int32_t yb,
                 int32_t xc, int32_t yc, int32_t xd, int32_t yd,
                 uint32_t chr, uint32_t tw, uint32_t th,
                 uint16_t colr, uint16_t pmod, uint32_t grda,
                 uint16_t flat, int textured, unsigned flip)
{
    /* Step count over the longer of the two "vertical" edges.
     *
     * Per Ymir vdp1_steppers.hpp QuadStepper: each delta is SIGN-EXTENDED TO
     * 13 BITS before abs(), and the maximum is masked to 12 bits. Both matter.
     * The coordinates are already 13-bit signed (-4096..4095), so a difference
     * can reach +-8190, which does NOT fit in 13 bits -- the hardware wraps it.
     * A delta of 8000 is really -192. Computing the delta in full precision
     * instead drew a quad stretched across the whole screen where the hardware
     * draws a short one, which is how long-span geometry (a road running to the
     * horizon) came out wrong. Masking to 0xFFF also WRAPS where we used to
     * saturate at 4096. */
    int32_t d1 = abs(se13(xd - xa)), d2 = abs(se13(yd - ya));
    int32_t d3 = abs(se13(xc - xb)), d4 = abs(se13(yc - yb));
    int32_t steps = d1;
    edgestep eL, eR;
    /* Gouraud is CMDPMOD BIT 2, not the value 4: bits 1-0 carry half-luminance
     * and half-transparency alongside it, so 5, 6 and 7 are gouraud too and
     * were all falling through unshaded. */
    int use_g = (pmod & 4u) != 0;
    int fast_replace = (pmod & 0x0503u) == 0;

    if (d2 > steps) steps = d2;
    if (d3 > steps) steps = d3;
    if (d4 > steps) steps = d4;
    /* No texture-height bump: forcing steps up to th made the top screen row
     * rasterise twice (i=0 and i=1 both landing on it), so every sprite's
     * first row showed the SECOND texel row. Minification falls out of the
     * ty mapping by itself. */
    /* Ymir masks the major delta to 12 bits (`m_dmaj = max(...) & 0xFFF`). */
    steps &= 0xFFF;
    if (steps <= 0) steps = 1;

    /* In Vulkan mode the command walker still resolves coordinates and
     * register state at the hardware-visible instant, but the CPU never
     * enters the pixel loops below. The compute renderer consumes these in
     * exact submission order and keeps both Saturn framebuffers persistent. */
    {
        saturn_vk_vdp1_op op;
        memset(&op, 0, sizeof op);
        op.kind = SATURN_VK_VDP1_QUAD;
        op.target = (uint32_t)c->s->fb_draw;
        op.xy[0] = xa; op.xy[1] = ya; op.xy[2] = xb; op.xy[3] = yb;
        op.xy[4] = xc; op.xy[5] = yc; op.xy[6] = xd; op.xy[7] = yd;
        op.chr = chr; op.tw = tw; op.th = th;
        op.colr = colr; op.pmod = pmod; op.grda = grda;
        op.flat = flat; op.textured = (uint32_t)textured; op.flip = flip;
        op.sys_x1 = c->sys_x1; op.sys_y1 = c->sys_y1;
        op.usr_x0 = c->usr_x0; op.usr_y0 = c->usr_y0;
        op.usr_x1 = c->usr_x1; op.usr_y1 = c->usr_y1;
        if (gpu_emit(c, &op)) {
            int32_t top = abs(xb - xa), bottom = abs(xc - xd);
            int32_t side = abs(yb - ya);
            if (abs(yc - yd) > side) side = abs(yc - yd);
            if (bottom > top) top = bottom;
            c->pixels += (uint32_t)((top + 1) * (side + 1));
            return;
        }
    }

    /* Left edge A->D, right edge B->C, both walked over `steps` increments. */
    edge_setup(&eL, xa, ya, xd, yd, steps);
    edge_setup(&eR, xb, yb, xc, yc, steps);

    for (int32_t i = 0; i <= steps; i++) {
        int32_t lx, ly, rx, ry;
        lx = eL.x; ly = eL.y;
        rx = eR.x; ry = eR.y;
        edge_step(&eL);
        edge_step(&eR);
        int32_t sd = abs(rx - lx);
        int32_t sv = abs(ry - ly);
        int32_t n  = sd > sv ? sd : sv;
        linestep ln;
        if (n < 0) n = 0;
        if (n > 4096) n = 4096;
        /* A zero-length span is ONE pixel, not two -- `n = 1` here used to
         * double-plot every degenerate span. */
        line_setup(&ln, lx, ly, rx, ry);
        /* This first step undoes the setup's pre-backup and lands on the start
         * pixel; its return value is the anti-alias flag for THAT pixel. */
        int aa = line_step(&ln);

        /* These are exactly the old integer mappings, expressed as a
         * quotient/remainder sequence. `ty` is invariant across this row;
         * `tx_base` advances so iteration j is floor(j*tw/(n+1)). This removes
         * two 64-bit divides from every textured pixel without changing a
         * sampled texel or any raster geometry. */
        uint32_t ty = 0, row_texel = 0;
        uint32_t tx_base = 0, tx_q = 0, tx_r = 0, tx_acc = 0;
        uint32_t tx_den = (uint32_t)n + 1u;
        if (textured) {
            ty = (uint32_t)((int64_t)i * (int64_t)th / (steps + 1));
            if (flip & 2u) ty = th - 1u - ty;
            if (ty >= th) ty = th - 1u;
            row_texel = ty * tw;
            tx_q = tw / tx_den;
            tx_r = tw % tx_den;
        }

        for (int32_t j = 0; j <= n; j++) {
            /* Ymir masks the plotted coordinate to 11 bits (LineStepper::X/Y). */
            int32_t px = ln.x & 0x7FF;
            int32_t py = ln.y & 0x7FF;
            /* Ymir AACoord(): the corner pixel, taken from the position being
             * plotted now -- so it must be read BEFORE stepping on. */
            int32_t apx = (ln.x + ln.aaXInc) & 0x7FF;
            int32_t apy = (ln.y + ln.aaYInc) & 0x7FF;
            int aa_now = aa;
            uint16_t col = flat;
            aa = line_step(&ln);        /* flag belongs to the NEXT pixel */
            int transparent = 0;
            uint32_t tx = 0;

            /* NOTE: the Ymir TextureStepper DDA (ts_setup/ts_advance above) is
             * NOT used here yet. Ported naively it failed tests/vdp1_quad
             * ("RGB texel (7,7) got 80E6 want 80E7") -- it stops one texel
             * short of the end, so the call ordering around
             * ShouldStepTexel/StepTexel/StepPixel still needs deriving from
             * vdp_renderer_sw.cpp rather than guessing. Until then keep the
             * direct mapping, which is less accurate but correct at the
             * endpoints. */
            if (textured) {
                tx = tx_base;
                tx_base += tx_q;
                tx_acc += tx_r;
                if (tx_acc >= tx_den) {
                    tx_base++;
                    tx_acc -= tx_den;
                }
                if (flip & 1u) tx = tw - 1 - tx;
                if (tx >= tw) tx = tw - 1;
                col = texel(c, chr, row_texel + tx, tx,
                            colr, pmod, &transparent);
                if (transparent) continue;
            }
            col = shade(c, col, grda, use_g, j, i, n, steps);
            if (fast_replace) put_replace(c, px, py, col);
            else              put(c, px, py, col, pmod);
            /* Ymir: `if (aa) VDP1PlotPixel(line.AACoord(), ...)` with the same
             * pixel parameters. This is the pixel that fills the staircase. */
            if (aa_now) {
                if (fast_replace) put_replace(c, apx, apy, col);
                else              put(c, apx, apy, col, pmod);
            }
        }
    }
}


/* ---------------------------------------------------------- frame interp --
 * MEASUREMENT for 30 Hz -> 60 Hz presentation. A title like NiGHTS builds a
 * VDP1 list every OTHER field, so the missing frames could be synthesised by
 * lerping each quad between the list that built frame N and the one that
 * builds frame N+1. Whether that is viable at all comes down to one number:
 * how reliably a command in this frame can be identified as "the same object"
 * as a command in the last one. VDP1 gets SCREEN-SPACE quads -- SGL already
 * did the transform on the SH-2 -- so there is no matrix to interpolate and no
 * object id to key on; the identity has to be inferred.
 *
 * SATURN_INTERP=1 captures both lists and reports the match rate. It changes
 * nothing on screen. */
typedef struct {
    uint32_t srca;                 /* texture address: the strongest key */
    uint32_t addr;                 /* where the command lives in VRAM */
    uint16_t size, pmod;
    uint8_t  cmd;
    int16_t  v[8];                 /* xa,ya,xb,yb,xc,yc,xd,yd */
} icmd;

#define ICMD_MAX 4096
static icmd  icap[2][ICMD_MAX];
static int   icap_n[2];
static int   icap_cur;             /* which buffer this frame is filling */
static int   interp_on = -1;

static int icmd_same(const icmd *a, const icmd *b)
{
    return a->srca == b->srca && a->size == b->size &&
           a->cmd == b->cmd && a->pmod == b->pmod;
}

/* Manhattan distance between two quads' corners: the tiebreak that decides
 * WHICH of several identical-looking quads is "the same object". Particles and
 * repeated tiles share texture, size and mode, so the key alone pairs them
 * arbitrarily -- and a wrong pairing renders as an object teleporting across
 * the screen, which is far worse to look at than simply not interpolating. */
static int32_t icmd_dist(const icmd *a, const icmd *b)
{
    int32_t d = 0; int k;
    for (k = 0; k < 8; k++) {
        int32_t e = (int32_t)a->v[k] - (int32_t)b->v[k];
        d += e < 0 ? -e : e;
    }
    return d;
}

/* Find the previous-frame command that best corresponds to `cur`: same key,
 * nearest position, and only if it has not already been claimed. Returns -1
 * when nothing matches -- the caller must then HOLD that quad rather than
 * invent a position for it. */
static int icmd_match(const icmd *cur, const icmd *prv, int pn,
                      const uint8_t *taken, int hint)
{
    int best = -1; int32_t bestd = 0x7FFFFFFF; int j;

    /* The list is usually stable, so the same slot is nearly always right and
     * costs one comparison instead of a scan. */
    if (hint >= 0 && hint < pn && !taken[hint] && icmd_same(cur, &prv[hint]))
        return hint;

    for (j = 0; j < pn; j++) {
        int32_t d;
        if (taken[j] || !icmd_same(cur, &prv[j])) continue;
        d = icmd_dist(cur, &prv[j]);
        if (d < bestd) { bestd = d; best = j; }
    }
    /* A "match" that moved half the screen is not the same object. 96 px is
     * about 8x the mean per-frame motion measured in NiGHTS (8.9 px). */
    if (best >= 0 && bestd > 96 * 8) return -1;
    return best;
}

/* Compare the just-built list against the previous one and report. */
static void icap_report(saturn *s)
{
    const icmd *cur = icap[icap_cur], *prv = icap[icap_cur ^ 1];
    int n = icap_n[icap_cur], pn = icap_n[icap_cur ^ 1];
    static uint8_t taken[ICMD_MAX];
    int inplace = 0, near = 0, miss = 0, i;
    long long motion = 0; int moved = 0;

    if (pn == 0) return;                       /* nothing to compare against */
    memset(taken, 0, (size_t)pn);
    for (i = 0; i < n; i++) {
        int j = icmd_match(&cur[i], prv, pn, taken, i);
        if (j < 0) { miss++; continue; }
        taken[j] = 1;
        if (j == i) inplace++; else near++;
        motion += icmd_dist(&cur[i], &prv[j]);
        moved++;
    }
    printf("[interp] frame %llu: %d cmds (prev %d) | same-slot %d (%.1f%%) "
           "nearest %d | UNMATCHED %d (%.1f%%) | mean |dv| %.1f px\n",
           (unsigned long long)s->frames, n, pn,
           inplace, 100.0 * inplace / (n ? n : 1), near,
           miss, 100.0 * miss / (n ? n : 1),
           moved ? (double)motion / (moved * 8.0) : 0.0);
}


/* Build the midpoint frame between the previous command list and the one just
 * drawn, at t = num/den, into the scratch framebuffer.
 *
 * Rather than re-implement the 200-line command decoder with substituted
 * vertices, this PATCHES the eight vertex words of each matched command in
 * VRAM, re-runs the ordinary walker with the framebuffer redirected, and puts
 * the originals back. The patch is live only for the duration of this call and
 * nothing else runs in between, so no other reader can observe it.
 *
 * A quad with no match in the previous list is left exactly where it is: an
 * object that pops is far less objectionable than one that teleports because
 * the matcher paired it with the wrong instance. */
void vdp1_build_interp(saturn *s, int num, int den)
{
    const icmd *cur = icap[icap_cur], *prv = icap[icap_cur ^ 1];
    int n = icap_n[icap_cur], pn = icap_n[icap_cur ^ 1];
    static uint8_t taken[ICMD_MAX];
    static int16_t saved[ICMD_MAX][8];
    static int     patched[ICMD_MAX];
    int i, np = 0;

    s->vdp1_interp_ready = 0;
    if (!interp_on || n == 0 || pn == 0) return;

    memset(taken, 0, (size_t)pn);
    for (i = 0; i < n; i++) {
        int j = icmd_match(&cur[i], prv, pn, taken, i);
        int k;
        patched[i] = 0;
        if (j < 0) continue;                    /* unmatched: hold in place */
        taken[j] = 1;
        for (k = 0; k < 8; k++) {
            int32_t a = prv[j].v[k], b = cur[i].v[k];
            int32_t mid = a + ((b - a) * num) / den;
            saved[i][k] = (int16_t)vram16(s, cur[i].addr + CMDXA + (uint32_t)k * 2u);
            vram_w16(s, cur[i].addr + CMDXA + (uint32_t)k * 2u, (uint16_t)mid);
        }
        patched[i] = 1;
        np++;
    }
    if (np == 0) return;                        /* nothing moved: no midpoint */

    memset(s->vdp1_interp_fb, 0, VDP1_FB_SZ);
    memset(s->vdp1_interp_mesh, 0, VDP1_FB_SZ);
    s->vdp1_fb_override   = s->vdp1_interp_fb;
    s->vdp1_mesh_override = s->vdp1_interp_mesh;
    s->vdp1_interp_pass   = 1;
    vdp1_execute(s);
    s->vdp1_interp_pass   = 0;
    s->vdp1_fb_override   = NULL;
    s->vdp1_mesh_override = NULL;

    for (i = 0; i < n; i++) {
        int k;
        if (!patched[i]) continue;
        for (k = 0; k < 8; k++)
            vram_w16(s, cur[i].addr + CMDXA + (uint32_t)k * 2u,
                     (uint16_t)saved[i][k]);
    }
    s->vdp1_interp_ready = 1;
}

/* Execute the command list. Called when the game writes PTMR. */
void vdp1_execute(saturn *s)
{
    vctx c;
    int async = async_v1.active && async_v1.s == s;
    uint32_t addr = async ? async_v1.addr : 0;
    int guard = 0;
    uint32_t ret_addr = async ? async_v1.ret_addr : 0xFFFFFFFFu;

    /* SATURN_VDP1DUMP=frame: print every command of the list executed on
     * that frame (and the next one), with resolved fields. */
    int dumping = 0;
    {
        static long long dump_at = -2;
        if (dump_at == -2) {
            const char *e = getenv("SATURN_VDP1DUMP");
            dump_at = e ? atoll(e) : -1;
        }
        dumping = dump_at >= 0 && (long long)s->frames >= dump_at &&
                  (long long)s->frames <= dump_at + 1;
    }

    if (interp_on < 0) interp_on = getenv("SATURN_INTERP") != NULL;
    if (interp_on && !s->vdp1_interp_pass) { icap_cur ^= 1; icap_n[icap_cur] = 0; }

    if (async) {
        c = async_v1.c;
        guard = async_v1.guard;
    } else {
        vctx_init(&c, s);
    }

    for (; guard < 8192; guard++) {
        uint16_t ctrl = vram16(s, addr + CMDCTRL);
        uint16_t pmod, colr, size, link;
        uint32_t srca, grda;
        uint32_t px_before = c.pixels;
        c.cur_addr = addr; c.cur_ctrl = ctrl;
        int32_t xa, ya, xb, yb, xc_, yc_, xd, yd;
        unsigned cmd, jump;
        int skip;

        /* COPR reports the command the VDP1 is working on right now. Ymir
         * latches it before the end-bit test (vdp.cpp VDP1ProcessCommand), so
         * a list that ends immediately still leaves COPR on the END command
         * rather than on the one before it. */
        s->vdp1_copr = addr;

        if (ctrl & 0x8000) {                      /* END */
            /* EDSR.CEF -- "end bit fetched". Ymir sets it in VDP1EndFrame and
             * clears it in VDP1BeginFrame; a runaway list that never reaches
             * an END leaves it clear, exactly as the hardware does. */
            s->vdp1_cef = 1;
            if (async) async_v1.active = 0;
            break;
        }
        /* Bit 0x4000 is SKIP: the command is present but must not be drawn.
         * We were rasterising these unconditionally. Yabause gates the whole
         * dispatch on !(command & 0x4000). */
        skip = (ctrl & 0x4000) != 0;

        cmd  = ctrl & 0x000F;
        jump = (ctrl >> 12) & 7u;
        pmod = vram16(s, addr + CMDPMOD);
        colr = vram16(s, addr + CMDCOLR);
        srca = (uint32_t)vram16(s, addr + CMDSRCA) * 8u;
        size = vram16(s, addr + CMDSIZE);
        grda = vram16(s, addr + CMDGRDA);
        link = vram16(s, addr + CMDLINK);
        /* Latch the diagnostic fields only once the command words are loaded --
         * assigning them above the reads left SATURN_V1PIX naming garbage. */
        c.cur_pmod = pmod; c.cur_colr = colr; c.cur_srca = srca;

        xa = coord(vram16(s, addr + CMDXA)); ya = coord(vram16(s, addr + CMDYA));
        xb = coord(vram16(s, addr + CMDXB)); yb = coord(vram16(s, addr + CMDYB));
        xc_= coord(vram16(s, addr + CMDXC)); yc_= coord(vram16(s, addr + CMDYC));
        xd = coord(vram16(s, addr + CMDXD)); yd = coord(vram16(s, addr + CMDYD));

        if (dumping)
            printf("[v1 f%llu %04X] ctrl=%04X pmod=%04X colr=%04X srca=%05X "
                   "size=%04X grda=%05X A(%d,%d) B(%d,%d) C(%d,%d) D(%d,%d) loc(%d,%d)%s\n",
                   (unsigned long long)s->frames, addr, ctrl, pmod, colr,
                   srca, size, grda, xa, ya, xb, yb, xc_, yc_, xd, yd,
                   c.local_x, c.local_y, skip ? " SKIP" : "");
        if (dumping && grda)
            printf("[v1g %04X] G %04X %04X %04X %04X%s", addr,
                   vram16(s, grda * 8u), vram16(s, grda * 8u + 2),
                   vram16(s, grda * 8u + 4), vram16(s, grda * 8u + 6),
                   "\n");

        {
            uint32_t tw = (uint32_t)((size >> 8) & 0x3F) * 8u;
            uint32_t th = (uint32_t)(size & 0xFF);
            int32_t lx = c.local_x, ly = c.local_y;

            if (skip) { /* fall through to the link step */ }
            else switch (cmd) {
            case 0x0: { /* normal sprite: A is the top-left corner */
                int32_t x0 = lx + xa, y0 = ly + ya;
                int32_t x1 = x0 + (int32_t)tw - 1, y1 = y0 + (int32_t)th - 1;
                if (tw && th)
                    quad(&c, x0, y0, x1, y0, x1, y1, x0, y1,
                         srca, tw, th, colr, pmod, grda, 0, 1,
                         (ctrl >> 4) & 3u);
                break;
            }
            case 0x1: { /* scaled sprite. CMDCTRL bits 8-11 pick a zoom point:
                         * 0 = A and C are opposite corners; otherwise A pins
                         * the edge/centre the code names and B is the display
                         * size (Ymir VDP1Cmd_DrawScaledSprite). Reading C
                         * unconditionally collapsed every anchored sprite to
                         * its anchor -- the CD player icons in a heap. */
                uint32_t zph = (ctrl >> 8) & 3u, zpv = (ctrl >> 10) & 3u;
                int32_t qxa = xa, qya = ya, qxb = xa, qyb = ya;
                int32_t qxc = xa, qyc = ya, qxd = xa, qyd = ya;
                if      (zph == 0) { qxb = xc_; qxc = xc_; }
                else if (zph == 1) { qxb += xb; qxc += xb; }
                else if (zph == 2) { qxa -= xb >> 1; qxb += (xb + 1) >> 1;
                                     qxc += (xb + 1) >> 1; qxd -= xb >> 1; }
                else               { qxa -= xb; qxd -= xb; }
                if      (zpv == 0) { qyc = yc_; qyd = yc_; }
                else if (zpv == 1) { qyc += yb; qyd += yb; }
                else if (zpv == 2) { qya -= yb >> 1; qyb -= yb >> 1;
                                     qyc += (yb + 1) >> 1; qyd += (yb + 1) >> 1; }
                else               { qya -= yb; qyb -= yb; }
                if (tw && th)
                    quad(&c, lx + qxa, ly + qya, lx + qxb, ly + qyb,
                         lx + qxc, ly + qyc, lx + qxd, ly + qyd,
                         srca, tw, th, colr, pmod, grda, 0, 1,
                         (ctrl >> 4) & 3u);
                break;
            }
            case 0x2:
            case 0x3:   /* distorted sprite: four free corners */
                if (tw && th)
                    quad(&c, lx + xa, ly + ya, lx + xb, ly + yb,
                         lx + xc_, ly + yc_, lx + xd, ly + yd,
                         srca, tw, th, colr, pmod, grda, 0, 1,
                         (ctrl >> 4) & 3u);
                break;

            case 0x4:   /* polygon: flat-filled quad */
                /* SATURN_V1NOPOLY=1 -- drop every polygon, to answer whether
                 * the polygons are what differs from the reference. */
                { static int nop=-1;
                  if (nop<0) nop = getenv("SATURN_V1NOPOLY") != NULL;
                  if (nop) break; }
                quad(&c, lx + xa, ly + ya, lx + xb, ly + yb,
                     lx + xc_, ly + yc_, lx + xd, ly + yd,
                     0, 0, 0, colr, pmod, grda,
                     colr, 0, 0);   /* CMDCOLR verbatim: the sprite layer decides
                                  * transparency, not the rasteriser. The BIOS
                                  * player erases with 0x0000 polygons. */
                break;

            case 0x5:
            case 0x7:   /* polyline: four edges */
            case 0x6: { /* line: A->B only */
                int32_t px[4] = { lx + xa, lx + xb, lx + xc_, lx + xd };
                int32_t py[4] = { ly + ya, ly + yb, ly + yc_, ly + yd };
                int edges = (cmd == 0x6) ? 1 : 4;
                for (int e = 0; e < edges; e++) {
                    int32_t x0 = px[e], y0 = py[e];
                    int32_t x1 = px[(e + 1) & 3], y1 = py[(e + 1) & 3];
                    int32_t dx = abs(x1 - x0), dy = abs(y1 - y0);
                    int32_t n = dx > dy ? dx : dy;
                    if (n <= 0) n = 1;
                    if (n > 4096) n = 4096;
                    {
                        saturn_vk_vdp1_op op;
                        memset(&op, 0, sizeof op);
                        op.kind = SATURN_VK_VDP1_LINE;
                        op.target = (uint32_t)s->fb_draw;
                        op.xy[0] = x0; op.xy[1] = y0;
                        op.xy[2] = x1; op.xy[3] = y1;
                        op.colr = colr; op.pmod = pmod;
                        op.sys_x1 = c.sys_x1; op.sys_y1 = c.sys_y1;
                        op.usr_x0 = c.usr_x0; op.usr_y0 = c.usr_y0;
                        op.usr_x1 = c.usr_x1; op.usr_y1 = c.usr_y1;
                        if (gpu_emit(&c, &op)) {
                            c.pixels += (uint32_t)n + 1u;
                            continue;
                        }
                    }
                    for (int32_t t = 0; t <= n; t++)
                        put(&c, x0 + (x1 - x0) * t / n, y0 + (y1 - y0) * t / n,
                            colr, pmod);
                }
                break;
            }

            case 0xB:   /* user clipping (alternate encoding) */
            case 0x8:   /* user clipping -- per list, NOT persistent */
                c.usr_x0 = xa;
                c.usr_y0 = ya;
                c.usr_x1 = xc_;
                c.usr_y1 = yc_;
                /* User and system clipping are separate VDP1 commands.  The
                 * missing break here also replaced the system clip with the
                 * user rectangle, so later foreground commands could be
                 * clipped even after their user-clipping test passed. */
                break;

            case 0x9:   /* system clipping -- per list, NOT persistent */
                c.sys_x1 = xc_ ? xc_ : FB_W - 1;
                c.sys_y1 = yc_ ? yc_ : FB_H - 1;

                break;
            case 0xA:   /* local coordinates -- persists across command lists */
                c.local_x = s->vdp1_local_x = xa;
                c.local_y = s->vdp1_local_y = ya;
                break;
            default:
                break;
            }
        }

        if (interp_on && !s->vdp1_interp_pass && !skip &&
            icap_n[icap_cur] < ICMD_MAX && cmd <= 7u) {
            icmd *e = &icap[icap_cur][icap_n[icap_cur]++];
            e->srca = srca; e->size = size; e->pmod = pmod; e->addr = addr;
            e->cmd  = (uint8_t)cmd;
            e->v[0] = (int16_t)xa;  e->v[1] = (int16_t)ya;
            e->v[2] = (int16_t)xb;  e->v[3] = (int16_t)yb;
            e->v[4] = (int16_t)xc_; e->v[5] = (int16_t)yc_;
            e->v[6] = (int16_t)xd;  e->v[7] = (int16_t)yd;
        }

        c.commands++;
        c.typecnt[cmd]++;
        c.typepix[cmd] += c.pixels - px_before;
        if (skip) c.skipcnt++;

        /* Jump mode, per the VDP1 manual and Yabause's Vdp1DrawCommands:
         *   0 NEXT   -> following table
         *   1 ASSIGN -> CMDLINK
         *   2 CALL   -> CMDLINK, remembering where to come back to
         *   3 RETURN -> back to the saved address, else fall through
         * We previously treated CALL as a plain jump and RETURN as end-of-list,
         * which truncates any list a game builds with subroutines. */
        switch (jump & 3u) {
        case 0: addr += 0x20; break;
        case 1: addr = (uint32_t)link * 8u; break;
        case 2:
            if (ret_addr == 0xFFFFFFFFu) ret_addr = addr + 0x20;
            addr = (uint32_t)link * 8u;
            break;
        default:
            if (ret_addr != 0xFFFFFFFFu) { addr = ret_addr; ret_addr = 0xFFFFFFFFu; }
            else                          addr += 0x20;
            break;
        }

        addr &= (VDP1_VRAM_SZ - 1);

        if (async) {
            uint32_t command_pixels = c.pixels - px_before;
            async_v1.c = c;
            async_v1.addr = addr;
            async_v1.ret_addr = ret_addr;
            async_v1.guard = guard + 1;
            /* Ymir charges a 16-cycle fetch plus raster work, with VDP1
             * running at four internal cycles per SH-2 cycle.  Keep at least
             * one scheduler slice between commands so CPU VRAM edits can
             * interleave with the list walk. */
            async_v1.wait = 16u + ((uint64_t)command_pixels >> 2);
            if (async_v1.wait < 128u) async_v1.wait = 128u;
            return;
        }
    }

    if (guard >= 8192) {
        if (getenv("SATURN_VDP1WARN"))
            printf("[vdp1] command walk hit the 8192 guard at %05X -- no END bit, runaway list\n", addr);
        /* This is an artificial host-side safety limit, not VDP1 draw end.
         * The old fallthrough below raised Sprite Draw End every scheduler
         * slice while async_v1.guard remained 8192. A game with an unfinished
         * table therefore received hundreds of thousands of fake interrupts
         * and we re-accounted the same runaway list forever. Hardware keeps
         * CEF clear and never signals draw end until it actually fetches END. */
        if (async) async_v1.active = 0;
        s->vdp1_commands += c.commands;
        s->vdp1_pixels   += c.pixels;
        s->vdp1_lists++;
        return;
    }
    s->vdp1_commands += c.commands;
    s->vdp1_pixels   += c.pixels;
    s->vdp1_lists++;
    if (interp_on && !s->vdp1_interp_pass) {
        icap_report(s);
        /* Build the midpoint between the list just drawn and the one before
         * it. Guarded by vdp1_interp_pass, which the nested call sets, so this
         * cannot recurse. */
        vdp1_build_interp(s, 1, 2);
    }

    /* SATURN_V1FB=frame: write both VDP1 framebuffers out as PNG on that
     * frame. This is the one thing the command dump and the pixel histogram
     * cannot tell apart -- "the rasteriser never drew it" versus "it was
     * drawn and the VDP2 sprite layer then discarded it". */
    {
        static long long fb_at = -2;
        if (fb_at == -2) {
            const char *e = getenv("SATURN_V1FB");
            fb_at = e ? atoll(e) : -1;
        }
        if (fb_at >= 0 && (long long)s->frames == fb_at) {
            static uint32_t img[FB_W * FB_H];
            const char *base = getenv("SATURN_V1FB_OUT");
            int b;
            for (b = 0; b < 2; b++) {
                const uint8_t *f = s->vdp1_fb[b];
                char path[256];
                unsigned nz = 0;
                int i;
                for (i = 0; i < FB_W * FB_H; i++) {
                    uint16_t px = (uint16_t)((f[i * 2] << 8) | f[i * 2 + 1]);
                    uint32_t r = px & 0x1F, g = (px >> 5) & 0x1F, bl = (px >> 10) & 0x1F;
                    if (px) nz++;
                    img[i] = 0xFF000000u | (((r << 3) | (r >> 2)) << 16)
                                         | (((g << 3) | (g >> 2)) << 8)
                                         |  ((bl << 3) | (bl >> 2));
                }
                snprintf(path, sizeof path, "%s.fb%d.png",
                         base ? base : "v1fb", b);
                png_write(path, img, FB_W, FB_H);
                printf("[v1fb] fb%d %s nonzero=%u -> %s\n", b,
                       b == s->fb_draw ? "draw" : "disp", nz, path);
            }
            fflush(stdout);
        }
    }

    /* SATURN_V1HIST=N: once every N command lists, print how many commands of
     * each type the list contained and how many pixels they produced. One long
     * run then answers "does this game ever issue polygons?" without a second
     * pass. */
    {
        static long long hist_every = -2;
        static unsigned long long nlist = 0;
        if (hist_every == -2) {
            const char *e = getenv("SATURN_V1HIST");
            hist_every = e ? atoll(e) : -1;
            if (hist_every == 0) hist_every = 1;
        }
        if (hist_every > 0 && (nlist % (unsigned long long)hist_every) == 0) {
            printf("[v1h f%llu cy%llu] cmds=%u skip=%u px=%u  sprN=%u sprS=%u sprD=%u "
                   "poly=%u plines=%u line=%u uclip=%u sclip=%u loc=%u  sys=(%d,%d) usr=(%d,%d %d,%d)\n",
                   (unsigned long long)s->frames,
                   (unsigned long long)s->master.cycles,
                   c.commands, c.skipcnt, c.pixels,
                   c.typecnt[0], c.typecnt[1], c.typecnt[2] + c.typecnt[3],
                   c.typecnt[4], c.typecnt[5] + c.typecnt[7], c.typecnt[6],
                   c.typecnt[8] + c.typecnt[0xB], c.typecnt[9], c.typecnt[0xA],
                   c.sys_x1, c.sys_y1, c.usr_x0, c.usr_y0, c.usr_x1, c.usr_y1);
            printf("[v1p f%llu] pixels by type: sprN=%u sprS=%u sprD=%u poly=%u pline=%u line=%u\n",
                   (unsigned long long)s->frames,
                   c.typepix[0], c.typepix[1], c.typepix[2] + c.typepix[3],
                   c.typepix[4], c.typepix[5] + c.typepix[7], c.typepix[6]);
            fflush(stdout);
        }
        nlist++;
    }

    /* Sprite draw end, the interrupt games use for frame pacing. EDSR.CEF is
     * set by the END command in the walk above, not unconditionally here. */
    scu_raise(s, 13);
    /* Sprite Draw End is also SCU DMA trigger factor 6. */
    scu_dma_trigger(s, 6);
}

/* Ymir vdp.cpp VDP1BeginFrame: reset the command pointer and the end-bit
 * status, then walk the list. Reached from a PTMR=1 write and from a
 * framebuffer swap when PTMR=2. */
void vdp1_begin_frame(saturn *s)
{
    /* SATURN_FBLOG: when the list is DRAWN, on the same clock as [fbdec] and
     * [fbswap]. Erase, draw and swap only produce a picture in that order --
     * an erase that lands after the draw wipes the frame before it is ever
     * presented, which looks exactly like "the sprite layer is empty". */
    { static int lg = -1; static unsigned long long n;
      if (lg < 0) lg = getenv("SATURN_FBLOG") ? 1 : 0;
      if (lg && ++n <= 100000u)
          printf("[fbdraw] into fb%d clk=%llu%s", s->fb_draw,
                 (unsigned long long)s->clk, "\n"); }
    s->vdp1_copr = 0;
    s->vdp1_cef  = 0;
    async_v1.s = s;
    vctx_init(&async_v1.c, s);
    async_v1.addr = 0;
    async_v1.ret_addr = 0xFFFFFFFFu;
    async_v1.guard = 0;
    async_v1.wait = 0;
    async_v1.active = 1;
}

void vdp1_tick(saturn *s, uint32_t cycles)
{
    if (!async_v1.active || async_v1.s != s) return;
    if (async_v1.wait > cycles) {
        async_v1.wait -= cycles;
        return;
    }
    async_v1.wait = 0;
    vdp1_execute(s);              /* resumes exactly one command */
}

/* VDP::Reset(false): reset VDP1 registers and internal drawing state without
 * clearing VRAM or either framebuffer. */
void vdp1_soft_reset(saturn *s)
{
    memset(s->vdp1_reg, 0, sizeof s->vdp1_reg);
    s->vdp1_copr = s->vdp1_lopr = 0;
    s->vdp1_cef = s->vdp1_bef = 0;
    s->vdp1_fbparams = 0;
    s->vdp1_ew_val = 0;
    s->vdp1_ew_x1 = s->vdp1_ew_y1 = 0;
    s->vdp1_ew_x3 = s->vdp1_ew_y3 = 0;
    s->vdp1_local_x = s->vdp1_local_y = 0;
    s->vdp1_usr_x0 = s->vdp1_usr_y0 = 0;
    s->vdp1_usr_x1 = s->vdp1_usr_y1 = -1;
    s->vdp1_sys_x1 = 512;
    s->vdp1_sys_y1 = 256;
    s->vdp1_clip_set = 0;
    s->vdp1_erase_pending = 0;
    s->vdp1_show_interp = 0;
    s->vdp1_interp_ready = 0;
    if (async_v1.s == s) async_v1.active = 0;
    if (gpu_v1.s == s && gpu_v1.sink.reset)
        gpu_v1.sink.reset(gpu_v1.sink.userdata);
}

/* Ymir vdp.cpp VDP1SwapFramebuffer. LOPR/BEF are the previous frame's COPR/CEF
 * latched here; the erase parameters are latched at the same instant, so an
 * EWDR write mid-frame only takes effect from the next swap. */
void vdp1_swap(saturn *s)
{
    s->vdp1_lopr = s->vdp1_copr;
    s->vdp1_bef  = s->vdp1_cef;
    s->vdp1_cef  = 0;

    s->vdp1_ew_val = s->vdp1_reg[3];
    s->vdp1_ew_y1  = (uint16_t)( s->vdp1_reg[4]        & 0x1FFu);
    s->vdp1_ew_x1  = (uint16_t)(((s->vdp1_reg[4] >> 9) & 0x3Fu) << 3);
    s->vdp1_ew_y3  = (uint16_t)( s->vdp1_reg[5]        & 0x1FFu);
    s->vdp1_ew_x3  = (uint16_t)(((s->vdp1_reg[5] >> 9) & 0x7Fu) << 3);

    s->fb_draw ^= 1;
    /* SATURN_FBLOG: swaps against draws. A game that renders at 30fps draws a
     * list every OTHER field, so swapping every field displays an erased
     * buffer half the time -- which is exactly what manual swap mode exists to
     * prevent. Counting both is the only way to tell an over-swap from a
     * missing draw. */
    { static int lg = -1; static unsigned long long n;
      if (lg < 0) lg = getenv("SATURN_FBLOG") ? 1 : 0;
      if (lg && ++n <= 100000u)
          printf("[fbswap] #%llu -> draw=%d clk=%llu\n",
                 (unsigned long long)n, s->fb_draw,
                 (unsigned long long)s->clk); }

    /* PTM bit 1 ("draw on framebuffer swap") is kicked by the caller rather
     * than here. Ymir erases late in the display period, after VDP2 has
     * consumed the frame; we erase at the swap instead, because the
     * compositor runs a whole frame at a time -- so the erase has to land
     * between the flip and the draw, or it wipes the list it just drew. */
}

/* Ymir vdp_renderer_sw.cpp VDP1DoEraseFramebuffer: fill the erase window with
 * the latched EWDR value, rather than the whole buffer with zero. */
void vdp1_erase(saturn *s)
{
    /* Erase the DISPLAY buffer, not the draw buffer -- Ymir
     * VDP1DoEraseFramebuffer uses `VDP1GetDisplayFBIndex()`, i.e. the frame
     * that has just been shown and which becomes the next draw target after
     * the following swap.
     *
     * We erased `fb_draw`, and in MANUAL mode (FCM=1) that is fatal: the game
     * requests erase and swap in SEPARATE fields, so the cadence measured in
     * NiGHTS gameplay is
     *     swap -> fb_draw=0, game draws into fb0
     *     erase            <- cleared fb0, the frame just drawn
     *     swap -> fb_draw=1, ...
     * Every frame was wiped before the swap could present it, which is why the
     * sprite layer read as empty in-stage while VDP1 was rasterising millions
     * of pixels into a buffer nobody ever saw. Erasing the displayed frame
     * instead leaves the pending one intact and still has the next draw target
     * clear in time. */
    uint8_t *fb = s->vdp1_fb[s->fb_draw ^ 1];
    uint32_t x1 = s->vdp1_ew_x1, x3 = s->vdp1_ew_x3;
    uint32_t y1 = s->vdp1_ew_y1, y3 = s->vdp1_ew_y3;
    uint32_t x, y;

    if (vdp1_gpu_is_bound(s)) {
        saturn_vk_vdp1_op op;
        memset(&op, 0, sizeof op);
        op.kind = SATURN_VK_VDP1_ERASE;
        op.target = (uint32_t)(s->fb_draw ^ 1);
        op.flat = s->vdp1_ew_val;
        if (x3 <= x1 || y3 < y1) {
            op.xy[0] = 0; op.xy[1] = 0;
            op.xy[2] = FB_W; op.xy[3] = FB_H - 1;
        } else {
            if (x3 > FB_W) x3 = FB_W;
            if (y3 > FB_H - 1) y3 = FB_H - 1;
            op.xy[0] = (int32_t)x1; op.xy[1] = (int32_t)y1;
            op.xy[2] = (int32_t)x3; op.xy[3] = (int32_t)y3;
        }
        if (gpu_v1.sink.enqueue(gpu_v1.sink.userdata, &op)) return;
    }

    /* Nothing latched yet (before the first swap): a full clear is what the
     * machine looks like out of reset. */
    uint8_t *mesh = s->vdp1_meshfb[s->fb_draw ^ 1];
    if (x3 <= x1 || y3 < y1) {
        memset(fb, 0, VDP1_FB_SZ);
        memset(mesh, 0, VDP1_FB_SZ);
        return;
    }
    if (x3 > FB_W) x3 = FB_W;
    if (y3 > FB_H - 1) y3 = FB_H - 1;

    for (y = y1; y <= y3; y++) {
        uint32_t row = y * FB_W;
        for (x = x1; x < x3; x++) {
            uint32_t o = ((row + x) * 2u) & (VDP1_FB_SZ - 2u);
            fb[o]     = (uint8_t)(s->vdp1_ew_val >> 8);
            fb[o + 1] = (uint8_t)s->vdp1_ew_val;
            mesh[o] = mesh[o + 1] = 0;
        }
    }
}

/* The VDP1 register window, per Ymir vdp1_regs.hpp VDP1Regs::Read/Write.
 * Offsets are masked with 0x7FFFF -- the block does NOT mirror every 32 bytes
 * the way ours used to -- and only EDSR/LOPR/COPR/MODR are readable. */
uint16_t vdp1_read_reg(saturn *s, uint32_t off)
{
    switch (off & 0x7FFFEu) {
    case 0x10:      /* EDSR: bit 0 BEF, bit 1 CEF */
        return (uint16_t)((s->vdp1_bef ? 1u : 0u) | (s->vdp1_cef ? 2u : 0u));
    case 0x12:      /* LOPR: last operation command address / 8 */
        return (uint16_t)(s->vdp1_lopr >> 3);
    case 0x14:      /* COPR: current operation command address / 8 */
        return (uint16_t)(s->vdp1_copr >> 3);
    case 0x16: {    /* MODR: version 1 plus a read-only view of the mode bits */
        uint16_t tvmr = s->vdp1_reg[0], fbcr = s->vdp1_reg[1];
        uint16_t v = 0x1000u;                       /* VER = 0b0001 */
        v |= (uint16_t)(tvmr & 0x000Fu);            /* TVM 2-0, VBE 3 */
        v |= (uint16_t)((fbcr & 0x0002u) << 3);     /* FCM -> bit 4 */
        v |= (uint16_t)((fbcr & 0x0004u) << 3);     /* DIL -> bit 5 */
        v |= (uint16_t)((fbcr & 0x0008u) << 3);     /* DIE -> bit 6 */
        v |= (uint16_t)((fbcr & 0x0010u) << 3);     /* EOS -> bit 7 */
        return v;
    }
    default:
        return 0;   /* TVMR..ENDR are write-only; everything else reads 0 */
    }
}

void vdp1_write_reg(saturn *s, uint32_t off, uint16_t v)
{
    off &= 0x7FFFEu;
    if (off > 0x0Cu) return;            /* the status registers are read-only */
    if (getenv("SATURN_V1REGLOG"))
        printf("[v1reg] f%llu clk=%llu off=%02X val=%04X pc=%08X\n",
               (unsigned long long)s->frames, (unsigned long long)s->clk,
               (unsigned)off, v, s->cur ? s->cur->pc : s->master.pc);
    s->vdp1_reg[off >> 1] = v;

    switch (off) {
    case 0x02:      /* FBCR. The erase/swap decision is deferred to the field
                     * boundary; the hardware only latches "FBCR was written"
                     * (Ymir vdp1_regs.hpp fbParamsChanged). */
        s->vdp1_fbparams = 1;
        break;
    case 0x04:      /* PTMR. Only PTM=1 draws now; PTM=2 draws at the swap. */
        if ((v & 3u) == 1u) vdp1_begin_frame(s);
        break;
    default:
        break;
    }
}
