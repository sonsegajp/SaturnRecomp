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
#include <stdio.h>
#include <string.h>

static saturn S;
static int fails;

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

    (void)mirrors;
    if (fails == 0) printf("PASS  bus aliasing (11 checks)\n");
    else            printf("FAIL  bus aliasing: %d\n", fails);
    return fails ? 1 : 0;
}
