/* SH-2 sound-bus wait states are observable by real counted command polls.
 * Verify instruction/data semantics, cumulative scheduler budgets and DMA
 * isolation; no game media, host audio device or renderer is required. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "saturn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CODE 0x06004000u
#define RAM  0x06008000u
#define SOUND 0x05A00400u
static saturn s;
static unsigned checks, failures;
static char scenario[128];

static void check(const char *what, uint64_t actual, uint64_t expected)
{
    ++checks;
    if (actual != expected) {
        ++failures;
        printf("FAIL %s: %s got %llu expected %llu\n", scenario, what,
               (unsigned long long)actual, (unsigned long long)expected);
    }
}
static sh2 *cpu(unsigned slave) { return slave ? &s.slave : &s.master; }
static void seed(void)
{
    saturn_init(&s);
    sh2_reset(&s.master, &s, 0, CODE, 0x06010000u);
    sh2_reset(&s.slave, &s, 1, CODE, 0x06018000u);
    s.cur = NULL;
    for (unsigned i = 0; i < 64; i += 2) bus_w16(&s, CODE + i, 0x0009);
}
static uint32_t read_width(uint32_t address, unsigned width)
{
    return width == 1 ? bus_r8(&s, address) : width == 2 ? bus_r16(&s, address) : bus_r32(&s, address);
}
static void write_width(uint32_t address, unsigned width, uint32_t value)
{
    if (width == 1) bus_w8(&s, address, (uint8_t)value);
    else if (width == 2) bus_w16(&s, address, (uint16_t)value);
    else bus_w32(&s, address, value);
}

static void instruction_accesses(void)
{
    /* Cached/cache-through areas, the sound-RAM mirror, and the SCSP
     * register mirror all resolve to the same slow external bus. */
    const uint32_t addresses[] = { SOUND, SOUND + 0x80000u, SOUND | 0x20000000u,
                                  0x05B00004u, 0x05B01004u, 0x25B00004u };
    for (unsigned slave = 0; slave < 2; ++slave)
    for (unsigned fast = 0; fast < 2; ++fast)
    for (unsigned a = 0; a < sizeof addresses / sizeof *addresses; ++a)
    for (unsigned width = 1; width <= 4; width *= 2)
    for (unsigned write = 0; write < 2; ++write) {
        snprintf(scenario, sizeof scenario, "%s %s MOV.%u %s %08X",
            slave ? "slave" : "master", fast ? "run" : "step", width,
            write ? "write" : "read", addresses[a]);
        seed();
        sh2 *c = cpu(slave), *other = cpu(!slave);
        unsigned offset = width == 1 ? 1 : width == 2 ? 2 : 0;
        uint32_t address = addresses[a] + offset;
        bus_w32(&s, addresses[a], 0x8192A3B4u);
        c->r[1] = address;
        c->r[2] = 0x91A2B3C4u;
        other->cycles = 777;
        unsigned code = width == 1 ? 0 : width == 2 ? 1 : 2;
        bus_w16(&s, CODE, (uint16_t)((write ? 0x2120 : 0x6010) | code));
        if (fast) sh2_run(c, 1); else sh2_step(c);
        check("instruction cycle total", c->cycles, write ? 2 : 40);
        check("one instruction advances PC", c->pc, CODE + 2);
        check("other CPU cycles unchanged", other->cycles, 777);
        check("CPU access scope restored", s.cpu_bus_active, 0);
        check("no CPU fault", c->halted, 0);
        if (write) {
            uint32_t expected = width == 1 ? 0x81C4A3B4u : width == 2 ? 0x8192B3C4u : 0x91A2B3C4u;
            check("store width and byte ordering", bus_r32(&s, addresses[a]), expected);
        } else {
            uint32_t expected = width == 1 ? 0xFFFFFF92u : width == 2 ? 0xFFFFA3B4u : 0x8192A3B4u;
            check("load value and sign extension", c->r[0], expected);
        }
        uint64_t before = c->cycles;
        read_width(address, width);
        write_width(address, width, 0);
        check("diagnostics outside instruction scope are free", c->cycles, before);
    }
}

static void scheduler_carry(void)
{
    for (unsigned slave = 0; slave < 2; ++slave) {
        snprintf(scenario, sizeof scenario, "%s scheduler overshoot", slave ? "slave" : "master");
        seed(); sh2 *c = cpu(slave);
        c->r[1] = SOUND;
        bus_w16(&s, CODE, 0x6010);     /* MOV.B @R1,R0: 40 clocks */
        bus_w16(&s, CODE + 2, 0x7201); /* ADD #1,R2: one clock */
        sh2_run(c, 1);
        check("first slice completes slow read", c->cycles, 40);
        check("first slice owes one clock", c->run_target, 1);
        check("38 clocks pay existing overshoot", sh2_run(c, 38), 0);
        check("last owed clock does not execute ahead", sh2_run(c, 1), 0);
        check("no premature next instruction", c->r[2], 0);
        check("next budget executes one instruction", sh2_run(c, 1), 1);
        check("next instruction runs at clock 41", c->cycles, 41);
        check("register updated once", c->r[2], 1);
        check("scope restored after empty slices", s.cpu_bus_active, 0);
    }
}

static void folded_sound_polls(void)
{
    static const uint16_t byte_poll[] = {0x6010, 0xC901, 0x2008, 0x89FB};
    static const uint16_t word_poll[] = {0x6011, 0x600D, 0x2008, 0x8DFB, 0xE000};
    for (unsigned slave = 0; slave < 2; ++slave)
    for (unsigned word = 0; word < 2; ++word) {
        uint64_t cycles[2]; uint32_t pc[2], sr[2];
        snprintf(scenario, sizeof scenario, "%s folded sound %s poll", slave ? "slave" : "master", word ? "word" : "byte");
        for (unsigned fast = 0; fast < 2; ++fast) {
            seed(); sh2 *c = cpu(slave); c->r[1] = SOUND;
            const uint16_t *program = word ? word_poll : byte_poll;
            unsigned count = word ? 5 : 4;
            for (unsigned i = 0; i < count; ++i) bus_w16(&s, CODE + i * 2, program[i]);
            if (fast) sh2_run(c, 200);
            else while (c->cycles < 200 && !c->halted) sh2_step(c);
            cycles[fast] = c->cycles; pc[fast] = c->pc; sr[fast] = c->sr;
        }
        check("optimized loop retains reference wait clocks", cycles[1], cycles[0]);
        check("optimized loop stops at reference instruction", pc[1], pc[0]);
        check("optimized loop preserves flags", sr[1], sr[0]);
    }
}

static void dma_isolation(void)
{
    for (unsigned slave = 0; slave < 2; ++slave)
    for (unsigned transfer = 0; transfer < 6; ++transfer)
    for (unsigned to_sound = 0; to_sound < 2; ++to_sound) {
        snprintf(scenario, sizeof scenario, "%s DMA kind %u %s sound RAM",
            slave ? "slave" : "master", transfer, to_sound ? "to" : "from");
        seed(); sh2 *c = cpu(slave), *other = cpu(!slave);
        uint32_t source = to_sound ? RAM : SOUND;
        uint32_t destination = to_sound ? SOUND : RAM;
        for (unsigned i = 0; i < 16; ++i) bus_w8(&s, source + i, (uint8_t)(0x31 + i));
        s.cur = c; c->cycles = 1234; other->cycles = 5678;
        /* A DMA triggered during a CPU instruction inherits its context,
         * but none of the DMA's sound accesses are that CPU's instructions. */
        s.cpu_bus_active = 1;
        unsigned bytes = transfer < 4 ? 1u << transfer : 4;
        if (transfer == 3) bytes = 16;
        if (transfer < 4) {
            bus_w32(&s, 0xFFFFFF80u, source);
            bus_w32(&s, 0xFFFFFF84u, destination);
            bus_w32(&s, 0xFFFFFF88u, transfer == 3 ? 4 : 1);
            bus_w32(&s, 0xFFFFFFB0u, 1);
            bus_w32(&s, 0xFFFFFF8Cu, 0x5001u | (transfer << 10));
            check("DMAC completes", bus_r32(&s, 0xFFFFFF88u), 0);
        } else if (transfer == 4) {
            bus_w32(&s, 0x25FE0000u, source);
            bus_w32(&s, 0x25FE0004u, destination);
            bus_w32(&s, 0x25FE0008u, 4);
            bus_w32(&s, 0x25FE000Cu, 0x102u);
            bus_w32(&s, 0x25FE0014u, 7);
            bus_w32(&s, 0x25FE0010u, 0x101u);
            check("SCU DMA completes", bus_r32(&s, 0x25FE0010u), 0);
        } else {
            bus_w32(&s, 0x25FE0088u, 0);
            bus_w32(&s, 0x25FE008Cu, 0x31323334u);
            bus_w32(&s, 0x25FE0080u, 0x8000u);
            bus_w32(&s, 0x25FE0084u, (to_sound ? 0x9C000000u : 0x98000000u) |
                    ((to_sound ? destination : source) >> 2));
            /* B-bus writes emit two halfwords, so use a two-byte stride. */
            bus_w32(&s, 0x25FE0084u, to_sound ? 0xC0009001u : 0xC0010001u);
            bus_w32(&s, 0x25FE0084u, 0xF0000000u);
            bus_w32(&s, 0x25FE0080u, 0x18000u);
            scu_dsp_tick(&s, 16);
            check("DSP DMA reaches END", s.scu_dsp.executing, 0);
        }
        check("DMA leaves initiating CPU clocks unchanged", c->cycles, 1234);
        check("DMA leaves other CPU clocks unchanged", other->cycles, 5678);
        check("DMA restores depth", s.dma_bus_depth, 0);
        check("DMA restores enclosing CPU access scope", s.cpu_bus_active, 1);
        s.cpu_bus_active = 0;
        if (transfer == 5 && !to_sound)
            check("DSP reads sound payload", s.scu_dsp.data[0][0], 0x31323334u);
        else for (unsigned i = 0; i < bytes; ++i)
            check("DMA payload byte", bus_r8(&s, destination + i), 0x31 + i);
    }
}

static void counted_command_poll(void)
{
    /* A real 68000 counts down, then acknowledges a sound-RAM command.
     * Without SH-2 wait states the bounded producer poll expires before the
     * sound CPU can acknowledge; with normal bus timing it sees the reply. */
    static const uint16_t sound_program[] = {
        0x203C, 0x0000, 0x4000, /* MOVE.L #16384,D0 */
        0x5380, 0x66FC,         /* SUBQ.L #1,D0; BNE loop */
        0x13FC, 0x0000, 0x0000, 0x0400, /* MOVE.B #0,$400.L */
        0x4E72, 0x2700          /* STOP #$2700 */
    };
    static const uint16_t poll_program[] = {
        0x6010, 0x2008, 0x8904, /* MOV.B @R1,R0; TST R0,R0; BT ready */
        0x4210, 0x8BFA,         /* DT R2; BF loop */
        0xE301, 0xA001, 0x0009, /* timeout: MOV #1,R3; BRA done; NOP */
        0xE302, 0x001B          /* ready: MOV #2,R3; SLEEP */
    };
    for (unsigned fast = 0; fast < 2; ++fast) {
        snprintf(scenario, sizeof scenario, "counted FIFO poll %s", fast ? "optimized" : "reference");
        seed();
        bus_w32(&s, 0x05A00000u, 0x0007FF00u);
        bus_w32(&s, 0x05A00004u, 0x00001000u);
        for (unsigned i = 0; i < sizeof sound_program / sizeof *sound_program; ++i)
            bus_w16(&s, 0x05A01000u + i * 2, sound_program[i]);
        for (unsigned i = 0; i < sizeof poll_program / sizeof *poll_program; ++i)
            bus_w16(&s, CODE + i * 2, poll_program[i]);
        bus_w8(&s, SOUND, 1);
        s.master.r[1] = SOUND; s.master.r[2] = 65536;
        sound_set_on(&s, 1);
        for (unsigned ticks = 0; ticks < 40000 && !s.master.r[3] && !s.master.halted; ++ticks) {
            if (fast) sh2_run(&s.master, 100);
            else {
                s.master.run_target += 100;
                while (s.master.cycles < s.master.run_target && !s.master.halted) sh2_step(&s.master);
            }
            s.clk += 100;
            sound_run(&s, 100);
        }
        sound_sync(&s);
        check("sound CPU executes valid acknowledgement program", s.sound_cpu.halted, 0);
        check("sound CPU clears the command flag", s.sound_ram[0x400], 0);
        check("bounded poll receives acknowledgement instead of timing out", s.master.r[3], 2);
        check("SH-2 did not exhaust its retry count", s.master.r[2] > 0, 1);
        check("CPU bus scope restored after scheduled work", s.cpu_bus_active, 0);
    }
}

static void diagnostic_environment(const char *name, const char *value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static void diagnostic_accesses(void)
{
    /* Diagnostic flags are cached by the interpreter, so this group runs in
     * a separate invocation from the ordinary optimized/reference checks. */
    diagnostic_environment("SATURN_HOT", "1");
    diagnostic_environment("SATURN_JSRAT", "0x06004000");
    diagnostic_environment("SATURN_IRQLOG", "1");
    diagnostic_environment("SATURN_TRACEPC", "0x25A00400,0x25A00402");
    for (unsigned slave = 0; slave < 2; ++slave)
    for (unsigned scheduled = 0; scheduled < 2; ++scheduled) {
        snprintf(scenario, sizeof scenario, "diagnostic snapshot %s %s",
                 slave ? "slave" : "master", scheduled ? "scheduled" : "step");
        seed();
        sh2 *c = cpu(slave);
        s.ring_trig_pc = CODE; s.ring_any = 1; s.snap_addr = SOUND;
        for (unsigned i = 0; i < 128; ++i) bus_w16(&s, SOUND + i * 2, (uint16_t)(0x8100u + i));
        if (scheduled) sh2_run(c, 1); else sh2_step(c);
        check("snapshot is captured", s.snap_taken, 1);
        check("snapshot does not charge the NOP", c->cycles, 1);
        check("snapshot retains first word", s.snap[0], 0x8100);
        check("snapshot retains final word", s.snap[127], 0x817F);
        check("snapshot restores CPU scope", s.cpu_bus_active, 0);
        /* A following actual sound read must still charge after the peek. */
        bus_w16(&s, CODE + 2, 0x6011); c->r[1] = SOUND;
        if (scheduled) sh2_run(c, 1); else sh2_step(c);
        check("following guest MOV remains timed", c->cycles, 41);
        check("following guest MOV retains data", c->r[0], 0xFFFF8100u);

        snprintf(scenario, sizeof scenario, "diagnostic JSR pointer %s %s",
                 slave ? "slave" : "master", scheduled ? "scheduled" : "step");
        seed(); c = cpu(slave);
        bus_w16(&s, CODE, 0x410B); /* JSR @R1, NOP delay slot */
        c->r[1] = CODE + 8; c->r[3] = SOUND + 4;
        bus_w32(&s, SOUND, 0xAABBCCDDu);
        if (scheduled) sh2_run(c, 1); else sh2_step(c);
        check("pointer trace does not charge branch", c->cycles, 2);
        check("traced JSR reaches its target", c->pc, CODE + 8);
        check("traced JSR retains return address", c->pr, CODE + 4);
        check("pointer trace restores CPU scope", s.cpu_bus_active, 0);
    }
    for (unsigned scheduled = 0; scheduled < 2; ++scheduled) {
        snprintf(scenario, sizeof scenario, "diagnostic opcode trace %s", scheduled ? "scheduled" : "step");
        seed();
        s.master.pc = SOUND | 0x20000000u;
        /* A read watch makes ifetch use the bus rather than its plain-memory
         * host pointer. The textual trace must reuse that actual fetch. */
        s.rrange_lo = SOUND; s.rrange_hi = SOUND + 1;
        bus_w16(&s, SOUND, 0x0009);
        if (scheduled) sh2_run(&s.master, 1); else sh2_step(&s.master);
        check("traced sound-RAM fetch is charged exactly once", s.master.cycles, 40);
        check("traced sound-RAM NOP advances", s.master.pc, (SOUND | 0x20000000u) + 2);
        check("opcode trace restores CPU scope", s.cpu_bus_active, 0);

        snprintf(scenario, sizeof scenario, "diagnostic interrupt trace %s", scheduled ? "scheduled" : "step");
        seed();
        s.master.vbr = SOUND; s.master.r[15] = SOUND + 0x200;
        s.master.sr = 0;
        bus_w32(&s, SOUND + 0x40 * 4, CODE + 8);
        s.scu_reg[0xA0 >> 2] = ~1u; s.scu_ipend = 1;
        if (scheduled) sh2_run(&s.master, 1); else sh2_step(&s.master);
        check("real interrupt stack and vector remain timed once", s.master.cycles, 55);
        check("interrupt handler executes NOP", s.master.pc, CODE + 10);
        check("interrupt keeps vector diagnostic", s.irqall_pc, CODE + 8);
        check("interrupt saves return PC", bus_r32(&s, SOUND + 0x1F8), CODE);
        check("interrupt trace restores CPU scope", s.cpu_bus_active, 0);
    }
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--diagnostics") == 0) {
        diagnostic_accesses();
        printf("sound bus diagnostics: %u checks, %u failures\n", checks, failures);
        return failures != 0;
    }
    instruction_accesses();
    scheduler_carry();
    folded_sound_polls();
    dma_isolation();
    counted_command_poll();
    printf("sound bus timing: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
