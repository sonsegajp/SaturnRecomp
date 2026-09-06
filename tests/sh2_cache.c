/* SH7604 write-through cache regressions. Real instruction fetches populate
 * private cache lines; ordinary stores update a local hit but never allocate
 * or snoop another CPU. DMA changes backing memory without changing either
 * CPU's cached instruction stream. */
#include "saturn.h"
#include <stdio.h>
#include <string.h>

#define CODE 0x06004000u
#define SOURCE 0x06008000u
static saturn s;
static int failures, checks;
static char scenario[96];

static void check(const char *what, uint32_t got, uint32_t expected)
{
    checks++;
    if (got != expected) {
        printf("FAIL %s: %s got %08X expected %08X\n", scenario, what, got, expected);
        failures++;
    }
}

static sh2 *cpu(unsigned slave) { return slave ? &s.slave : &s.master; }

static void seed(void)
{
    saturn_init(&s);
    sh2_reset(&s.master, &s, 0, CODE, 0x06010000u);
    sh2_reset(&s.slave, &s, 1, CODE, 0x06018000u);
    s.cur = NULL;
    for (unsigned offset = 0; offset < 32; offset += 2)
        bus_w16(&s, CODE + offset, 0x0009u);
    bus_w16(&s, CODE + 8, 0xE001u);  /* mov #1,r0 */
    bus_w16(&s, CODE + 10, 0xE302u); /* mov #2,r3 */
    s.master.onchip[0x92] = s.slave.onchip[0x92] = 1;
}

static void fetch_line(sh2 *c)
{
    c->pc = CODE;
    sh2_step(c);
    check("initial NOP executed", c->pc, CODE + 2);
}

static uint32_t cached(sh2 *c, uint32_t address, unsigned width)
{
    unsigned set = (address >> 4) & 63u;
    uint32_t tag = (address >> 10) & 0x7FFFFu;
    for (unsigned way = 0; way < 4; way++) {
        if (!c->cache_valid[set][way] || c->cache_tag[set][way] != tag) continue;
        const uint8_t *bytes = c->cache_data + (way << 10) + (set << 4) + (address & 15u);
        uint32_t value = 0;
        for (unsigned i = 0; i < width; i++) value = (value << 8) | bytes[i];
        return value;
    }
    check("expected cache line is present", 0, 1);
    return 0;
}

static void store(uint32_t address, unsigned width, uint32_t value)
{
    if (width == 1) bus_w8(&s, address, (uint8_t)value);
    else if (width == 2) bus_w16(&s, address, (uint16_t)value);
    else bus_w32(&s, address, value);
}

static void bus_write_hits(void)
{
    for (unsigned slave = 0; slave < 2; slave++) {
        for (unsigned width = 1; width <= 4; width *= 2) {
            /* ID/DD replacement-disable bits must not disable a write hit. */
            for (unsigned ccr = 1; ccr <= 7; ccr += 6) {
                snprintf(scenario, sizeof scenario, "bus %s width=%u CCR=%u", slave ? "slave" : "master", width, ccr);
                seed();
                fetch_line(&s.master); fetch_line(&s.slave);
                sh2 *local = cpu(slave), *other = cpu(!slave);
                local->onchip[0x92] = (uint8_t)ccr;
                s.cur = local;
                uint32_t address = CODE + (width == 1 ? 9 : 8);
                uint32_t value = width == 1 ? 7 : width == 2 ? 0xE007u : 0xE007E308u;
                store(address, width, value);
                check("write reaches backing memory", bus_r32(&s, CODE + 8), width == 4 ? 0xE007E308u : 0xE007E302u);
                check("local cached opcode is updated", cached(local, CODE + 8, 4), width == 4 ? 0xE007E308u : 0xE007E302u);
                check("other CPU's cached opcode remains private", cached(other, CODE + 8, 4), 0xE001E302u);
                local->pc = CODE + 8; sh2_step(local);
                check("local fetch executes updated opcode", local->r[0], 7);
                other->pc = CODE + 8; sh2_step(other);
                check("other CPU executes its private opcode", other->r[0], 1);
            }
        }
    }
}

static void bypass_and_misses(void)
{
    for (unsigned slave = 0; slave < 2; slave++) {
        for (unsigned width = 1; width <= 4; width *= 2) {
            for (unsigned mode = 0; mode < 3; mode++) {
                snprintf(scenario, sizeof scenario, "bypass %s width=%u mode=%u", slave ? "slave" : "master", width, mode);
                seed();
                sh2 *local = cpu(slave);
                fetch_line(local);
                sh2 before = *local;
                uint32_t address = CODE + (width == 1 ? 9 : 8);
                if (mode == 0) local->onchip[0x92] = 0; /* disabled */
                if (mode == 1) address |= 0x20000000u; /* cache-through */
                if (mode == 2) address += 0x400u;      /* same set, different tag */
                s.cur = local;
                store(address, width, 0x10203040u);
                uint32_t expected = width == 1 ? 0x40u : width == 2 ? 0x3040u : 0x10203040u;
                uint32_t actual = width == 1 ? bus_r8(&s, address) : width == 2 ? bus_r16(&s, address) : bus_r32(&s, address);
                check("bypass/miss reaches memory", actual, expected);
                check("cache bytes unchanged", memcmp(before.cache_data, local->cache_data, sizeof before.cache_data) == 0, 1);
                check("cache tags unchanged", memcmp(before.cache_tag, local->cache_tag, sizeof before.cache_tag) == 0, 1);
                check("no line allocated or invalidated", memcmp(before.cache_valid, local->cache_valid, sizeof before.cache_valid) == 0, 1);
                check("replacement state unchanged", memcmp(before.cache_lru, local->cache_lru, sizeof before.cache_lru) == 0, 1);
            }
        }
    }
}

static void write_hit_lru(void)
{
    snprintf(scenario, sizeof scenario, "write hit then sequential instruction fetch LRU");
    seed();
    bus_w16(&s, CODE + 0x400u, 0x0009u);
    fetch_line(&s.master);
    s.master.pc = CODE + 0x400u;
    sh2_step(&s.master); /* Another way in the same set. */
    fetch_line(&s.master);
    unsigned set = (CODE >> 4) & 63u;
    uint8_t before = s.master.cache_lru[set];
    bus_w16(&s, CODE + 0x400u, 0xE007u);
    check("write hit touches the other cache way", s.master.cache_lru[set] != before, 1);
    /* CODE+2 still uses the sequential-line fetch shortcut. It must become
     * most-recently-used again after the intervening write to another way. */
    sh2_step(&s.master);
    check("sequential fetch restores its way's LRU position", s.master.cache_lru[set], before);
}

static void guest_self_modification(void)
{
    for (unsigned slave = 0; slave < 2; slave++) {
        for (unsigned fast = 0; fast < 2; fast++) {
            for (unsigned width = 1; width <= 4; width *= 2) {
                for (unsigned through = 0; through < 2; through++) {
                    snprintf(scenario, sizeof scenario, "guest %s %s width=%u through=%u", slave ? "slave" : "master", fast ? "run" : "step", width, through);
                    seed();
                    /* The first NOP fetch fills the line before MOV patches a
                     * later instruction in the same line. No explicit purge. */
                    s.cur = NULL;
                    bus_w16(&s, CODE + 2, width == 1 ? 0x2210u : width == 2 ? 0x2211u : 0x2212u); /* mov.b/w/l r1,@r2 */
                    bus_w16(&s, CODE + 12, 0xAFFEu); /* bra . ; nop */
                    sh2 *local = cpu(slave);
                    local->r[1] = width == 1 ? 7 : width == 2 ? 0xE007u : 0xE007E308u;
                    local->r[2] = (CODE + (width == 1 ? 9 : 8)) | (through ? 0x20000000u : 0);
                    if (fast) sh2_run(local, 12);
                    else for (unsigned i = 0; i < 6; i++) sh2_step(local);
                    check("guest store reaches memory", bus_r16(&s, CODE + 8), 0xE007u);
                    check("guest executes the correct cached instruction", local->r[0], through ? 1 : 7);
                    check("long store updates both instructions", local->r[3], !through && width == 4 ? 8 : 2);
                }
            }
        }
    }
}

static void dma_private_cache(void)
{
    for (unsigned slave = 0; slave < 2; slave++) {
        for (unsigned transfer = 0; transfer < 6; transfer++) {
            snprintf(scenario, sizeof scenario, "%s DMA initiator=%s", transfer == 5 ? "DSP" : transfer == 4 ? "SCU" : "SH2", slave ? "slave" : "master");
            seed();
            /* Prime both copies, then start DMA while a CPU is current. Its
             * bus callbacks must not be mistaken for ordinary CPU stores. */
            fetch_line(&s.master); fetch_line(&s.slave);
            s.cur = cpu(slave);
            bus_w32(&s, SOURCE, 0xE007E308u);
            uint32_t destination = CODE + (transfer == 0 ? 9 : 8);
            if (transfer == 0) bus_w8(&s, SOURCE, 7);
            if (transfer < 4) {
                bus_w32(&s, 0xFFFFFF80u, SOURCE);
                bus_w32(&s, 0xFFFFFF84u, destination);
                bus_w32(&s, 0xFFFFFF88u, transfer == 3 ? 4 : 1);
                bus_w32(&s, 0xFFFFFFB0u, 1);
                bus_w32(&s, 0xFFFFFF8Cu, 0x5001u | (transfer << 10));
                check("SH2 DMA completes", bus_r32(&s, 0xFFFFFF88u), 0);
                check("SH2 DMA transfer-end flag", bus_r32(&s, 0xFFFFFF8Cu) & 3u, 2);
            } else if (transfer == 4) {
                bus_w32(&s, 0x25FE0000u, SOURCE);
                bus_w32(&s, 0x25FE0004u, destination);
                bus_w32(&s, 0x25FE0008u, 4);
                bus_w32(&s, 0x25FE000Cu, 0x102u);
                bus_w32(&s, 0x25FE0014u, 7);
                bus_w32(&s, 0x25FE0010u, 0x101u);
                check("SCU DMA completes", bus_r32(&s, 0x25FE0010u), 0);
            } else {
                /* Load MD0 and a real DSP program: MVI destination,WA0;
                 * DMA MD0,D0,1 (stride 4); END. Start via the SCU registers. */
                bus_w32(&s, 0x25FE0088u, 0);
                bus_w32(&s, 0x25FE008Cu, 0xE007E308u);
                bus_w32(&s, 0x25FE0080u, 0x8000u);
                bus_w32(&s, 0x25FE0084u, 0x9C000000u | (destination >> 2));
                bus_w32(&s, 0x25FE0084u, 0xC0011001u);
                bus_w32(&s, 0x25FE0084u, 0xF0000000u);
                bus_w32(&s, 0x25FE0080u, 0x18000u);
                scu_dsp_tick(&s, 16);
                check("DSP program reaches END", s.scu_dsp.executing, 0);
                check("DSP DMA completes", s.scu_dsp.dma_run, 0);
                check("DSP DMA advances source bank", s.scu_dsp.ct[0], 1);
            }
            check("DMA writes backing code", bus_r16(&s, CODE + 8), 0xE007u);
            check("DMA keeps initiating CPU context", s.cur == cpu(slave), 1);
            check("DMA restores ordinary CPU write behavior", s.dma_bus_depth, 0);
            for (unsigned reader = 0; reader < 2; reader++) {
                sh2 *local = cpu(reader);
                check("DMA leaves CPU cached opcode unchanged", cached(local, CODE + 8, 2), 0xE001u);
                local->pc = CODE + 8; sh2_step(local);
                check("CPU still executes private cached opcode", local->r[0], 1);
            }
        }
    }
}

int main(void)
{
    bus_write_hits();
    bypass_and_misses();
    write_hit_lru();
    guest_self_modification();
    dma_private_cache();
    printf("%s SH-2 cache write-through, self-modifying code and DMA: %d/%d failures\n", failures ? "FAIL" : "PASS", failures, checks);
    return failures != 0;
}
