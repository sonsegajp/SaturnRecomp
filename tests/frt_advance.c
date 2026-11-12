/* Event-free FRT spans must agree with the former tick-by-tick scheduler.
 * Check complete CPU state, including untouched register/cache/debug data. */
#include "saturn.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OC(off) (((off) - 0xFE00u) & 0x1FFu)
extern int g_frt_irq;
static sh2 initial, expected, actual;
static uint32_t rng = 0x19A7362Bu;
static unsigned checks, failures;
static int disabled;

static uint32_t random32(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

static void compare_value(unsigned address, uint16_t value)
{
    initial.onchip[OC(address)] = (uint8_t)(value >> 8);
    initial.onchip[OC(address) + 1] = (uint8_t)value;
}

/* Deliberately retain the simple one-tick oracle. A compare-A clear happens
 * before compare B observes the counter, and also resets the prescaler. */
static void reference(sh2 *c, uint32_t cycles)
{
    unsigned mode = c->onchip[OC(0xFE16u)] & 3u;
    uint32_t divisor, ticks;
    uint16_t a, b;
    if (disabled || mode == 3u) return;
    divisor = 8u << (mode * 2u);
    c->frt_pre += cycles;
    ticks = c->frt_pre / divisor;
    if (!ticks) return;
    c->frt_pre -= ticks * divisor;
    a = (uint16_t)((c->onchip[OC(0xFE14u)] << 8) | c->onchip[OC(0xFE15u)]);
    b = (uint16_t)((c->onchip[OC(0xFE1Au)] << 8) | c->onchip[OC(0xFE1Bu)]);
    while (ticks--) {
        uint16_t previous = c->frc;
        c->frc = (uint16_t)(c->frc + 1u);
        if (c->frc < previous) c->onchip[OC(0xFE11u)] |= 2u;
        if (c->frc == a) {
            c->onchip[OC(0xFE11u)] |= 8u;
            if (c->onchip[OC(0xFE11u)] & 1u) {
                c->frc = 0;
                c->frt_pre = 0;
            }
        }
        if (c->frc == b) c->onchip[OC(0xFE11u)] |= 4u;
        /* Compare-match and overflow are always deliverable; input capture
         * only on the slave or with SATURN_FRTIRQ. The cached flag also
         * covers a raised WDT interval interrupt (OVF set, WT/IT clear). */
        c->frt_pend =
            (int)((c->onchip[OC(0xFE10u)] & c->onchip[OC(0xFE11u)] &
                   ((g_frt_irq || c->is_slave) ? 0x8Eu : 0x0Eu)) != 0) ||
            (c->onchip[OC(0xFE80u)] & 0xC0u) == 0x80u;
    }
}

static void compare(uint32_t cycles)
{
    expected = initial;
    actual = initial;
    reference(&expected, cycles);
    frt_advance(&actual, cycles);
    ++checks;
    if (memcmp(&actual, &expected, sizeof actual)) {
        if (failures++ < 8)
            printf("FAIL case %u clocks=%u FRC=%04X prescale=%u TCR=%02X "
                   "expected=%04X/%u/%02X/%d actual=%04X/%u/%02X/%d\n",
                   checks, cycles, initial.frc, initial.frt_pre,
                   initial.onchip[OC(0xFE16u)], expected.frc, expected.frt_pre,
                   expected.onchip[OC(0xFE11u)], expected.frt_pend,
                   actual.frc, actual.frt_pre,
                   actual.onchip[OC(0xFE11u)], actual.frt_pend);
    }
}

static void known(const char *name, uint16_t counter, uint8_t flags,
                  uint32_t prescale, int pending)
{
    ++checks;
    if (actual.frc != counter || actual.onchip[OC(0xFE11u)] != flags ||
        actual.frt_pre != prescale || actual.frt_pend != pending) {
        ++failures;
        printf("FAIL explicit %s\n", name);
    }
}

int main(void)
{
    disabled = getenv("SATURN_NOFRT") != NULL;
    memset(&initial, 0xA5, sizeof initial);
    for (unsigned n = 0; n < 100000; ++n) {
        initial.frt_pre = random32() & 127u;
        initial.frc = (uint16_t)random32();
        initial.frt_pend = (int)(random32() & 1u);
        initial.onchip[OC(0xFE10u)] = (uint8_t)random32();
        initial.onchip[OC(0xFE11u)] = (uint8_t)random32();
        initial.onchip[OC(0xFE16u)] = (uint8_t)random32();
        compare_value(0xFE14u, (uint16_t)random32());
        compare_value(0xFE1Au, (uint16_t)random32());
        switch (n & 7u) {
        case 0:
            compare_value(0xFE14u, initial.frc);
            compare_value(0xFE1Au, initial.frc);
            break;
        case 1:
            compare_value(0xFE14u, (uint16_t)(initial.frc + 1u));
            initial.onchip[OC(0xFE11u)] |= 1u;
            break;
        case 2:
            compare_value(0xFE14u, 0); compare_value(0xFE1Au, 0);
            initial.frc = 65530;
            break;
        case 3:
            compare_value(0xFE14u, 1); compare_value(0xFE1Au, 0);
            initial.onchip[OC(0xFE11u)] |= 1u;
            break;
        }
        g_frt_irq = (int)((n >> 3) & 1u);
        uint32_t cycles = random32() % 4097u;
        if (n % 1000u == 0) cycles = 2000000u;
        if (n % 256u == 0) {
            initial.frt_pre = UINT32_MAX - 127u;
            cycles = 256;
        }
        compare(cycles);
    }

    for (unsigned mode = 0; mode < 4; ++mode) {
        for (unsigned clear = 0; clear < 2; ++clear) {
            memset(&initial, 0, sizeof initial);
            initial.onchip[OC(0xFE16u)] = (uint8_t)mode;
            initial.onchip[OC(0xFE11u)] = (uint8_t)clear;
            initial.onchip[OC(0xFE10u)] = 0xFF;
            compare_value(0xFE14u, clear ? 123 : 0xFFFF);
            compare_value(0xFE1Au, 0);
            initial.frc = 65530;
            g_frt_irq = 1;
            compare(0x01000000u); /* many wraps/clear events in one span */
            initial.frt_pre = UINT32_MAX - 8u;
            compare(16); /* preserve the existing unsigned accumulator wrap */
        }
    }

    if (!disabled) {
        memset(&initial, 0, sizeof initial);
        initial.frc = 100;
        compare_value(0xFE14u, 200); compare_value(0xFE1Au, 300);
        compare(128); known("event-free span", 116, 0, 0, 0);
        initial.frc = 200;
        compare_value(0xFE1Au, 200);
        compare(8); known("compare equality is not an immediate event", 201, 0, 0, 0);
        initial.frc = 65535;
        compare_value(0xFE14u, 123); compare_value(0xFE1Au, 0);
        compare(8); known("overflow and B match", 0, 6, 0, 0);
        initial.frc = 0;
        initial.frt_pre = 7;
        initial.onchip[OC(0xFE10u)] = 0x0C;
        initial.onchip[OC(0xFE11u)] = 1;
        compare_value(0xFE14u, 1);
        compare(2); known("A clear before B match resets prescaler", 0, 13, 0, 1);
    }

    /* Master FRT delivery: compare-match always, input capture only with the
     * SATURN_FRTIRQ switch. IPRB level 5, VCRC = ICI 0x64 / OCI 0x65. */
    {
        int vec = 0, lvl = 0;
        static sh2 m;
        static saturn sys;                            /* frt_pending counts into it */
        memset(&m, 0, sizeof m);
        m.sys = &sys;
        m.onchip[OC(0xFE60u)] = 0x05;
        m.onchip[OC(0xFE66u)] = 0x64; m.onchip[OC(0xFE67u)] = 0x65;
        m.onchip[OC(0xFE10u)] = 0x88;                 /* ICIE | OCIAE */
        m.onchip[OC(0xFE11u)] = 0x08;                 /* OCFA */
        g_frt_irq = 0;
        ++checks;
        if (!frt_pending(&m, 0, &vec, &lvl) || vec != 0x65 || lvl != 5) {
            ++failures; printf("FAIL master OCIA delivered by default\n");
        }
        m.onchip[OC(0xFE11u)] = 0x80;                 /* ICF only */
        ++checks;
        if (frt_pending(&m, 0, &vec, &lvl)) {
            ++failures; printf("FAIL master ICI held back without switch\n");
        }
        g_frt_irq = 1;
        ++checks;
        if (!frt_pending(&m, 0, &vec, &lvl) || vec != 0x64) {
            ++failures; printf("FAIL master ICI delivered with switch\n");
        }
        g_frt_irq = 0;
    }

    /* Watchdog timer (SH7604 WDT; Ymir hw/sh2/sh2_wdt.hpp). */
    {
        int vec = 0, lvl = 0;
        static sh2 w;
        memset(&w, 0, sizeof w);
        w.onchip[OC(0xFE80u)] = 0x18; w.onchip[OC(0xFE83u)] = 0x1F;
        w.onchip[OC(0xFEE3u)] = 0xF0;                 /* IPRA WDT level 15 */
        w.onchip[OC(0xFEE4u)] = 0x68;                 /* VCRWDT ITI vector */
        wdt_advance(&w, 100000);
        ++checks;
        if (w.onchip[OC(0xFE81u)] != 0) { ++failures; printf("FAIL WDT idle without TME\n"); }
        wdt_write16(&w, 0xFE80u, 0xA522);             /* TME, interval, phi/128 */
        ++checks;
        if (w.onchip[OC(0xFE80u)] != 0x3A) { ++failures; printf("FAIL WTCSR write\n"); }
        wdt_write16(&w, 0xFE80u, 0x5AF0);             /* WTCNT = 0xF0 */
        wdt_advance(&w, 128u * 15u + 127u);
        ++checks;
        if (w.onchip[OC(0xFE81u)] != 0xFF || (w.onchip[OC(0xFE80u)] & 0x80u)) {
            ++failures; printf("FAIL WDT counts at phi/128\n");
        }
        wdt_advance(&w, 1);
        ++checks;
        if (w.onchip[OC(0xFE81u)] != 0 || !(w.onchip[OC(0xFE80u)] & 0x80u) || !w.frt_pend) {
            ++failures; printf("FAIL WDT overflow sets OVF\n");
        }
        ++checks;
        if (!wdt_pending(&w, 0, &vec, &lvl) || vec != 0x68 || lvl != 15) {
            ++failures; printf("FAIL WDT ITI vector/level\n");
        }
        ++checks;
        if (wdt_pending(&w, 0xF0, &vec, &lvl)) { ++failures; printf("FAIL WDT ITI masked by SR.I\n"); }
        wdt_write16(&w, 0xFE80u, 0xA522);             /* 0 to OVF before any read */
        ++checks;
        if (!(w.onchip[OC(0xFE80u)] & 0x80u)) { ++failures; printf("FAIL OVF clear needs a read first\n"); }
        wdt_note_read(&w, 0xFE80u);
        wdt_write16(&w, 0xFE80u, 0xA522);
        ++checks;
        if ((w.onchip[OC(0xFE80u)] & 0x80u) || w.frt_pend || wdt_pending(&w, 0, &vec, &lvl)) {
            ++failures; printf("FAIL OVF cleared after read\n");
        }
        w.onchip[OC(0xFE81u)] = 0x10;
        wdt_write16(&w, 0xFE80u, 0xA500);             /* TME off clears WTCNT */
        ++checks;
        if (w.onchip[OC(0xFE81u)] != 0) { ++failures; printf("FAIL TME off clears WTCNT\n"); }
        wdt_write16(&w, 0xFE80u, 0xA560);             /* watchdog mode, phi/2 */
        wdt_write16(&w, 0xFE80u, 0x5AFF);
        wdt_advance(&w, 2);
        ++checks;
        if (!(w.onchip[OC(0xFE83u)] & 0x80u) || (w.onchip[OC(0xFE80u)] & 0x80u) ||
            wdt_pending(&w, 0, &vec, &lvl)) {
            ++failures; printf("FAIL watchdog-mode overflow sets WOVF only\n");
        }
        wdt_write16(&w, 0xFE82u, 0xA500);             /* clear WOVF */
        ++checks;
        if (w.onchip[OC(0xFE83u)] != 0x1F) { ++failures; printf("FAIL RSTCSR WOVF clear\n"); }
    }
    printf("FRT advance: %u checks, %u failures (%s)\n",
           checks, failures, disabled ? "disabled" : "enabled");
    return failures != 0;
}
