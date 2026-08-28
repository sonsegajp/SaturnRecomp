/* m68k_bus.c -- the sound CPU's view of memory.
 *
 * Kept apart from m68k.c so the core stays a plain 68000 that a unit test can
 * drive against a bare array, and apart from bus.c because the 68000 sees a
 * completely different map from the SH-2s: sound RAM at zero, SCSP registers
 * at 0x100000, nothing else.
 */
#include "saturn.h"
#include "m68k.h"

static saturn *sys_of(m68k *m) { return (saturn *)m->sys; }

/* Sound RAM wraps within its 512KB: the 68000's address bus is 24 bits and
 * the driver relies on the mirror. */
static uint32_t sram_off(uint32_t a) { return a & (SOUND_RAM_SZ - 1u); }

uint8_t m68k_r8(m68k *m, uint32_t a)
{
    saturn *s = sys_of(m);
    a &= 0x00FFFFFFu;
    if (!s) return 0;
    if (a < 0x100000u) return s->sound_ram[sram_off(a)];
    if (a < 0x101000u) {
        uint16_t w = scsp_read(s, (a - 0x100000u) & ~1u);
        return (uint8_t)((a & 1u) ? (w & 0xFFu) : (w >> 8));
    }
    return 0;
}

uint16_t m68k_r16(m68k *m, uint32_t a)
{
    saturn *s = sys_of(m);
    a &= 0x00FFFFFEu;
    if (!s) return 0;
    if (a < 0x100000u) {
        uint32_t o = sram_off(a);
        return (uint16_t)((s->sound_ram[o] << 8) | s->sound_ram[o + 1u]);
    }
    if (a < 0x101000u) return scsp_read(s, a - 0x100000u);
    return 0;
}

uint32_t m68k_r32(m68k *m, uint32_t a)
{
    return ((uint32_t)m68k_r16(m, a) << 16) | m68k_r16(m, a + 2u);
}

void m68k_w8(m68k *m, uint32_t a, uint8_t v)
{
    saturn *s = sys_of(m);
    a &= 0x00FFFFFFu;
    if (!s) return;
    if (a < 0x100000u) { s->sound_ram[sram_off(a)] = v; return; }
    if (a < 0x101000u) {
        /* Byte writes to a 16-bit register file: merge into the word. */
        uint32_t ra = (a - 0x100000u) & ~1u;
        uint16_t w = scsp_read(s, ra);
        w = (uint16_t)((a & 1u) ? ((w & 0xFF00u) | v) : ((w & 0x00FFu) | ((uint16_t)v << 8)));
        scsp_write(s, ra, w);
    }
}

void m68k_w16(m68k *m, uint32_t a, uint16_t v)
{
    saturn *s = sys_of(m);
    a &= 0x00FFFFFEu;
    if (!s) return;
    if (a < 0x100000u) {
        uint32_t o = sram_off(a);
        s->sound_ram[o]      = (uint8_t)(v >> 8);
        s->sound_ram[o + 1u] = (uint8_t)v;
        return;
    }
    if (a < 0x101000u) scsp_write(s, a - 0x100000u, v);
}

void m68k_w32(m68k *m, uint32_t a, uint32_t v)
{
    m68k_w16(m, a,      (uint16_t)(v >> 16));
    m68k_w16(m, a + 2u, (uint16_t)v);
}
