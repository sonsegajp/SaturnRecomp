/* bus_alias.c -- does memory read back what was written, through every path?
 *
 * Three separate investigations hit the same wall: a byte written once, never
 * overwritten, reads back as zero later (0x06012CCC, 0x06012CCD, 0x06005F9E).
 * Rather than chase a fourth, check the bus directly: every width, every
 * mirror, master and slave.
 *
 * The SH-2 selects cache behaviour with address bits 31-29, so 0x06xxxxxx,
 * 0x26xxxxxx and 0x2xxxxxxx variants must all reach the same WRAM byte.
 */
#include "saturn.h"
#include "vulkan_renderer.h"
#include <stdio.h>
#include <string.h>

static saturn S;
static int fails;
static saturn_vk_vdp1_op gpu_ops[8];
static unsigned gpu_n;

static int gpu_enqueue(void *userdata, const saturn_vk_vdp1_op *op)
{
    (void)userdata;
    if (gpu_n < sizeof gpu_ops / sizeof gpu_ops[0]) gpu_ops[gpu_n++] = *op;
    return 1;
}

static void ck(const char *what, uint32_t got, uint32_t want)
{
    if (got != want) {
        printf("  FAIL %-40s got %08X want %08X\n", what, got, want);
        fails++;
    }
}

int main(void)
{
    static const uint32_t wram_h = 0x06005F9E;   /* the address that bit us */
    static const uint32_t mirrors[] = {
        0x06005F9E, 0x26005F9E, 0x00205F9E + 0x06000000 - 0x06000000
    };
    memset(&S, 0, sizeof S);

    /* 1. byte write, byte read, same address. */
    bus_w8(&S, wram_h, 0x60);
    ck("w8 -> r8 same address", bus_r8(&S, wram_h), 0x60);

    /* 2. byte write, WORD read spanning it (this is what an SH-2 fetch does). */
    bus_w8(&S, wram_h + 1, 0x20);
    ck("w8 pair -> r16 (opcode fetch)", bus_r16(&S, wram_h), 0x6020);

    /* 3. word write, byte read back. */
    bus_w16(&S, wram_h, 0x1234);
    ck("w16 -> r8 high", bus_r8(&S, wram_h), 0x12);
    ck("w16 -> r8 low",  bus_r8(&S, wram_h + 1), 0x34);

    /* 4. long write, word/byte read back. */
    bus_w32(&S, 0x06012CCC, 0x00010000);
    ck("w32 -> r32", bus_r32(&S, 0x06012CCC), 0x00010000);
    ck("w32 -> r16 high", bus_r16(&S, 0x06012CCC), 0x0001);
    ck("w32 -> r8 [1]", bus_r8(&S, 0x06012CCD), 0x01);

    /* 5. byte writes assembling a long, as the BIOS decompressor does. */
    bus_w8(&S, 0x06012CCC, 0x00);
    bus_w8(&S, 0x06012CCD, 0x01);
    bus_w8(&S, 0x06012CCE, 0x00);
    bus_w8(&S, 0x06012CCF, 0x00);
    ck("bytewise -> r32 (literal pool)", bus_r32(&S, 0x06012CCC), 0x00010000);

    /* 6. cache-through mirror must alias to the same storage. */
    bus_w32(&S, 0x06010000, 0xDEADBEEF);
    ck("0x06 write -> 0x26 read", bus_r32(&S, 0x26010000), 0xDEADBEEF);
    bus_w32(&S, 0x26010004, 0xCAFEF00D);
    ck("0x26 write -> 0x06 read", bus_r32(&S, 0x06010004), 0xCAFEF00D);

    /* 7. low WRAM (WRAM-L) and the high mirror. */
    bus_w32(&S, 0x00200000, 0x11223344);
    ck("WRAM-L w32 -> r32", bus_r32(&S, 0x00200000), 0x11223344);
    ck("WRAM-L 0x20 mirror", bus_r32(&S, 0x20200000), 0x11223344);

    /* 8. Direct VDP1 framebuffer writes must also be sent to Vulkan in bus
     * order. The CPU shadow remains readable and the replaced pixels lose any
     * stale transparent-mesh metadata. */
    {
        saturn_vdp1_gpu_sink sink = { NULL, gpu_enqueue, NULL };
        S.fb_draw = 1;
        memset(S.vdp1_meshfb[1], 0xFF, sizeof S.vdp1_meshfb[1]);
        vdp1_gpu_bind(&S, &sink);
        bus_w8 (&S, 0x05C80001u, 0x34u);
        bus_w16(&S, 0x05C80002u, 0x5678u);
        bus_w32(&S, 0x05C80004u, 0x9ABCDEF0u);
        ck("VDP1 FB CPU shadow", bus_r32(&S, 0x05C80004u), 0x9ABCDEF0u);
        ck("VDP1 FB mesh clear byte write", S.vdp1_meshfb[1][0], 0u);
        ck("VDP1 FB mesh clear long end", S.vdp1_meshfb[1][7], 0u);
        ck("VDP1 FB write op count", gpu_n, 3u);
        ck("VDP1 FB byte op kind", gpu_ops[0].kind, SATURN_VK_VDP1_FB_WRITE);
        ck("VDP1 FB byte op target", gpu_ops[0].target, 1u);
        ck("VDP1 FB byte op offset", gpu_ops[0].chr, 1u);
        ck("VDP1 FB word op payload", gpu_ops[1].flat, 0x5678u);
        ck("VDP1 FB long op payload", gpu_ops[2].flat, 0x9ABCDEF0u);
        vdp1_gpu_bind(&S, NULL);
    }

    /* 9. TVMR.VBE enabled from the V-Blank handler belongs to the active
     * blanking interval.  Sonic R writes it on the same boundary clock; losing
     * this latch leaves the previous scene's VDP1 sprites in the next buffer. */
    S.vdp1_vblank_erase = 0;
    S.vdp2_reg[0x04 >> 1] = 0x0008u;
    vdp1_write_reg(&S, 0x00u, 0x0008u);
    ck("VDP1 VBE latches during V-Blank", S.vdp1_vblank_erase, 1u);
    S.vdp1_vblank_erase = 0;
    S.vdp2_reg[0x04 >> 1] = 0;
    vdp1_write_reg(&S, 0x00u, 0x0008u);
    ck("VDP1 VBE waits outside V-Blank", S.vdp1_vblank_erase, 0u);

    /* 10. The SH-2 reaches SCSP registers through the SCU bus window at
     * 0x05B00000-0x05BFFFFF. Sonic R programs the two CDDA/EXTS mixers here;
     * dropping these writes produces decoded CD audio that is permanently
     * muted in the final mix. The register block mirrors every 0x1000 bytes. */
    bus_w16(&S, 0x05B00216u, 0x00E0u);
    ck("SCSP SH-2 word write/read", bus_r16(&S, 0x05B00216u), 0x00E0u);
    bus_w8(&S, 0x05B00216u, 0x12u);
    bus_w8(&S, 0x05B00217u, 0x34u);
    ck("SCSP SH-2 byte writes merge", bus_r16(&S, 0x05B00216u), 0x1234u);
    bus_w32(&S, 0x05B01214u, 0x00E010E0u);
    ck("SCSP SH-2 long write high", bus_r16(&S, 0x05B00214u), 0x00E0u);
    ck("SCSP SH-2 long write low", bus_r16(&S, 0x05B00216u), 0x10E0u);
    ck("SCSP SH-2 mirror read", bus_r32(&S, 0x05B02214u), 0x00E010E0u);

    (void)mirrors;
    if (fails == 0) printf("PASS  bus aliasing, VDP1 FB writes, VBE, and SCSP bus (27 checks)\n");
    else            printf("FAIL  bus aliasing: %d\n", fails);
    return fails ? 1 : 0;
}
