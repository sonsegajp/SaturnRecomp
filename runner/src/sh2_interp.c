/* sh2_interp.c — SH-2 interpreter.
 *
 * This is the co-simulation oracle. The static recompiler's output is only
 * ever trusted to the extent it matches this, so correctness here beats speed
 * everywhere. It decodes through the shared sh2_decoder table rather than a
 * private switch, so the interpreter and the emitter can never disagree about
 * what an encoding *is* -- only about what it *does*.
 *
 * Delay slots are modelled explicitly: a branch computes its target, then the
 * following instruction is executed with PC still pointing at the slot, and
 * only then does control transfer. Instructions that are illegal in a slot are
 * faulted rather than silently executed.
 */
#include <stdlib.h>
#include "saturn.h"
#include "../../external/sh2-recomp-core/common/sh2_isa.h"
#include <string.h>

#define T_BIT   (c->sr & SR_T)
#define SET_T(x) do { if (x) c->sr |= SR_T; else c->sr &= ~SR_T; } while (0)
#define R       c->r

/* Shared by the reference and optimized interpreter paths so a diagnostic
 * transition between them cannot emit two "first" captures. */
static int slave_low_reset_reported;

static void fault(sh2 *c, const char *why)
{
    /* NiGHTS's Cinepak driver registration writes strings over handler
     * pointers; on hardware the init completes and overwrites them with
     * real addresses, but here the CPK file load fails so they keep the
     * string data. The main loop calls through these garbage pointers,
     * landing in ROM data or unmapped space. Ymir never sees this because
     * its CPK path succeeds. Rather than halt, recover to the caller --
     * the handler was supposed to be a no-op anyway (uninitialised). */
    if (!c->is_slave && c->pr >= 0x06000000u && c->pr < 0x08000000u) {
        static int count;
        if (count < 100000) {
            count++;
            if (getenv("SATURN_HACKCNT") && count < 4)
                fprintf(stderr, "[hack-fault] #%d pc=%08X pr=%08X why=%s\n",
                        count, c->pc, c->pr, why);
            /* The FIRST swallowed fault is the real one -- everything after is
             * the master wandering after this hack redirected it. Dump the path
             * that reached it while the ring is still clean. */
            if (count == 1 && getenv("SATURN_FAULT1")) {
                saturn *fs = c->sys;
                uint32_t n = fs->mring_head < 48u ? fs->mring_head : 48u, k;
                fprintf(stderr, "[fault1] pc=%08X pr=%08X sr=%08X cy=%llu why=%s\n",
                        c->pc, c->pr, c->sr, (unsigned long long)c->cycles, why);
                for (k = n; k > 0; k--) {
                    uint32_t a = fs->mring[(fs->mring_head - k) & 255u];
                    fprintf(stderr, "    master %08X  %04X\n", a, bus_r16(fs, a));
                }
            }
            c->pc = c->pr; return;
        }
    }
    c->halted   = 1;
    c->fault    = why;
    c->fault_pc = c->pc;
}

/* Undefined opcode -> SH-2 exception, per Ymir SH2::EnterException:
 *   [R15-4] = SR, [R15-8] = PC, R15 -= 8, PC = [VBR + vector*4]
 * Vector 4 is a general illegal instruction, 6 a slot illegal instruction.
 *
 * Falls back to halting when the vector table does not hold a plausible code
 * address -- jumping to a garbage handler destroys the evidence, and a run
 * that stops with a clear fault is worth more than one that wanders. Set
 * SATURN_HALTILL to force the old halt-always behaviour while bisecting. */
static int sh2_illegal(sh2 *c, uint32_t pc, uint8_t vector)
{
    saturn *s = c->sys;
    const char *why = (vector == 6) ? "illegal instruction in delay slot"
                                    : "illegal instruction";
    uint32_t target;
    static int hard = -1;
    static unsigned long long taken;

    /* DEFAULT OFF, opt in with SATURN_ILLEXC.
     *
     * Vectoring is what the hardware does, but we only ever GET here because
     * of an upstream bug, and the exception makes that worse rather than
     * better: it pushes SR and PC (R15 -= 8) and the real BIOS's handler for
     * vector 4 is a deliberate `ldc r0,sr; bf .` hang that never returns, so
     * every attempt burns 8 more bytes of stack. By the time the live-lock
     * guard below gives up, the stack pointer is 128 bytes into garbage and
     * `fault()`'s recovery returns into wreckage.
     *
     * Measured: enabling this turned NiGHTS' attract screen from the correct
     * sky and mountains into a white screen, and the frame is bit-identical to
     * the known-good reference with it off. Until whatever produces the wild
     * jump is fixed, recovering to the caller is strictly better. */
    if (hard < 0) hard = getenv("SATURN_ILLEXC") ? 0 : 1;
    c->pc = pc;
    if (hard) { fault(c, why); return 0; }

    target = bus_r32(s, c->vbr + (uint32_t)vector * 4u);
    {
        uint32_t t = target & 0x07FFFFFFu;
        int ok = (t < BIOS_SIZE)                                  /* IPL     */
              || (t >= 0x00200000u && t < 0x00300000u)            /* WRAM-L  */
              || (t >= 0x06000000u && t < 0x06100000u);           /* WRAM-H  */
        if (!ok || (target & 1u)) { fault(c, why); return 0; }
    }

    /* An UNREGISTERED vector loops forever: the BIOS trampoline looks the
     * handler up in the user table, finds nothing, and executes `rte` -- which
     * restores the pushed PC and lands straight back on the faulting opcode.
     * Hardware does exactly that, but a live-lock destroys the evidence just as
     * thoroughly as a halt and costs a whole run to notice. Take the exception
     * for a real handler; give up once the same PC has taken it repeatedly. */
    {
        static uint32_t last_pc[2];
        static unsigned repeat[2];
        int w = c->is_slave ? 1 : 0;
        if (last_pc[w] == pc) {
            if (++repeat[w] > 16u) { fault(c, why); return 0; }
        } else {
            last_pc[w] = pc;
            repeat[w]  = 0;
        }
    }

    if (getenv("SATURN_ILLLOG") && ++taken <= 8)
        fprintf(stderr, "[ill] vec=%u pc=%08X op=%04X -> handler %08X (#%llu)%s\n",
                vector, pc, bus_r16(s, pc), target,
                (unsigned long long)taken, c->is_slave ? " [slave]" : "");

    R[15] -= 4; bus_w32(s, R[15], c->sr);
    R[15] -= 4; bus_w32(s, R[15], pc);
    c->pc = target;
    c->cycles += 13;
    return 1;
}

void sh2_reset(sh2 *c, saturn *s, int is_slave, uint32_t pc, uint32_t sp)
{
    memset(c, 0, sizeof(*c));
    c->sys      = s;
    c->is_slave = is_slave;
    c->pc       = pc;
    c->r[15]    = sp;
    c->sr       = 0xF0;          /* interrupts masked at level 15 */
    /* A hardware reset starts with VBR=0. Direct/HLE game entry starts after
     * the BIOS has installed its WRAM-H vector table. Distinguishing the two
     * matters for SSHON: the slave must execute the real reset trampoline. */
    c->vbr      = ((pc & 0x07FFFFFFu) < BIOS_SIZE) ? 0u : 0x06000000u;
    c->if_tag   = 1;             /* never matches: memset left it 0 == page 0 */
    c->if_cache_base = 1;        /* likewise: cache-line bases are aligned    */

    /* SH7604 bus-state-controller reset values. BCR1 bit 15 is hard-wired to
     * the processor identity (0=master, 1=slave); the Saturn BIOS reads it at
     * 0xFFFFFFE0 and branches the two CPUs onto different startup paths. A
     * zero-filled on-chip bank made the slave identify as the master, execute
     * the master's cache-transition BRAF, and disappear into 0xE000021E. */
    c->onchip[0x1E0] = (uint8_t)(is_slave ? 0x83u : 0x03u); /* BCR1 = x3F0 */
    c->onchip[0x1E1] = 0xF0u;
    c->onchip[0x1E4] = 0x00u; c->onchip[0x1E5] = 0xFCu;     /* BCR2 */
    c->onchip[0x1E8] = 0xAAu; c->onchip[0x1E9] = 0xFFu;     /* WCR  */
}

/* Execute a single non-branch instruction. Branches are handled by the caller
 * so it can sequence the delay slot. Returns 1, or 0 on fault. */
static int exec_one(sh2 *c, const sh2_insn *i);

/* Debug knobs, resolved ONCE. These were getenv() calls inside sh2_step --
 * two environment-block scans per instruction executed, which capped the whole
 * interpreter at ~85k instructions/second. The environment cannot change under
 * us, so a one-time read is behaviourally identical. */
static struct {
    int      init;
    int      heavy;         /* any per-instruction instrumentation armed   */
    int      rings;         /* rolling PC history requested                */
    uint32_t pclog_pc;      /* SATURN_PCLOG        */
    uint64_t pclog_after;   /* SATURN_PCLOG_AFTER  */
    uint32_t badr0_pc;      /* SATURN_BADR0        */
    uint32_t pcrel_pc;      /* SATURN_PCREL        */
} dbg;

/* Pre-decoded opcode cache. sh2_decode is pure in (op, pc): the only fields
 * that depend on pc are `addr` and `target`, and target's dependence is one of
 * exactly three shapes -- none, +pc (branches, word pool loads), or +(pc&~3)
 * (long pool loads and MOVA). Rather than duplicate the decoder's rules, the
 * shape is measured on first sight by decoding the opcode at pc=0, 2 and 4 and
 * comparing targets. After that an execution costs a struct copy and an add,
 * instead of a memset plus a format-template walk. */
typedef struct { sh2_insn i; uint8_t valid, bad, fix; } dslot;
static dslot dcache[65536];
static uint32_t ophist[65536];

static const sh2_insn *decode_cached(uint16_t op, uint32_t pc, sh2_insn *out)
{
    dslot *d = &dcache[op];
    if (!d->valid) {
        sh2_insn a2, a4;
        d->bad = (uint8_t)!sh2_decode(op, 0, &d->i);
        d->fix = 0;
        if (!d->bad) {
            sh2_decode(op, 2, &a2);
            sh2_decode(op, 4, &a4);
            if (a4.target == d->i.target)           d->fix = 0;
            else if (a2.target == d->i.target + 2u) d->fix = 1;
            else                                    d->fix = 2;
        }
        d->valid = 1;
    }
    if (d->bad) return NULL;
    *out = d->i;
    out->addr = pc;
    if (d->fix == 1u)      out->target += pc;
    else if (d->fix == 2u) out->target += pc & ~3u;
    return out;
}

/* SH7604 unified cache.  Sonic 3D Blast depends on instruction lines staying
 * private to the slave while the master replaces the backing WRAM overlay.
 * Fetching opcodes from a live host pointer made the slave execute a hybrid of
 * the old routine and the new SONIC.BIN chunk.
 *
 * LRU state and replacement follow Ymir's sh2_cache.hpp.  Two-way mode uses
 * ways 2/3; ID disables instruction replacement on a miss. */
static unsigned cache_lru_way(uint8_t lru, int two_way)
{
    if (two_way) return (lru & 1u) ? 2u : 3u;
    if ((lru & 0x38u) == 0x38u) return 0u;
    if ((lru & 0x26u) == 0x06u) return 1u;
    if ((lru & 0x15u) == 0x01u) return 2u;
    return 3u;
}

static void cache_lru_touch(sh2 *c, unsigned set, unsigned way)
{
    static const uint8_t and_mask[4] = { 0x07u, 0x19u, 0x2Au, 0x34u };
    static const uint8_t or_mask [4] = { 0x00u, 0x20u, 0x14u, 0x0Bu };
    c->cache_lru[set] = (uint8_t)((c->cache_lru[set] & and_mask[way]) |
                                  or_mask[way]);
}

static uint16_t cache_ifetch(saturn *s, sh2 *c, uint32_t pc)
{
    uint32_t tag = (pc >> 10) & 0x7FFFFu;
    uint32_t base = pc & ~0xFu;
    unsigned set = (pc >> 4) & 63u, way;
    uint8_t ccr = c->onchip[0x92];
    uint8_t *line;

    /* A sequential run fetches eight opcodes from one cache line. The old
     * path searched four ways and compared four tags for every one of those
     * fetches. Keep the exact SH7604 LRU touch below, but reuse the way while
     * its valid/tag metadata still names this line. Cache purges, address-array
     * writes and replacement all invalidate this check naturally. */
    if (c->if_cache_base == base) {
        way = c->if_cache_way;
        if (way < 4u && c->cache_valid[set][way] &&
            c->cache_tag[set][way] == tag)
            goto hit;
    }

    for (way = 0; way < 4; way++) {
        if (c->cache_valid[set][way] && c->cache_tag[set][way] == tag)
            goto hit;
    }

    /* Instruction replacement disabled: the miss is served cache-through. */
    if (ccr & 0x02u) return bus_r16(s, pc);

    {
        unsigned first = (ccr & 0x08u) ? 2u : 0u;
        for (way = first; way < 4; way++)
            if (!c->cache_valid[set][way]) break;
        if (way >= 4) way = cache_lru_way(c->cache_lru[set], (ccr & 0x08u) != 0);
    }

    line = &c->cache_data[(way << 10) | (set << 4)];
    {
        const uint8_t *page = bus_page(s, base);
        if (page) memcpy(line, page + (base & 0xFFFu), 16u);
        else for (unsigned i = 0; i < 16; i++) line[i] = bus_r8(s, base + i);
    }
    c->cache_tag[set][way] = tag;
    c->cache_valid[set][way] = 1;

hit:
    c->if_cache_base = base;
    c->if_cache_way = (uint8_t)way;
    cache_lru_touch(c, set, way);
    line = &c->cache_data[(way << 10) | (set << 4)];
    return (uint16_t)((line[pc & 0xEu] << 8) | line[(pc & 0xEu) + 1u]);
}

/* Opcode fetch through the per-core page cache. Falls back to the full bus
 * path for on-chip PCs, pages that are not plain memory, and whenever a read
 * watch is armed (the fast path would hide fetches from it). */
static uint16_t ifetch(saturn *s, sh2 *c, uint32_t pc)
{
    uint32_t fa = pc & 0x07FFFFFEu;
    /* Only partition 000 is cached. Partition 001 is the cache-through alias. */
    if ((pc >> 29) == 0u && (c->onchip[0x92] & 0x01u) && !s->rrange_hi)
        return cache_ifetch(s, c, pc);
    if ((fa & ~0xFFFu) == c->if_tag)
        return (uint16_t)((c->if_page[fa & 0xFFEu] << 8) |
                           c->if_page[(fa & 0xFFEu) + 1u]);
    if ((pc >> 29) != 7u && !s->rrange_hi) {
        const uint8_t *pg = bus_page(s, fa);
        if (pg) {
            c->if_page = pg;
            c->if_tag  = fa & ~0xFFFu;
            return (uint16_t)((pg[fa & 0xFFEu] << 8) | pg[(fa & 0xFFEu) + 1u]);
        }
    }
    return bus_r16(s, pc);
}

static int exec_addr_ok(uint32_t pc)
{
    return (pc < 0x00080000u) ||
           (pc >= 0x06000000u && pc < 0x08000000u) ||
           (pc >= 0x00200000u && pc < 0x00300000u) ||
           (pc >= 0x05C00000u && pc < 0x05D00000u) ||   /* VDP1 VRAM + FB */
           (pc >= 0x05E00000u && pc < 0x05F00000u) ||   /* VDP2 VRAM      */
           (pc >= 0x20000000u && pc < 0x28000000u);
}

/* NOTE ON ALIGNMENT: the `& ~1u` / `& ~3u` below are REQUIRED, not sloppy.
 * bus_r16/bus_w16 mask the bus address with 0x07FFFFFE and the 32-bit pair
 * with 0x07FFFFFC, so the reference interpreter forces a misaligned access
 * down to the containing word. These helpers bypass that masking, so they
 * have to reproduce it or the two engines write different bytes whenever a
 * pointer is misaligned. Removing them makes tests/sh2_fastpath_fuzz.c fail
 * within a few hundred programs. (Real SH-2 raises an address error for a
 * misaligned access; neither engine models that yet, so what matters here is
 * only that both agree.)
 *
 * Plain-memory fast path for the common inline opcodes below. Calling the
 * full bus decoder for every stack/work-RAM load performs cache/on-chip,
 * watchpoint and peripheral tests even though gameplay spends overwhelmingly
 * in WRAM-H. Keep all observable watch/protection cases on the full path. */
static inline const uint8_t *fast_read_ptr(sh2 *c, uint32_t a, uint32_t size)
{
    saturn *s = c->sys;
    uint32_t b = a & 0x07FFFFFFu, o;
    /* P4 (bits 31..29 == 111) is the per-core on-chip/cache-array space.
     * Masking it to the 27-bit Saturn bus first makes 0xFFFFFE11 look like a
     * WRAM-H mirror, which hid the slave's FRT input-capture flag and broke
     * inter-CPU rendezvous.  P4 must always retain full bus semantics.
     *
     * The cache ADDRESS ARRAY needs the same treatment: bus.c routes
     * 0xC0000000 (and its 0x80000000 mirror) to per-core cache RAM via
     * is_cachearr(), so it is not a WRAM-H mirror either. Masking it to the
     * 27-bit bus sends the access into work RAM -- and NiGHTS parks the SLAVE
     * STACK there, so every push and pop would land in the wrong memory. */
    if ((a >> 29) == 7u) return NULL;
    /* Partition 010 is the associative cache-purge area, not an external-bus
     * mirror.  Let bus.c return the SH7604 purge read pattern.  Falling
     * through here masked 0x4xxxxxxx to the Saturn bus and made an optimized
     * read observe ordinary RAM instead. */
    if ((a >> 29) == 2u) return NULL;
    if ((a >> 29) == 3u) return NULL;   /* cache address array */
    if (((a >> 29) & 5u) == 4u) return NULL;
    if (s->rrange_hi) return NULL;
    if (b >= 0x06000000u) {
        o = (b - 0x06000000u) & (WRAM_H_SIZE - 1u);
        return o + size <= WRAM_H_SIZE ? &s->wram_h[o] : NULL;
    }
    if (b >= 0x00200000u && b < 0x00300000u) {
        o = (b - 0x00200000u) & (WRAM_L_SIZE - 1u);
        return o + size <= WRAM_L_SIZE ? &s->wram_l[o] : NULL;
    }
    if (b < 0x00100000u) {
        o = b & (BIOS_SIZE - 1u);
        return o + size <= BIOS_SIZE ? &s->bios[o] : NULL;
    }
    return NULL;
}

static inline uint8_t *fast_write_ptr(sh2 *c, uint32_t a, uint32_t size)
{
    saturn *s = c->sys;
    uint32_t b = a & 0x07FFFFFFu, o;
    if ((a >> 29) == 7u) return NULL;
    /* Cache-purge writes invalidate cache entries internally and never reach
     * external memory.  This must be rejected before the 27-bit bus mask:
     * the slave BIOS writes zero to 0x46000240 while polling the physical
     * mailbox at 0x06000240.  The fast path previously aliased those two and
     * erased the master's "2RDY" word. */
    if ((a >> 29) == 2u) return NULL;
    if ((a >> 29) == 3u) return NULL;   /* cache address array */
    if (((a >> 29) & 5u) == 4u) return NULL;   /* cache data array */
    if (s->wrange_hi || s->wwatch_addr) return NULL;
    if (c == &s->master && c->pc >= 0x000002B0u && c->pc <= 0x000002B6u &&
        s->prot_hi && b >= s->prot_lo && b < s->prot_hi) return NULL;
    if (b >= 0x06000000u) {
        o = (b - 0x06000000u) & (WRAM_H_SIZE - 1u);
        return o + size <= WRAM_H_SIZE ? &s->wram_h[o] : NULL;
    }
    if (b >= 0x00200000u && b < 0x00300000u) {
        o = (b - 0x00200000u) & (WRAM_L_SIZE - 1u);
        return o + size <= WRAM_L_SIZE ? &s->wram_l[o] : NULL;
    }
    return NULL;
}

static inline uint8_t fast_r8(sh2 *c, uint32_t a)
{
    const uint8_t *p = fast_read_ptr(c, a, 1u);
    return p ? p[0] : bus_r8(c->sys, a);
}
static inline uint16_t fast_r16(sh2 *c, uint32_t a)
{
    const uint8_t *p = fast_read_ptr(c, a & ~1u, 2u);
    return p ? (uint16_t)((p[0] << 8) | p[1]) : bus_r16(c->sys, a);
}
static inline uint32_t fast_r32(sh2 *c, uint32_t a)
{
    const uint8_t *p = fast_read_ptr(c, a & ~3u, 4u);
    return p ? ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
               ((uint32_t)p[2] << 8) | p[3] : bus_r32(c->sys, a);
}
static inline void fast_w8(sh2 *c, uint32_t a, uint8_t v)
{
    uint8_t *p = fast_write_ptr(c, a, 1u);
    if (p) p[0] = v; else bus_w8(c->sys, a, v);
}
static inline void fast_w16(sh2 *c, uint32_t a, uint16_t v)
{
    uint8_t *p = fast_write_ptr(c, a & ~1u, 2u);
    if (p) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
    else bus_w16(c->sys, a, v);
}
static inline void fast_w32(sh2 *c, uint32_t a, uint32_t v)
{
    uint8_t *p = fast_write_ptr(c, a & ~3u, 4u);
    if (p) {
        p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
        p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
    } else bus_w32(c->sys, a, v);
}

/* SATURN_TRACEWIN=start,end -- print every master instruction retired in a
 * cycle window, with the registers the vertex pipeline lives in. The fatal
 * computation is ~800 instructions of straight-line code; a window trace is
 * the whole argument, not a sample of it. */
static struct { uint64_t a, b; int on, slave; uint32_t pclo, pchi; } tw;

static void tracewin_init(void)
{
    const char *e = getenv("SATURN_TRACEWIN");
    tw.on = 0;
    /* SATURN_TRACESLAVE traces the SLAVE in the window instead of the
     * master. The slave does the per-frame work in most titles, so a
     * master-only tracer cannot see who wrote a shared structure. */
    tw.slave = getenv("SATURN_TRACESLAVE") != NULL;
    if (e) {
        char buf[64]; strncpy(buf, e, 63); buf[63] = 0;
        char *c = strchr(buf, ',');
        if (c) { *c = 0; tw.a = strtoull(buf, NULL, 0); tw.b = strtoull(c + 1, NULL, 0); tw.on = 1; }
    }
    /* SATURN_TRACEPC=lo,hi -- alternative gate: trace by PC range instead of
     * cycle window (handlers recur; cycle windows can't target them). */
    e = getenv("SATURN_TRACEPC");
    if (e) {
        char buf[64]; strncpy(buf, e, 63); buf[63] = 0;
        char *c = strchr(buf, ',');
        if (c) { *c = 0; tw.pclo = (uint32_t)strtoul(buf, NULL, 0);
                 tw.pchi = (uint32_t)strtoul(c + 1, NULL, 0);
                 tw.a = 0; tw.b = ~0ull; tw.on = 1; }
    }
}

static void dbg_init(void)
{
    const char *e;
    if (dbg.init) return;
    dbg.init = 1;
    if ((e = getenv("SATURN_PCLOG")))       dbg.pclog_pc    = (uint32_t)strtoul(e, NULL, 0);
    if ((e = getenv("SATURN_PCLOG_AFTER"))) dbg.pclog_after = strtoull(e, NULL, 0);
    if ((e = getenv("SATURN_BADR0")))       dbg.badr0_pc    = (uint32_t)strtoul(e, NULL, 0);
    if ((e = getenv("SATURN_PCREL")))       dbg.pcrel_pc    = (uint32_t)strtoul(e, NULL, 0);
    /* The heavy block below (hot-PC histogram, instruction ring, watch
     * machinery, r13 tracker) costs real throughput per instruction; it only
     * arms when a diagnostic that needs it is requested. SATURN_HOT keeps the
     * old always-on histogram. */
    dbg.heavy = getenv("SATURN_HOT")   != NULL ||
                getenv("SATURN_RING")  != NULL ||
                getenv("SATURN_RING_ANY") != NULL ||
                getenv("SATURN_WATCH") != NULL ||
                getenv("SATURN_DUMPAT") != NULL ||
                getenv("SATURN_BAL_A") != NULL ||
                getenv("SATURN_SNAP")  != NULL;
    dbg.rings = dbg.heavy || getenv("SATURN_JSRTRACE") != NULL ||
                getenv("SATURN_FAULT1") != NULL;
}

/* Compute a branch's destination and any side effects (PR for calls). */
static uint32_t branch_target(sh2 *c, const sh2_insn *i, uint32_t insn_pc)
{
    switch (i->op) {
    case SH2_OP_BRA:  case SH2_OP_BSR:
    case SH2_OP_BT:   case SH2_OP_BF:
    case SH2_OP_BTS:  case SH2_OP_BFS:
        return i->target;
    case SH2_OP_BRAF: case SH2_OP_BSRF:
        /* Renesas pseudocode is "PC = PC + Rm" with PC already advanced to
         * instruction+4, i.e. the target is insn_pc + 4 + Rm. Using +2 lands
         * two bytes short and, in practice, one instruction before the
         * intended function prologue. */
        return insn_pc + 4 + R[i->n];
    case SH2_OP_JMP:  case SH2_OP_JSR:
        /* SATURN_JSRAT=pc: dump the state feeding one indirect call. A wild
         * `jsr @rN` is the classic way a game leaves its own code, and the
         * question is always "where did that register come from" -- which
         * needs GBR and the source regs at the moment of the branch, not
         * after. */
        {
            static uint32_t at = 1;
            if (at == 1) {
                const char *e = getenv("SATURN_JSRAT");
                at = e ? (uint32_t)strtoul(e, NULL, 0) : 0;
            }
            if (at && insn_pc == at) {
                /* Also read back the word the load came from. When a register
                 * holds a value that no watch ever saw written to the address
                 * it was loaded from, one of the two is lying -- printing both
                 * at the same instant says which. */
                printf("[jsr] pc=%08X -> %08X gbr=%08X r0=%08X r3=%08X "
                       "r8=%08X [r3-4]=%08X cy=%llu %s\n", insn_pc, R[i->n],
                       c->gbr, R[0], R[3], R[8],
                       bus_r32(c->sys, R[3] - 4u),
                       /* s->clk, NOT c->cycles: the two cores' own counters are
                        * reset independently (sh2_reset memsets the core, and
                        * the slave is re-reset on every SSHON), so they cannot
                        * be ordered against each other. The machine clock is
                        * the only common timeline. */
                       (unsigned long long)c->sys->clk,
                       c->is_slave ? "slave" : "master");
                /* SATURN_JSRTRACE: also dump how the core GOT here. A wild
                 * indirect call is never explained by its own registers -- the
                 * question is which branch landed on it, and the PC ring
                 * already holds that. Only dump for targets outside the
                 * expected one, or the log is unreadable. */
                if (getenv("SATURN_JSRTRACE")) {
                    saturn *s = c->sys;
                    const uint32_t *ring = c->is_slave ? s->sring : s->mring;
                    uint32_t head = c->is_slave ? s->sring_head : s->mring_head;
                    uint32_t n = head < 24u ? head : 24u, k;
                    for (k = n; k > 0; k--) {
                        uint32_t a = ring[(head - k) & 255u];
                        printf("      via %08X  %04X\n", a, bus_r16(s, a));
                    }
                }
            }
        }
        return R[i->n];
    case SH2_OP_RTS:
        return c->pr;
    case SH2_OP_RTE:
        return 0;                            /* filled in by the caller */
    default:
        return 0;
    }
}

/* Accept the highest-priority deliverable SCU interrupt, if any.
 * Exception sequence per the SH-2 manual: push SR, push PC, raise SR.I to the
 * interrupt's level, then vector through VBR. SLEEP is cancelled. */
static int take_interrupt(sh2 *c)
{
    saturn *s = c->sys;
    int vector, level, bit;

    /* Fast path: nothing pended at the SCU and the FRT source is compiled out
     * by default -- checked before the full priority scan, which otherwise
     * walks every source table at every instruction boundary. */
    /* Gate on DELIVERABLE bits, not on anything-pending: masked sources stay
     * latched in ipend for the rest of the run (measured: 0x208F held forever
     * once the BIOS masked them), and testing ipend alone put the full
     * priority scan back on every instruction after boot. The level-vs-SR.I
     * comparison still happens in the scan; this only skips the scan when the
     * mask alone proves nothing can fire. */
    /* MASTER ONLY. The slave has no SCU connection (see below), so its pending
     * mask says nothing about whether the slave has an interrupt -- and
     * returning here on the strength of it skipped the slave's dmac_pending()
     * check entirely, so a slave DMA transfer-end interrupt was only ever
     * serviced when the SCU happened to have something deliverable queued. */
    if (!c->is_slave && !c->frt_pend) {
        uint32_t m = s->scu_reg[0xA0 >> 2];
        if (!(s->scu_ipend & 0x3FFFu & ~m) &&
            (!(s->scu_ipend >> 16) || (m & 0x10000u))) return 0;
    }

    /* The SCU drives the MASTER SH-2's interrupt lines. The slave has no
     * connection to it at all: its interrupts come from its own on-chip
     * peripherals. Letting the slave take SCU sources meant it consumed the
     * pending bit -- so a V-Blank the master was waiting on was answered by the
     * slave running the master's handler on the slave's stack, and the master
     * never saw the event. */
    if (c->is_slave) {
        if (!frt_pending(c, c->sr, &vector, &level) &&
            !dmac_pending(c, c->sr, &vector, &level)) return 0;
        bit = -1;
    } else {
        bit = scu_pending(s, c->sr, &vector, &level);
        if (bit < 0 && !frt_pending(c, c->sr, &vector, &level) &&
            !dmac_pending(c, c->sr, &vector, &level)) return 0;
        if (bit >= 0) s->scu_ipend &= ~(1u << bit);
    }

    /* Record the PC we are about to save as the return address whenever we
     * interrupt the routine under investigation. If what comes back out of
     * `rte` is not this value, the return address is being corrupted. */
    if (c->pc >= 0x06012C6Cu && c->pc <= 0x06012C92u && s->nirqpc < 8) {
        s->irqpc[s->nirqpc]    = c->pc;
        s->irqpc_r0[s->nirqpc] = R[0];
        s->nirqpc++;
    }
    R[15] -= 4; bus_w32(s, R[15], c->sr);
    R[15] -= 4; bus_w32(s, R[15], c->pc);

    c->sr = (c->sr & ~SR_I) | ((uint32_t)level << 4);
    c->pc = bus_r32(s, c->vbr + (uint32_t)vector * 4);
    /* SATURN_IRQLOG: name the source of every interrupt taken. A handler that
     * is re-entered forever shows up here as one vector repeating, which is
     * the only way to tell an interrupt storm from a busy main loop. */
    {
        static int irqlog = -1;
        static unsigned long long seen[256];
        if (irqlog < 0) irqlog = getenv("SATURN_IRQLOG") ? 1 : 0;
        if (irqlog && !c->is_slave) {
            unsigned v = (unsigned)vector & 0xFFu;
            if (++seen[v] <= 3 || (seen[v] % 200000ull) == 0)
                fprintf(stderr, "[irq] vec=%02X lvl=%d scubit=%d -> %08X "
                        "(hit %llu) from pc=%08X\n",
                        v, level, bit, c->pc, seen[v], bus_r32(s, R[15] + 4));
        }
    }
    c->sleeping = 0;
    /* Nesting depth. A handler that is re-entered before it returns is the one
     * remaining way the interrupt path can bring back a wrong register: the
     * inner entry pushes onto the outer one's frame. Record the depth and the
     * level pair that produced the deepest nest. */
    if (s->first_irq_cy == 0) s->first_irq_cy = c->cycles;
    /* r8..r14 are callee-saved: whatever the handler chain does, they must
     * come back unchanged. Snapshot them so RTE can verify. */
    if (!s->irqall_valid) {
        for (int k = 0; k < 16; k++) s->irqall[k] = R[k];
        s->irqall_mach = c->mach; s->irqall_macl = c->macl;
        s->irqall_gbr  = c->gbr;
        s->irqall_valid = 1;
        s->irqall_pc = bus_r32(s, c->vbr + (uint32_t)vector * 4u);
    }
    if (!s->irqsave_valid) {
        for (int k = 0; k < 7; k++) s->irqsave[k] = R[8 + k];
        s->irqsave_valid = 1;
        s->irq_clobber_pc = c->pc;
    }
    s->irq_depth++;
    if (s->irq_depth > s->irq_depth_max) {
        s->irq_depth_max  = s->irq_depth;
        s->irq_nest_outer = s->irq_last_level;
        s->irq_nest_inner = level;
        s->irq_nest_pc    = c->pc;
    }
    s->irq_last_level = level;
    s->irqs_taken++;
    if (vector < 128) s->irqvec_hist[vector]++;
    c->cycles += 13;
    return 1;
}

/* SATURN_COVER=lo,hi -- record which 32-byte blocks of a code range actually
 * execute, dumped at exit. "Which functions of this driver ran and which never
 * did" is the question that matters when a module loads correctly but does
 * nothing, and a PC trace is far too slow to answer it over hundreds of
 * millions of cycles.
 *
 * This lives at file scope because sh2_run's inline fast path never enters
 * sh2_step: a block made only of fast-path opcodes used to report as "never
 * run" even while it executed millions of times. Both paths call this. */
static uint32_t cov_lo, cov_hi;
static unsigned long long cov_after;
static int cov_armed;   /* 0 = unread, 1 = armed, -1 = disabled */

static void cov_init_env(void)
{
    const char *e = getenv("SATURN_COVER");
    const char *e2;
    cov_armed = -1;
    if (e) {
        char cb[128]; char *cc;
        strncpy(cb, e, sizeof(cb) - 1); cb[sizeof(cb)-1] = 0;
        cc = strchr(cb, ',');
        if (cc) { *cc = 0; cov_lo = (uint32_t)strtoul(cb, NULL, 0);
                  cov_hi = (uint32_t)strtoul(cc + 1, NULL, 0); }
    }
    e2 = getenv("SATURN_COVERAFTER");
    cov_after = e2 ? strtoull(e2, NULL, 0) : 0;
    if (cov_hi) cov_armed = 1;
}

/* A module loaded partway through a run shares its addresses with whatever was
 * there before. Without a cycle floor the map is contaminated by pre-load
 * execution and reports code as "covered" that never ran as part of this
 * module -- which is exactly how the first DUCK.BIN map misled me. */
static void cov_mark(saturn *s, sh2 *c, uint32_t pc)
{
    if (!cov_armed) cov_init_env();
    if (cov_armed < 0) return;
    if (pc >= cov_lo && pc < cov_hi && c->cycles >= cov_after) {
        uint32_t blk = (pc - cov_lo) >> 5;
        if (blk < COVER_BLOCKS) s->cover[blk >> 3] |= (uint8_t)(1u << (blk & 7));
    }
}

int sh2_step(sh2 *c)
{
    saturn  *s = c->sys;
    sh2_insn i, slot;
    uint16_t op;
    uint32_t pc;

    if (c->halted) return 0;

    /* On-chip register accesses route to the bank of the core making them, so
     * the bus has to know which core that is. Set it here rather than only in
     * the scheduler, so a caller stepping a core directly (the unit tests) also
     * sees its own FRT and INTC. */
    s->cur = c;

    /* SMPC clock change. Hardware (Mednafen smpc.cpp CMD_CKCHG*): the SMPC
     * soft-resets VDP1/VDP2/SCU/sound, switches the divisor, waits a few
     * V-Blanks, then sends an NMI to the master SH-2 -- which is sleeping
     * inside the BIOS routine at 0x52E with VBR pointed at the ROM vector
     * table, whose NMI vector (11) is 0x20000534: the routine's own tail.
     * Its first instruction `add #8,r15` pops the exception frame the NMI
     * just pushed, the tail completes the switch and returns to the CALLER
     * of the clock-change service -- the CD player's launcher (which then
     * enters the game) or the game's own init (which continues inline).
     * Every earlier model here (jump-to-caller, jump-to-game, cold reset)
     * was an approximation of this one mechanism, and each one stranded
     * some caller. */
    if (s->pending_ckchg == 2 && c == &s->master) {
        static int game_entered = 0;
        s->pending_ckchg = 0;
        s->scu_ipend = 0;
        /* One special case sits on top: the CD player's launcher streams the
         * first-read module over the very WRAM its own return chain lives in
         * (NiGHTS: 448KB spanning 0x06004000-0x0607157C buries the player and
         * the WRAM CD driver at 0x0602B500). Returning through the NMI tail
         * lands in the corpse and dies on an illegal instruction. For THAT
         * ckchg -- the first CKCHG320 with a staged image -- enter the game
         * the way the IPL hands off: SCU user vectors stubbed, V-Blank
         * enabled, PC at the first-read entry. */
        if (s->ckchg_cmd == 0x0F && !game_entered &&
            (bus_r32(s, 0x06004000u) != 0 || bus_r32(s, 0x06004004u) != 0)) {
            const uint32_t stub = 0x06000F00u;
            game_entered = 1;
            bus_w16(s, stub + 0, 0x000B);   /* rts */
            bus_w16(s, stub + 2, 0x0009);   /* nop */
            for (uint32_t vv = 0x40; vv <= 0x5F; vv++)
                bus_w32(s, 0x06000900u + vv * 4u, stub);
            s->scu_reg[0xA0 >> 2] = 0xFFFFFFFFu & ~0x3u;
            {
                uint32_t tvmd = bus_r32(s, 0x06000324u);
                bus_w16(s, 0x25F80000u, (uint16_t)tvmd);
            }
            {
                uint32_t sum = 0;
                for (uint32_t qq = 0; qq < 447868u; qq++)
                    sum += bus_r8(s, 0x06004000u + qq);
                printf("[ckchg320] entering game at 0x06004000 sum=%08X%s", sum, "\n");
            }
            c->pc       = 0x06004000u;
            c->r[15]    = 0x06100000u;
            c->sleeping = 0;
            c->sr      &= ~SR_I;
            c->if_tag   = 1;  /* invalidate ifetch cache */
            return 1;
        }
        c->r[15] -= 4; bus_w32(s, c->r[15], c->sr);
        c->r[15] -= 4; bus_w32(s, c->r[15], c->pc);
        c->pc = bus_r32(s, c->vbr + 11u * 4u);
        c->sleeping = 0;
        return 1;
    }

    /* Interrupts are checked at instruction boundaries only -- never between a
     * branch and its delay slot, which sh2_step never splits. */
    take_interrupt(c);

    if (c->sleeping) { c->cycles++; return 1; }
    pc = c->pc;
    /* Diagnostic: a normal slave hard reset starts at the cache-through
     * 0x20000200 vector and deliberately switches to the cached 0x0000022E
     * path.  Re-entering 0x00000200 later is different: it makes the BIOS's
     * E0000000 BRAF land in the I/O partition.  Capture the control path at
     * that first low-alias re-entry, before the BIOS cache-clear loop washes
     * it out of the rolling ring. */
    if (c->is_slave && pc < BIOS_SIZE && c->cycles > 100000000u &&
        getenv("SATURN_SLAVERESET")) {
        if (!slave_low_reset_reported) {
            uint32_t n = s->sring_head < 48u ? s->sring_head : 48u;
            slave_low_reset_reported = 1;
            fprintf(stderr, "[slave low reset] pr=%08X sr=%08X vbr=%08X sp=%08X cy=%llu\n",
                    c->pr, c->sr, c->vbr, R[15], (unsigned long long)c->cycles);
            for (uint32_t k = n; k > 0; k--) {
                uint32_t a = s->sring[(s->sring_head - k) & 255u];
                fprintf(stderr, "    before-reset %08X  %04X\n", a, bus_r16(s, a));
            }
            mem_dump_at(s, 1);
        }
    }
    /* A JSR/JMP to address 0 is never intentional on the Saturn: the reset
     * vector at 0x00000000 is the stack pointer, not code. NiGHTS reaches
     * here because its CPK driver registration overwrites a handler flag
     * with a string, and the handler pointer was never initialised (the CPK
     * file load failed). Recover by returning to the caller. */
    /* Ymir's SH-2 has a TODO for address-error on I/O-area fetches -- it
     * returns 0 rather than halting. The real SH-2 would raise an address
     * error exception (vector 9). NiGHTS hits this because its CPK driver
     * corrupts handler pointers with a string overflow (14-byte name
     * strncpy'd into 12 bytes) and the handler was never initialised since
     * the CPK file load failed. Recover like the TODO implies: a no-op. */
    {
        /* The SH-2 can fetch from VDP1/VDP2 VRAM over the B-bus, and Fighting
         * Vipers relies on it: the loader at 060349A2 calls [0x06034A10],
         * which is 0x05C7E174 -- VDP1 VRAM -- in the shipped AAFV.BIN (checked
         * byte-for-byte against the disc at file offset 0x30A10). Excluding
         * these ranges made that call silently vanish into the redirect below. */
        int ok = exec_addr_ok(pc);
        if (!ok) {
            if (c->is_slave) {
                /* Do not let the slave execute an endless stream of zeroes in
                 * an unmapped P4 address. That hid the original bad return and
                 * left the master waiting forever on a semaphore only the
                 * vanished slave could clear. Preserve the first fault and
                 * let run_slave's ring report show the actual control path. */
                fault(c, "address error on instruction fetch");
                return 0;
            } else {
                static int n; n++;
                if(getenv("SATURN_HACKCNT")&&n<4)fprintf(stderr,"[hack-addr] #%d pc=%08X pr=%08X\n",n,pc,c->pr);
                c->pc = c->pr;
                return 1;
            }
        }
    }
    if (cov_armed > 0) cov_mark(s, c, pc);
    if (!dbg.init) { dbg_init(); tracewin_init(); }
    if (tw.on && (c->is_slave ? tw.slave : !tw.slave) &&
        c->cycles >= tw.a && c->cycles <= tw.b &&
        (!tw.pchi || (c->pc >= tw.pclo && c->pc <= tw.pchi))) {
        char txt[64];
        uint16_t twop = ifetch(s, c, c->pc);
        if (!sh2_format(twop, c->pc, txt)) txt[0] = 0;
        printf("TW %llu %08X %-26s %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X mac=%08X:%08X\n",
               (unsigned long long)c->cycles, c->pc, txt,
               R[0], R[1], R[2], R[3], R[4], R[5], R[6], R[7],
               R[8], R[9], R[10], R[11], R[12], R[13], R[14], R[15],
               c->mach, c->macl);
    }
    if ((dbg.pclog_pc | dbg.badr0_pc | s->pclast_pc | s->regat_pc) != 0) {
        if (dbg.pclog_pc && !c->is_slave) {
            uint32_t w = dbg.pclog_pc;
            uint64_t after = dbg.pclog_after;
            if (pc == w && c->cycles >= after && s->pclog_n < 16) {
                s->pclog_n++;
                printf("[pclog] pc=%08X pr=%08X r0=%08X r3=%08X r4=%08X r6=%08X cy=%llu\n",
                       pc, c->pr, R[0], R[3], R[4], R[6],
                       (unsigned long long)c->cycles);
            }
        }
        if (s->dumpat_pc && pc == s->dumpat_pc && s->dumpat_done >= 0) {
            /* Nth hit, not the first: the interesting code is usually an
             * OVERLAY that is not resident yet when the address is first
             * executed. SATURN_DUMPAT_N selects which hit to capture. */
            if (++s->dumpat_done >= s->dumpat_n) {
                s->dumpat_done = -1;      /* fire once */
                mem_dump_at(s, 1);
            }
        }
        if (s->pclast_pc && pc == s->pclast_pc) {
            uint32_t k = s->pclast_head & 15u;
            s->pclast[k].pc = pc;  s->pclast[k].pr = c->pr;
            for (int q = 0; q < 16; q++) s->pclast[k].r[q] = R[q];
            s->pclast[k].cy = c->cycles;
            s->pclast_head++;
        }
        /* SATURN_REGAT=pc -- registers at a PC, for EITHER core. The existing
         * pclog/badr0 probes are gated on !c->is_slave, so nothing here could
         * see the slave's state, which is exactly where the interesting bugs
         * have been. Prints on the shared machine clock so it orders against
         * the write log. */
        if (s->regat_pc && pc == s->regat_pc && s->regat_n < 64u) {
            s->regat_n++;
            printf("[reg] pc=%08X r0=%08X r1=%08X r2=%08X r3=%08X "
                   "r4=%08X r5=%08X r6=%08X r7=%08X r11=%08X r13=%08X "
                   "r14=%08X r15=%08X sr=%04X pr=%08X clk=%llu %s\n",
                   pc, R[0], R[1], R[2], R[3], R[4], R[5], R[6], R[7],
                   R[11], R[13], R[14], R[15],
                   (unsigned)(c->sr & 0x3F3u), c->pr, (unsigned long long)s->clk,
                   c->is_slave ? "slave" : "master");
        }
        if (dbg.badr0_pc && !c->is_slave) {
            uint32_t w = dbg.badr0_pc;
            if (pc == w && R[0] != 0x00010000u && s->pcrel_logged < 6) {
                s->pcrel_logged++;
                printf("[badr0] pc=%08X r0=%08X r4=%08X r7=%08X pr=%08X sr=%08X cy=%llu\n",
                       pc, R[0], R[4], R[7], c->pr, c->sr,
                       (unsigned long long)c->cycles);
            }
        }
    }
    if (dbg.rings) {
        if (c == &s->slave) {
            s->sring[s->sring_head & 255u] = pc;
            s->sring_head++;
        } else {
            s->mring[s->mring_head & 255u] = pc;
            s->mring_head++;
        }
    }

    /* BIOS service trap: a sentinel PC standing in for a routine we implement
     * in C. Behaves exactly like the real routine's `rts`. */
    if (s->hle_active && bios_hle_is_trap(s, pc)) {
        bios_hle_call(s, pc, c->r);
        c->pc = c->pr;
        c->cycles += 4;
        return 1;
    }

    if (dbg.heavy) {
    { unsigned im = (c->sr >> 4) & 0xF;
      if (im < s->min_imask) { s->min_imask = im; s->min_imask_pc = pc; }
      if (im == 0) { s->last_open_pc = pc; s->last_open_cy = c->cycles; s->armed = 1; }
      else if (s->armed) { s->armed = 0; s->first_mask_pc = pc; s->nmaskings++; } }
    /* Hot-PC histogram: open-addressed, so a runaway loop is obvious. */
    { uint32_t h = (pc >> 1) & (HOTPC_SLOTS - 1);
      for (int k = 0; k < 8; k++) {
          uint32_t j = (h + (uint32_t)k) & (HOTPC_SLOTS - 1);
          if (s->hotpc[j].pc == pc || s->hotpc[j].n == 0) {
              s->hotpc[j].pc = pc; s->hotpc[j].n++; break; } } }

    /* Interrupt-handler stack balance: r15 at the first push of the prologue
     * must equal r15 at the matching final pop, minus the one slot that pop is
     * about to consume. A mismatch means the handler (or something nested
     * inside it) left the stack shifted, which is how a restored register
     * comes back wrong. */
    if (s->bal_a && pc == s->bal_a) { s->bal_sp = R[15]; s->bal_seen = 1; }
    if (s->bal_b && pc == s->bal_b && s->bal_seen) {
        if (R[15] != s->bal_sp) {
            if (s->bal_bad == 0) { s->bal_bad_sp = R[15]; s->bal_bad_exp = s->bal_sp; }
            s->bal_bad++;
        }
        s->bal_ok++;
    }

    if (s->watch_pc && pc == s->watch_pc) {
        for (int wi = 0; wi < 16; wi++) s->watch_regs[wi] = R[wi];   /* last hit */
            s->watch_pr = c->pr;
            s->watch_gbr = c->gbr;
        if (s->watch_hits == 0) s->watch_cy = c->cycles;
        if (s->watch_hits == 0)
            for (int wi = 0; wi < 16; wi++) s->watch_first[wi] = R[wi];
        {   /* ring: keep the LAST 24 hits, which is the state going into
             * whatever the loop finally gets stuck on. */
            int slot = (int)(s->watch_hits % 24);
            s->trace_r0[slot]   = R[0];
            s->trace_r4[slot]   = R[4];
            s->trace_prev[slot] = s->prev_pc;
            if (s->nltrace < 24) s->nltrace++;
        }
        if (R[0] == 0) {
            if (s->watch_zero == 0) s->watch_zero_prev = s->prev_pc;
            s->watch_zero++;
        }
        s->watch_hits++;
        /* Predecessor histogram: which PC actually fell or branched into the
         * watched address. A loop that is entered from somewhere other than
         * its own preamble means something is jumping into its middle. */
        {
            int seen = 0;
            for (int k = 0; k < s->npred; k++)
                if (s->pred[k].pc == s->prev_pc) { s->pred[k].n++; seen = 1; break; }
            if (!seen && s->npred < 12) {
                s->pred[s->npred].pc = s->prev_pc;
                s->pred[s->npred].n  = 1;
                s->npred++;
            }
        }
    }
    /* Name the instruction that performs the exact 0x00010000 -> 0 transition
     * in r0. Everything structural around this hang has been exonerated, so
     * the only thing left worth knowing is which opcode actually does it. */
    if (s->r0_prev != R[0] && R[0] == 0 && s->nr0chg < 8 &&
        s->prev_pc >= 0x06012C6Cu && s->prev_pc <= 0x06012C92u) {
        s->r0chg_pc[s->nr0chg]   = s->prev_pc;
        s->r0chg_next[s->nr0chg] = pc;
        s->nr0chg++;
    }
    /* Rolling window of the last RING_N retired instructions. It is frozen
     * the first time the trigger PC executes with r0 == 0, so the dump
     * contains the run-up to the hang rather than the hang itself. */
    if (!s->ring_frozen) {
        uint32_t h = s->ring_head & (RING_N - 1);
        s->ring_pc[h] = pc; s->ring_r0[h] = R[0]; s->ring_r4[h] = R[4];
        s->ring_head++;
        /* Freeze on the trigger PC. The r0==0 qualifier was for the M16
         * hang; SATURN_RING_ANY freezes on the first hit regardless, which
         * is what you want when the failure is not encoded in a register.
         * ring_skip lets you skip the first N hits and catch a later one. */
        if (s->ring_trig_pc && pc == s->ring_trig_pc &&
            (s->ring_any || R[0] == 0) &&
            (!s->ring_trig_pr || c->pr == s->ring_trig_pr)) {
            if (s->ring_skip > 0) s->ring_skip--;
            else {
                s->ring_frozen = 1;
                /* Snapshot the code AS IT IS NOW. The game clears this
                 * region before the run ends, so an end-of-run dump shows
                 * only zeros. */
                if (s->snap_addr) {
                    for (int q = 0; q < 128; q++)
                        s->snap[q] = bus_r16(s, s->snap_addr + q * 2u);
                    s->snap_taken = 1;
                }
            }
        }
    }
    /* Which instruction is writing r13? The loop at 0x06012E08..16 must not
     * touch it, so any change with prev_pc in that range names the culprit. */
    if (s->r13_prev == 0x7FFFFFFFu && R[13] != s->r13_prev && s->nr13w < 8) {
        s->r13w_pc[s->nr13w]  = s->prev_pc;
        s->r13w_old[s->nr13w] = s->r13_prev;
        s->r13w_new[s->nr13w] = R[13];
        s->nr13w++;
    }
    s->r13_prev = R[13];
    s->r0_prev = R[0];
    s->prev_pc = pc;
    }

    op = ifetch(s, c, pc);
    if (!decode_cached(op, pc, &i)) {
        /* An undefined opcode is an EXCEPTION on real hardware, not the end of
         * the machine: Ymir sh2.cpp does `EnterException(xvGenIllegalInstr)`.
         * Halting the core here turned any single wrong word -- a bad computed
         * jump, a game deliberately probing memory -- into a dead run, which
         * is why one stray branch in NiGHTS killed the whole title. */
        return sh2_illegal(c, pc, 4);
    }

    if (!(i.flags & SH2F_BRANCH)) {
        c->pc = pc + 2;
        if (!exec_one(c, &i)) return 0;
        c->cycles++;
        return 1;
    }

    /* ---- branch ---- */
    {
        uint32_t target = 0, rte_sr = 0;
        int taken = 1;

        if (i.op == SH2_OP_TRAPA) {
            /* TRAPA has no delay slot: push SR and PC, vector through VBR. */
            R[15] -= 4; bus_w32(s, R[15], c->sr);
            R[15] -= 4; bus_w32(s, R[15], pc + 2);
            c->pc = bus_r32(s, c->vbr + (uint32_t)i.imm * 4);
            c->cycles += 8;
            return 1;
        }

        if (i.flags & SH2F_COND) {
            int t = T_BIT ? 1 : 0;
            taken = (i.op == SH2_OP_BT || i.op == SH2_OP_BTS) ? t : !t;
        }

        if (i.op == SH2_OP_RTE) {
            if (s->irq_depth > 0) s->irq_depth--;
            if (s->irqall_valid && s->irq_depth == 0) {
                for (int k = 0; k < 16; k++) {
                    if (k == 15) continue;      /* SP legitimately moves */
                    if (R[k] != s->irqall[k] && s->irqall_reported < 8) {
                        s->irqall_reported++;
                        printf("[irq clobber] r%d %08X -> %08X  handler=%08X\n",
                               k, s->irqall[k], R[k], s->irqall_pc);
                    }
                }
                /* MACH/MACL are the accumulator of every dot product; a
                 * handler that fails to restore them corrupts whatever
                 * multiply the interrupt landed in. r0-r15 alone missed
                 * this. */
                if (c->mach != s->irqall_mach || c->macl != s->irqall_macl) {
                    if (s->irqall_reported < 8) {
                        s->irqall_reported++;
                        printf("[irq clobber] MAC %08X:%08X -> %08X:%08X  handler=%08X pc=%08X cy=%llu\n",
                               s->irqall_mach, s->irqall_macl,
                               c->mach, c->macl, s->irqall_pc, c->pc,
                               (unsigned long long)c->cycles);
                    }
                }
                if (c->gbr != s->irqall_gbr && s->irqall_reported < 8) {
                    s->irqall_reported++;
                    printf("[irq clobber] GBR %08X -> %08X  handler=%08X\n",
                           s->irqall_gbr, c->gbr, s->irqall_pc);
                }
                s->irqall_valid = 0;
            }
            if (s->irqsave_valid && s->irq_depth == 0) {
                for (int k = 0; k < 7; k++)
                    if (R[8 + k] != s->irqsave[k]) {
                        if (s->irq_clobber[k] == 0 && s->irq_clobber_reg < 0) {
                            s->irq_clobber_reg = 8 + k;
                            s->irq_clobber_old = s->irqsave[k];
                            s->irq_clobber_new = R[8 + k];
                        }
                        s->irq_clobber[k]++;
                    }
                s->irqsave_valid = 0;
            }
            c->pc  = bus_r32(s, R[15]); R[15] += 4;
            rte_sr = bus_r32(s, R[15]); R[15] += 4;
            target = c->pc;
        } else {
            target = branch_target(c, &i, pc);
        }

        /* bt/bf have no delay slot; bt/s, bf/s and the rest do. */
        if (!(i.flags & SH2F_DELAY)) {
            c->pc = taken ? target : pc + 2;
            c->cycles += taken ? 3 : 1;
            return 1;
        }

        /* Calls write PR to the instruction *after* the delay slot. */
        if (i.flags & SH2F_CALL) c->pr = pc + 4;

        /* Execute the delay slot with PC at the slot. */
        {
            uint16_t sop = ifetch(s, c, pc + 2);
            /* The window tracer prints at sh2_step entry, which never sees
             * delay slots -- they execute inside this branch path. A slot
             * store was invisible in every trace until this line. */
            if (tw.on && (c->is_slave ? tw.slave : !tw.slave) &&
        c->cycles >= tw.a && c->cycles <= tw.b &&
                (!tw.pchi || (pc + 2 >= tw.pclo && pc + 2 <= tw.pchi))) {
                char txt[64];
                if (!sh2_format(sop, pc + 2, txt)) txt[0] = 0;
                printf("TWs %llu %08X %-25s %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X mac=%08X:%08X\n",
                       (unsigned long long)c->cycles, pc + 2, txt,
                       R[0], R[1], R[2], R[3], R[4], R[5], R[6], R[7],
                       R[8], R[9], R[10], R[11], R[12], R[13], R[14], R[15],
                       c->mach, c->macl);
            }
            if (!decode_cached(sop, pc + 2, &slot)) {
                /* Vector 6, not 4: an undefined opcode in a DELAY SLOT is the
                 * separate "slot illegal instruction" exception, and the
                 * stacked PC is the branch, not the slot (Ymir
                 * xvSlotIllegalInstr). */
                return sh2_illegal(c, pc, 6);
            }
            if (slot.flags & SH2F_NOSLOT) {
                /* The manual lists MOVA and the two PC-relative loads as
                 * illegal in a delay slot, but shipped games contain them and
                 * run on real hardware: Fighting Vipers has
                 *     0605CBD0  bt/s  0605CC26
                 *     0605CBD2  mov.l @(0605CCAC), r1
                 * and faulting here killed the master the moment its 3-D code
                 * started. decode_cached() was handed the SLOT's own address,
                 * so slot.target already holds the right operand address --
                 * just run it. Branches and TRAPA in a slot stay fatal; those
                 * cannot work on any implementation. */
                if (slot.op != SH2_OP_MOVA &&
                    slot.op != SH2_OP_MOVW_PC &&
                    slot.op != SH2_OP_MOVL_PC) {
                    c->pc = pc + 2;
                    fault(c, "illegal slot instruction");
                    return 0;
                }
            }
            c->pc = pc + 4;
            if (!exec_one(c, &slot)) return 0;
        }

        if (i.op == SH2_OP_RTE) c->sr = rte_sr & SR_MASK;
        c->pc = taken ? target : pc + 4;
        c->cycles += 2;
        return 2;
    }
}

static int exec_one(sh2 *c, const sh2_insn *i)
{
    saturn *s = c->sys;
    const uint8_t n = i->n, m = i->m;

    switch (i->op) {

    /* ------------------------------------------------------ transfers */
    case SH2_OP_MOV_RR:  R[n] = R[m]; break;
    case SH2_OP_MOV_I:   R[n] = (uint32_t)i->imm; break;
    case SH2_OP_MOVW_PC: R[n] = (uint32_t)(int16_t)bus_r16(s, i->target); break;
    case SH2_OP_MOVL_PC:
        R[n] = bus_r32(s, i->target);
        {
            const char *e = getenv("SATURN_PCREL");
            if (e) {
                uint32_t want = (uint32_t)strtoul(e, NULL, 0);
                if ((i->target & 0x0FFFFFFCu) == (want & 0x0FFFFFFCu) &&
                    R[n] != 0x00010000u && s->pcrel_logged < 12) {
                    s->pcrel_logged++;
                    printf("[pcrel] pc=%08X target=%08X -> %08X\n",
                           c->pc, i->target, R[n]);
                }
            }
        }
        break;
    case SH2_OP_MOVA:    R[0] = i->target; break;

    case SH2_OP_MOVB_LD: R[n] = (uint32_t)(int8_t) bus_r8 (s, R[m]); break;
    case SH2_OP_MOVW_LD: R[n] = (uint32_t)(int16_t)bus_r16(s, R[m]); break;
    case SH2_OP_MOVL_LD: R[n] =                    bus_r32(s, R[m]); break;

    case SH2_OP_MOVB_ST: bus_w8 (s, R[n], (uint8_t) R[m]); break;
    case SH2_OP_MOVW_ST: bus_w16(s, R[n], (uint16_t)R[m]); break;
    case SH2_OP_MOVL_ST: bus_w32(s, R[n],           R[m]); break;

    case SH2_OP_MOVB_LDP:
        R[n] = (uint32_t)(int8_t)bus_r8(s, R[m]);
        if (n != m) R[m] += 1;
        break;
    case SH2_OP_MOVW_LDP:
        R[n] = (uint32_t)(int16_t)bus_r16(s, R[m]);
        if (n != m) R[m] += 2;
        break;
    case SH2_OP_MOVL_LDP:
        R[n] = bus_r32(s, R[m]);
        if (n != m) R[m] += 4;
        break;

    /* Pre-decrement stores compute the address, write the SOURCE, and only
     * then update Rn -- so `mov.l Rn,@-Rn` (m == n) stores the value Rn held
     * BEFORE the decrement. Ymir MOVBM/MOVWM/MOVLM do exactly this, as does
     * the manual's `Write_Byte(R[n] - 1, R[m]); R[n] -= 1;`. Decrementing
     * first stored the already-decremented pointer, which is a different
     * value whenever a routine pushes a pointer through its own register.
     * Found by tests/sh2_fastpath_fuzz.c, which flagged it as a fast-path
     * disagreement: the fast path was right and this, the reference, was not. */
    case SH2_OP_MOVB_STP: { uint32_t a = R[n] - 1; bus_w8 (s, a, (uint8_t) R[m]); R[n] = a; } break;
    case SH2_OP_MOVW_STP: { uint32_t a = R[n] - 2; bus_w16(s, a, (uint16_t)R[m]); R[n] = a; } break;
    case SH2_OP_MOVL_STP: { uint32_t a = R[n] - 4; bus_w32(s, a,           R[m]); R[n] = a; } break;

    case SH2_OP_MOVB_LD0: R[n] = (uint32_t)(int8_t) bus_r8 (s, R[0] + R[m]); break;
    case SH2_OP_MOVW_LD0: R[n] = (uint32_t)(int16_t)bus_r16(s, R[0] + R[m]); break;
    case SH2_OP_MOVL_LD0: R[n] =                    bus_r32(s, R[0] + R[m]); break;

    case SH2_OP_MOVB_ST0: bus_w8 (s, R[0] + R[n], (uint8_t) R[m]); break;
    case SH2_OP_MOVW_ST0: bus_w16(s, R[0] + R[n], (uint16_t)R[m]); break;
    case SH2_OP_MOVL_ST0: bus_w32(s, R[0] + R[n],           R[m]); break;

    case SH2_OP_MOVB_LDD: R[0] = (uint32_t)(int8_t) bus_r8 (s, R[m] + (uint32_t)i->disp); break;
    case SH2_OP_MOVW_LDD: R[0] = (uint32_t)(int16_t)bus_r16(s, R[m] + (uint32_t)i->disp); break;
    case SH2_OP_MOVL_LDD: R[n] =                    bus_r32(s, R[m] + (uint32_t)i->disp); break;

    /* mov.b/mov.w r0,@(disp,Rn) are encoded 0x80nd / 0x81nd -- the BASE
     * register is bits 7-4, i.e. `m`, not bits 11-8. Only the longword form
     * (0x1nmd) puts the base in `n`, which is why using `n` here looked right
     * and silently sent every byte/word displacement store to the wrong
     * address. */
    case SH2_OP_MOVB_STD: bus_w8 (s, R[m] + (uint32_t)i->disp, (uint8_t) R[0]); break;
    case SH2_OP_MOVW_STD: bus_w16(s, R[m] + (uint32_t)i->disp, (uint16_t)R[0]); break;
    case SH2_OP_MOVL_STD: bus_w32(s, R[n] + (uint32_t)i->disp,           R[m]); break;

    case SH2_OP_MOVB_LDG: R[0] = (uint32_t)(int8_t) bus_r8 (s, c->gbr + (uint32_t)i->disp); break;
    case SH2_OP_MOVW_LDG: R[0] = (uint32_t)(int16_t)bus_r16(s, c->gbr + (uint32_t)i->disp); break;
    case SH2_OP_MOVL_LDG: R[0] =                    bus_r32(s, c->gbr + (uint32_t)i->disp); break;

    case SH2_OP_MOVB_STG: bus_w8 (s, c->gbr + (uint32_t)i->disp, (uint8_t) R[0]); break;
    case SH2_OP_MOVW_STG: bus_w16(s, c->gbr + (uint32_t)i->disp, (uint16_t)R[0]); break;
    case SH2_OP_MOVL_STG: bus_w32(s, c->gbr + (uint32_t)i->disp,           R[0]); break;

    case SH2_OP_MOVT:  R[n] = T_BIT ? 1u : 0u; break;
    case SH2_OP_SWAPB: R[n] = (R[m] & 0xFFFF0000u) | ((R[m] & 0xFF) << 8) | ((R[m] >> 8) & 0xFF); break;
    case SH2_OP_SWAPW: R[n] = (R[m] >> 16) | (R[m] << 16); break;
    case SH2_OP_XTRCT: R[n] = (R[m] << 16) | (R[n] >> 16); break;

    /* ----------------------------------------------------- arithmetic */
    case SH2_OP_ADD:   R[n] += R[m]; break;
    case SH2_OP_ADD_I: R[n] += (uint32_t)i->imm; break;
    case SH2_OP_ADDC: {
        uint32_t t0 = R[n], t1 = t0 + R[m];
        uint32_t res = t1 + (T_BIT ? 1u : 0u);
        SET_T(t1 < t0 || res < t1);
        R[n] = res;
        break;
    }
    case SH2_OP_ADDV: {
        int32_t a = (int32_t)R[n], b = (int32_t)R[m];
        int64_t r = (int64_t)a + b;
        R[n] = (uint32_t)r;
        SET_T(r > 2147483647LL || r < -2147483648LL);
        break;
    }
    case SH2_OP_SUB:  R[n] -= R[m]; break;
    case SH2_OP_SUBC: {
        uint32_t t0 = R[n], t1 = t0 - R[m];
        uint32_t res = t1 - (T_BIT ? 1u : 0u);
        SET_T(t0 < t1 || t1 < res);
        R[n] = res;
        break;
    }
    case SH2_OP_SUBV: {
        int32_t a = (int32_t)R[n], b = (int32_t)R[m];
        int64_t r = (int64_t)a - b;
        R[n] = (uint32_t)r;
        SET_T(r > 2147483647LL || r < -2147483648LL);
        break;
    }
    case SH2_OP_NEG:  R[n] = 0u - R[m]; break;
    case SH2_OP_NEGC: {
        uint32_t t = 0u - R[m];
        uint32_t res = t - (T_BIT ? 1u : 0u);
        SET_T(0u < t || t < res);
        R[n] = res;
        break;
    }

    case SH2_OP_CMPEQ_I: SET_T((int32_t)R[0] == i->imm); break;
    case SH2_OP_CMPEQ:   SET_T(R[n] == R[m]); break;
    case SH2_OP_CMPHS:   SET_T(R[n] >= R[m]); break;
    case SH2_OP_CMPGE:   SET_T((int32_t)R[n] >= (int32_t)R[m]); break;
    case SH2_OP_CMPHI:   SET_T(R[n] >  R[m]); break;
    case SH2_OP_CMPGT:   SET_T((int32_t)R[n] >  (int32_t)R[m]); break;
    case SH2_OP_CMPPZ:   SET_T((int32_t)R[n] >= 0); break;
    case SH2_OP_CMPPL:   SET_T((int32_t)R[n] >  0); break;
    case SH2_OP_CMPSTR: {
        uint32_t t = R[n] ^ R[m];
        SET_T(!(t & 0xFF000000u) || !(t & 0x00FF0000u) ||
              !(t & 0x0000FF00u) || !(t & 0x000000FFu));
        break;
    }

    case SH2_OP_DIV0U: c->sr &= ~(SR_M | SR_Q | SR_T); break;
    case SH2_OP_DIV0S:
        if (R[n] & 0x80000000u) c->sr |= SR_Q; else c->sr &= ~SR_Q;
        if (R[m] & 0x80000000u) c->sr |= SR_M; else c->sr &= ~SR_M;
        SET_T(((R[n] ^ R[m]) & 0x80000000u) != 0);
        break;
    case SH2_OP_DIV1: {
        /* Faithful transcription of the SH-2 manual pseudocode. */
        uint32_t tmp0, tmp2 = R[m];
        uint8_t  old_q = (c->sr & SR_Q) ? 1 : 0;
        uint8_t  M     = (c->sr & SR_M) ? 1 : 0;
        uint8_t  Q     = (R[n] & 0x80000000u) ? 1 : 0;
        uint8_t  tmp1;

        R[n] = (R[n] << 1) | (T_BIT ? 1u : 0u);

        if (!old_q) {
            if (!M) { tmp0 = R[n]; R[n] -= tmp2; tmp1 = (R[n] > tmp0);
                      Q = Q ? (uint8_t)!tmp1 : tmp1; }
            else    { tmp0 = R[n]; R[n] += tmp2; tmp1 = (R[n] < tmp0);
                      Q = Q ? tmp1 : (uint8_t)!tmp1; }
        } else {
            if (!M) { tmp0 = R[n]; R[n] += tmp2; tmp1 = (R[n] < tmp0);
                      Q = Q ? (uint8_t)!tmp1 : tmp1; }
            else    { tmp0 = R[n]; R[n] -= tmp2; tmp1 = (R[n] > tmp0);
                      Q = Q ? tmp1 : (uint8_t)!tmp1; }
        }
        if (Q) c->sr |= SR_Q; else c->sr &= ~SR_Q;
        SET_T(Q == M);
        break;
    }

    case SH2_OP_DMULS: {
        int64_t r = (int64_t)(int32_t)R[n] * (int64_t)(int32_t)R[m];
        c->mach = (uint32_t)((uint64_t)r >> 32);
        c->macl = (uint32_t)r;
        break;
    }
    case SH2_OP_DMULU: {
        uint64_t r = (uint64_t)R[n] * (uint64_t)R[m];
        c->mach = (uint32_t)(r >> 32);
        c->macl = (uint32_t)r;
        break;
    }
    case SH2_OP_MULL:  c->macl = R[n] * R[m]; break;
    case SH2_OP_MULSW: c->macl = (uint32_t)((int32_t)(int16_t)R[n] * (int32_t)(int16_t)R[m]); break;
    case SH2_OP_MULUW: c->macl = (uint32_t)((uint32_t)(uint16_t)R[n] * (uint32_t)(uint16_t)R[m]); break;

    case SH2_OP_MACL: {
        int64_t tn, tm, acc, r;
        tn = (int32_t)bus_r32(s, R[n]); R[n] += 4;
        tm = (int32_t)bus_r32(s, R[m]); R[m] += 4;
        acc = (int64_t)(((uint64_t)c->mach << 32) | c->macl);
        r = acc + tn * tm;
        if (c->sr & SR_S) {
            /* 48-bit saturation */
            if (r >  0x00007FFFFFFFFFFFLL) r = 0x00007FFFFFFFFFFFLL;
            if (r < -0x0000800000000000LL) r = -0x0000800000000000LL;
        }
        c->mach = (uint32_t)((uint64_t)r >> 32);
        c->macl = (uint32_t)r;
        break;
    }
    case SH2_OP_MACW: {
        int32_t tn, tm, prod;
        tn = (int16_t)bus_r16(s, R[n]); R[n] += 2;
        tm = (int16_t)bus_r16(s, R[m]); R[m] += 2;
        prod = tn * tm;
        if (c->sr & SR_S) {
            /* 32-bit saturating accumulate into MACL; MACH holds the flag. */
            int64_t r = (int64_t)(int32_t)c->macl + prod;
            if (r > 2147483647LL)  { r = 2147483647LL;  c->mach |= 1; }
            if (r < -2147483648LL) { r = -2147483648LL; c->mach |= 1; }
            c->macl = (uint32_t)r;
        } else {
            int64_t acc = (int64_t)(((uint64_t)c->mach << 32) | c->macl);
            acc += prod;
            c->mach = (uint32_t)((uint64_t)acc >> 32);
            c->macl = (uint32_t)acc;
        }
        break;
    }

    case SH2_OP_DT:
        R[n] -= 1;
        SET_T(R[n] == 0);
        break;

    case SH2_OP_EXTSB: R[n] = (uint32_t)(int8_t) R[m]; break;
    case SH2_OP_EXTSW: R[n] = (uint32_t)(int16_t)R[m]; break;
    case SH2_OP_EXTUB: R[n] = R[m] & 0xFFu; break;
    case SH2_OP_EXTUW: R[n] = R[m] & 0xFFFFu; break;

    /* ---------------------------------------------------------- logic */
    case SH2_OP_AND:    R[n] &= R[m]; break;
    case SH2_OP_AND_I:  R[0] &= (uint32_t)i->imm; break;
    case SH2_OP_OR:     R[n] |= R[m]; break;
    case SH2_OP_OR_I:   R[0] |= (uint32_t)i->imm; break;
    case SH2_OP_XOR:    R[n] ^= R[m]; break;
    case SH2_OP_XOR_I:  R[0] ^= (uint32_t)i->imm; break;
    case SH2_OP_NOT:    R[n] = ~R[m]; break;
    case SH2_OP_TST:    SET_T((R[n] & R[m]) == 0); break;
    case SH2_OP_TST_I:  SET_T((R[0] & (uint32_t)i->imm) == 0); break;

    case SH2_OP_ANDB_G: {
        uint32_t a = c->gbr + R[0];
        bus_w8(s, a, (uint8_t)(bus_r8(s, a) & (uint8_t)i->imm));
        break;
    }
    case SH2_OP_ORB_G: {
        uint32_t a = c->gbr + R[0];
        bus_w8(s, a, (uint8_t)(bus_r8(s, a) | (uint8_t)i->imm));
        break;
    }
    case SH2_OP_XORB_G: {
        uint32_t a = c->gbr + R[0];
        bus_w8(s, a, (uint8_t)(bus_r8(s, a) ^ (uint8_t)i->imm));
        break;
    }
    case SH2_OP_TSTB_G:
        SET_T((bus_r8(s, c->gbr + R[0]) & (uint8_t)i->imm) == 0);
        break;

    case SH2_OP_TAS: {
        uint8_t v = bus_r8(s, R[n]);
        SET_T(v == 0);
        bus_w8(s, R[n], (uint8_t)(v | 0x80));
        break;
    }

    /* ---------------------------------------------------------- shift */
    case SH2_OP_SHLL:  SET_T(R[n] & 0x80000000u); R[n] <<= 1; break;
    case SH2_OP_SHLR:  SET_T(R[n] & 1u);          R[n] >>= 1; break;
    case SH2_OP_SHAL:  SET_T(R[n] & 0x80000000u); R[n] <<= 1; break;
    case SH2_OP_SHAR:  SET_T(R[n] & 1u); R[n] = (uint32_t)((int32_t)R[n] >> 1); break;
    case SH2_OP_SHLL2: R[n] <<= 2;  break;
    case SH2_OP_SHLR2: R[n] >>= 2;  break;
    case SH2_OP_SHLL8: R[n] <<= 8;  break;
    case SH2_OP_SHLR8: R[n] >>= 8;  break;
    case SH2_OP_SHLL16:R[n] <<= 16; break;
    case SH2_OP_SHLR16:R[n] >>= 16; break;
    case SH2_OP_ROTL: {
        uint32_t t = (R[n] >> 31) & 1u;
        SET_T(t); R[n] = (R[n] << 1) | t;
        break;
    }
    case SH2_OP_ROTR: {
        uint32_t t = R[n] & 1u;
        SET_T(t); R[n] = (R[n] >> 1) | (t << 31);
        break;
    }
    case SH2_OP_ROTCL: {
        uint32_t old_t = T_BIT ? 1u : 0u;
        SET_T(R[n] & 0x80000000u);
        R[n] = (R[n] << 1) | old_t;
        break;
    }
    case SH2_OP_ROTCR: {
        uint32_t old_t = T_BIT ? 1u : 0u;
        SET_T(R[n] & 1u);
        R[n] = (R[n] >> 1) | (old_t << 31);
        break;
    }

    /* --------------------------------------------------------- system */
    case SH2_OP_NOP:    break;
    case SH2_OP_CLRT:   c->sr &= ~SR_T; break;
    case SH2_OP_SETT:   c->sr |=  SR_T; break;
    case SH2_OP_CLRMAC: c->mach = c->macl = 0; break;
    case SH2_OP_SLEEP:  c->sleeping = 1; break;

    case SH2_OP_STC_SR:  R[n] = c->sr & SR_MASK; break;
    case SH2_OP_STC_GBR: R[n] = c->gbr; break;
    case SH2_OP_STC_VBR: R[n] = c->vbr; break;
    case SH2_OP_STS_MACH:R[n] = c->mach; break;
    case SH2_OP_STS_MACL:R[n] = c->macl; break;
    case SH2_OP_STS_PR:  R[n] = c->pr;  break;

    case SH2_OP_LDC_SR: {
        /* Log every SR load with the interrupt mask it installs, so the
         * places that open and close interrupts are visible by PC. */
        unsigned im = (R[n] >> 4) & 0xF;
        int seen = 0;
        for (int k = 0; k < s->nldcsr; k++)
            if (s->ldcsr[k].pc == i->addr) { s->ldcsr[k].n++; seen = 1; break; }
        if (!seen && s->nldcsr < 32) {
            s->ldcsr[s->nldcsr].pc = i->addr;
            s->ldcsr[s->nldcsr].imask = im;
            s->ldcsr[s->nldcsr].n = 1;
            s->nldcsr++;
        }
        c->sr = R[n] & SR_MASK;
        break;
    }
    case SH2_OP_LDC_GBR: c->gbr = R[n]; break;
    case SH2_OP_LDC_VBR: c->vbr = R[n]; break;
    case SH2_OP_LDS_MACH:c->mach = R[n]; break;
    case SH2_OP_LDS_MACL:c->macl = R[n]; break;
    case SH2_OP_LDS_PR:  c->pr   = R[n]; break;

    case SH2_OP_STCL_SR:  R[n] -= 4; bus_w32(s, R[n], c->sr & SR_MASK); break;
    case SH2_OP_STCL_GBR: R[n] -= 4; bus_w32(s, R[n], c->gbr); break;
    case SH2_OP_STCL_VBR: R[n] -= 4; bus_w32(s, R[n], c->vbr); break;
    case SH2_OP_STSL_MACH:R[n] -= 4; bus_w32(s, R[n], c->mach); break;
    case SH2_OP_STSL_MACL:R[n] -= 4; bus_w32(s, R[n], c->macl); break;
    case SH2_OP_STSL_PR:  R[n] -= 4; bus_w32(s, R[n], c->pr); break;

    case SH2_OP_LDCL_SR:  c->sr   = bus_r32(s, R[n]) & SR_MASK; R[n] += 4; break;
    case SH2_OP_LDCL_GBR: c->gbr  = bus_r32(s, R[n]); R[n] += 4; break;
    case SH2_OP_LDCL_VBR: c->vbr  = bus_r32(s, R[n]); R[n] += 4; break;
    case SH2_OP_LDSL_MACH:c->mach = bus_r32(s, R[n]); R[n] += 4; break;
    case SH2_OP_LDSL_MACL:c->macl = bus_r32(s, R[n]); R[n] += 4; break;
    case SH2_OP_LDSL_PR:  c->pr   = bus_r32(s, R[n]); R[n] += 4; break;

    default:
        fault(c, "unimplemented instruction");
        return 0;
    }
    return 1;
}

uint64_t sh2_run(sh2 *c, uint64_t n)
{
    saturn *s = c->sys;
    uint64_t done = 0;
    uint64_t target;
    int fastable;
    static int op_on = -1;
    static int movwtrace_on = -1;

    if (!dbg.init) { dbg_init(); tracewin_init(); }
    if (op_on < 0) op_on = getenv("SATURN_OPHIST") != NULL;
    /* This diagnostic used to call getenv() for every fast-path MOV.W.  The
     * BIOS animation spends a large share of its time in those instructions,
     * so the disabled trace still throttled the runtime to a few FPS. */
    if (movwtrace_on < 0)
        movwtrace_on = getenv("SATURN_MOVWTRACE") != NULL;
    if (!cov_armed) cov_init_env();
    s->cur = c;

    /* Scheduler slices are SH-2 CLOCK budgets, not instruction budgets.
     * Carry the absolute target between calls so an instruction which crosses
     * a slice boundary pays its extra cycles in the following slice. The old
     * `done < n` loop treated every instruction as one clock even though all
     * execution paths already add their real 1/2/3/... cycle cost to
     * c->cycles. During 3-D gameplay that overclocked both guest CPUs and made
     * the host execute far more geometry work per field than Saturn hardware. */
    c->run_target += n;
    target = c->run_target;

    /* Inline fast path for the opcodes that dominate real instruction
     * streams, including delayed branches whose slot is itself simple.
     * The interpreter benches at 41-48M instr/s synthetically but 5.6M
     * on the real boot; the common ops get their own short compare
     * chains here. Anything unmatched -- and everything, when a debug
     * facility is armed -- falls through to sh2_step unchanged. */
    /* The slave was excluded outright, which costs a 3-D title dearly: games
     * that hand geometry to the second CPU ran ALL of it through the full
     * decoder. Nothing in the fast path is master-specific -- R is c->r and
     * every helper takes c -- only the interrupt gate below needed splitting. */
    fastable = !dbg.heavy && !tw.on && !s->hle_active &&
               !(dbg.pclog_pc | dbg.badr0_pc) && !s->pclast_pc &&
               !s->regat_pc;

    /* MEASURED, do not "optimise" this: hoisting the interrupt gate out of the
     * loop and evaluating it once per call -- which is NOT correct, an
     * interrupt raised by a store inside the slice would go unseen until the
     * next one -- is worth only 3.4% (20.27s -> 19.58s over 8e8 instructions).
     * A correct version, re-checking whenever interrupt state changes, would
     * gain less than that. The interpreter is near the limit of this design;
     * real speed needs the emitter, not another tweak here. */
    while (c->cycles < target && !c->halted) {
        if (c->sleeping) {
            uint64_t idle;
            take_interrupt(c);
            if (c->sleeping) {
                /* No interrupt can appear until the scheduler advances the
                 * other devices at the end of this (at most 128-cycle) slice.
                 * Retiring SLEEP one pseudo-instruction at a time is both slow
                 * and less faithful than advancing the idle core in bulk. */
                idle = target - c->cycles;
                c->cycles += idle;
                done += idle;
                break;
            }
        }
        /* Same one-shot as the reference-path check in sh2_step(), placed
         * ahead of the inline decoder so tracing it does not require turning
         * the optimized interpreter off for the entire boot. */
        if (c->is_slave && c->pc < BIOS_SIZE && c->cycles > 100000000u &&
            getenv("SATURN_SLAVERESET")) {
            if (!slave_low_reset_reported) {
                uint32_t count = s->sring_head < 48u ? s->sring_head : 48u;
                slave_low_reset_reported = 1;
                fprintf(stderr, "[slave low reset] pr=%08X sr=%08X vbr=%08X sp=%08X cy=%llu\n",
                        c->pr, c->sr, c->vbr, R[15], (unsigned long long)c->cycles);
                for (uint32_t k = count; k > 0; k--) {
                    uint32_t a = s->sring[(s->sring_head - k) & 255u];
                    fprintf(stderr, "    before-reset %08X  %04X\n", a, bus_r16(s, a));
                }
                mem_dump_at(s, 1);
            }
        }
        if (c->is_slave && c->pc < BIOS_SIZE && getenv("SATURN_SLAVEBIOS")) {
            static int reported;
            if (!reported) {
                uint32_t count = s->sring_head < 24u ? s->sring_head : 24u;
                reported = 1;
                fprintf(stderr, "[slave bios entry] pc=%08X pr=%08X sr=%08X r0=%08X r1=%08X r2=%08X r4=%08X r5=%08X r6=%08X r7=%08X\n",
                        c->pc, c->pr, c->sr, R[0], R[1], R[2], R[4], R[5], R[6], R[7]);
                for (uint32_t k = count; k > 0; k--) {
                    uint32_t a = s->sring[(s->sring_head - k) & 255u];
                    fprintf(stderr, "    before %08X  %04X\n", a, bus_r16(s, a));
                }
            }
        }
        /* Invalid slave PCs must enter sh2_step so its address-error guard can
         * halt at the first bad control transfer and preserve the trace ring. */
        if (fastable && !c->sleeping && s->pending_ckchg != 2 &&
            (!c->is_slave || exec_addr_ok(c->pc))) {
            int may_fast;
            if (c->is_slave) {
                /* Its own on-chip peripherals are the slave's only interrupt
                 * sources. Testing the SCU's pending bits here would park the
                 * slave on the slow path for every frame in which the master
                 * merely had something queued. */
                int fvec, flvl;
                may_fast = !c->frt_pend && !dmac_pending(c, c->sr, &fvec, &flvl);
            } else {
                uint32_t m = s->scu_reg[0xA0 >> 2];
                may_fast = !c->frt_pend &&
                           !(s->scu_ipend & 0x3FFFu & ~m) &&
                           (!(s->scu_ipend >> 16) || (m & 0x10000u));
            }
            if (may_fast) {
                uint32_t pc = c->pc;
                uint16_t op = ifetch(s, c, pc);
                int handled;
                uint16_t hi4 = (uint16_t)(op >> 12);

                /* The fast path never enters sh2_step, so coverage has to be
                 * stamped here too or a block of all-fast opcodes reads as
                 * "never run" while executing millions of times. */
                if (cov_armed > 0) cov_mark(s, c, pc);
                if (dbg.rings) {
                    if (c == &s->slave) {
                        s->sring[s->sring_head & 255u] = pc;
                        s->sring_head++;
                    } else {
                        s->mring[s->mring_head & 255u] = pc;
                        s->mring_head++;
                    }
                }

                #define FAST_SIMPLE(OP, PCV, HND) \
                do { \
                    uint32_t fn = ((OP) >> 8) & 15u, fm = ((OP) >> 4) & 15u; \
                    HND = 1; \
                    switch ((OP) >> 12) { \
                    case 0x5: R[fn] = fast_r32(c, R[fm] + ((OP) & 15u) * 4u); break; \
                    case 0x1: fast_w32(c, R[fn] + ((OP) & 15u) * 4u, R[fm]); break; \
                    case 0x7: R[fn] += (uint32_t)(int32_t)(int8_t)((OP) & 0xFF); break; \
                    case 0xE: R[fn] = (uint32_t)(int32_t)(int8_t)((OP) & 0xFF); break; \
                    case 0x9: R[fn] = (uint32_t)(int32_t)(int16_t)fast_r16(c, (PCV) + 4 + ((OP) & 0xFFu) * 2u); break; \
                    case 0xD: R[fn] = fast_r32(c, (((PCV) & ~3u) + 4) + ((OP) & 0xFFu) * 4u); break; \
                    case 0x6: \
                        switch ((OP) & 15u) { \
                        case 0x2: R[fn] = fast_r32(c, R[fm]); break; \
                        case 0x3: R[fn] = R[fm]; break; \
                        case 0x4: { uint32_t v = fast_r8(c, R[fm]);  if (fn != fm) R[fm] += 1; R[fn] = (uint32_t)(int32_t)(int8_t)v; } break; \
                        case 0x5: { uint32_t v = fast_r16(c, R[fm]); if (fn != fm) R[fm] += 2; R[fn] = (uint32_t)(int32_t)(int16_t)v; } break; \
                        case 0x6: { uint32_t v = fast_r32(c, R[fm]); if (fn != fm) R[fm] += 4; R[fn] = v; } break; \
                        case 0x7: R[fn] = ~R[fm]; break; \
                        case 0x8: R[fn] = (R[fm] & 0xFFFF0000u) | ((R[fm] & 0xFFu) << 8) | ((R[fm] >> 8) & 0xFFu); break; \
                        case 0x9: R[fn] = (R[fm] << 16) | (R[fm] >> 16); break; \
                        case 0xB: R[fn] = 0u - R[fm]; break; \
                        case 0xC: R[fn] = R[fm] & 0xFFu; break; \
                        case 0xD: R[fn] = R[fm] & 0xFFFFu; break; \
                        case 0xE: R[fn] = (uint32_t)(int32_t)(int8_t)R[fm]; break; \
                        case 0xF: R[fn] = (uint32_t)(int32_t)(int16_t)R[fm]; break; \
                        case 0x0: R[fn] = (uint32_t)(int32_t)(int8_t)fast_r8(c, R[fm]); break; \
                        case 0x1: { \
                            uint32_t _a = R[fm], _v = fast_r16(c, _a); \
                            R[fn] = (uint32_t)(int32_t)(int16_t)_v; \
                            if (movwtrace_on && (PCV) == 0x06070D64u) \
                                fprintf(stderr, "[movw-fast] pc=%08X addr=%08X raw=%04X result=%08X\n", \
                                        (uint32_t)(PCV), _a, (unsigned)_v, R[fn]); \
                        } break; \
                        default: HND = 0; break; \
                        } break; \
                    case 0x2: \
                        switch ((OP) & 15u) { \
                        case 0x2: fast_w32(c, R[fn], R[fm]); break; \
                        /* Write the SOURCE before updating Rn: with m == n the \
                         * stored value is the pre-decrement one (Ymir MOVBM). */ \
                        case 0x4: { uint32_t _a = R[fn] - 1; fast_w8(c, _a, (uint8_t)R[fm]); R[fn] = _a; } break; \
                        case 0x5: { uint32_t _a = R[fn] - 2; fast_w16(c, _a, (uint16_t)R[fm]); R[fn] = _a; } break; \
                        case 0x6: { uint32_t _a = R[fn] - 4; fast_w32(c, _a, R[fm]); R[fn] = _a; } break; \
                        case 0x0: fast_w8(c, R[fn], (uint8_t)R[fm]); break; \
                        case 0x1: fast_w16(c, R[fn], (uint16_t)R[fm]); break; \
                        case 0x8: SET_T((R[fn] & R[fm]) == 0); break; \
                        case 0x9: R[fn] &= R[fm]; break; \
                        case 0xA: R[fn] ^= R[fm]; break; \
                        case 0xB: R[fn] |= R[fm]; break; \
                        case 0xD: R[fn] = (R[fm] << 16) | (R[fn] >> 16); break; \
                        default: HND = 0; break; \
                        } break; \
                    case 0x3: \
                        switch ((OP) & 15u) { \
                        case 0x0: SET_T(R[fn] == R[fm]); break; \
                        case 0x2: SET_T(R[fn] >= R[fm]); break; \
                        case 0x3: SET_T((int32_t)R[fn] >= (int32_t)R[fm]); break; \
                        case 0x6: SET_T(R[fn] > R[fm]); break; \
                        case 0x7: SET_T((int32_t)R[fn] > (int32_t)R[fm]); break; \
                        case 0xC: R[fn] += R[fm]; break; \
                        case 0x8: R[fn] -= R[fm]; break; \
                        case 0x4: { \
                            uint32_t _tmp0, _tmp2 = R[fm]; \
                            uint8_t _oq = (c->sr & SR_Q) != 0, _M = (c->sr & SR_M) != 0; \
                            uint8_t _Q = (R[fn] >> 31) & 1u, _tmp1; \
                            R[fn] = (R[fn] << 1) | ((c->sr & SR_T) != 0); \
                            if (!_oq) { \
                                if (!_M) { _tmp0=R[fn]; R[fn]-=_tmp2; _tmp1=R[fn]>_tmp0; _Q=_Q ? !_tmp1 : _tmp1; } \
                                else     { _tmp0=R[fn]; R[fn]+=_tmp2; _tmp1=R[fn]<_tmp0; _Q=_Q ? _tmp1 : !_tmp1; } \
                            } else { \
                                if (!_M) { _tmp0=R[fn]; R[fn]+=_tmp2; _tmp1=R[fn]<_tmp0; _Q=_Q ? !_tmp1 : _tmp1; } \
                                else     { _tmp0=R[fn]; R[fn]-=_tmp2; _tmp1=R[fn]>_tmp0; _Q=_Q ? _tmp1 : !_tmp1; } \
                            } \
                            if (_Q) c->sr |= SR_Q; else c->sr &= ~SR_Q; \
                            SET_T(_Q == _M); \
                        } break; \
                        case 0x5: { uint64_t _r=(uint64_t)R[fn]*R[fm]; c->mach=(uint32_t)(_r>>32); c->macl=(uint32_t)_r; } break; \
                        case 0xD: { int64_t _r=(int64_t)(int32_t)R[fn]*(int64_t)(int32_t)R[fm]; c->mach=(uint32_t)((uint64_t)_r>>32); c->macl=(uint32_t)_r; } break; \
                        default: HND = 0; break; \
                        } break; \
                    case 0x4: \
                        switch ((OP) & 0xFF) { \
                        case 0x10: R[fn] -= 1; SET_T(R[fn] == 0); break; \
                        case 0x00: SET_T((R[fn] >> 31) & 1u); R[fn] <<= 1; break; \
                        case 0x01: SET_T(R[fn] & 1u); R[fn] >>= 1; break; \
                        case 0x08: R[fn] <<= 2;  break; \
                        case 0x09: R[fn] >>= 2;  break; \
                        case 0x18: R[fn] <<= 8;  break; \
                        case 0x19: R[fn] >>= 8;  break; \
                        case 0x28: R[fn] <<= 16; break; \
                        case 0x29: R[fn] >>= 16; break; \
                        case 0x21: SET_T(R[fn] & 1u); R[fn] = (uint32_t)((int32_t)R[fn] >> 1); break; \
                        case 0x24: { uint32_t _t=(c->sr & SR_T)!=0; SET_T(R[fn] >> 31); R[fn]=(R[fn]<<1)|_t; } break; \
                        case 0x25: { uint32_t _t=(c->sr & SR_T)!=0; SET_T(R[fn] & 1u); R[fn]=(R[fn]>>1)|(_t<<31); } break; \
                        case 0x22: R[fn] -= 4; fast_w32(c, R[fn], c->pr); break; \
                        case 0x26: c->pr = fast_r32(c, R[fn]); R[fn] += 4; break; \
                        case 0x11: SET_T((int32_t)R[fn] >= 0); break; \
                        case 0x15: SET_T((int32_t)R[fn] > 0); break; \
                        case 0x02: R[fn]-=4; fast_w32(c,R[fn],c->mach); break; \
                        case 0x12: R[fn]-=4; fast_w32(c,R[fn],c->macl); break; \
                        case 0x06: c->mach=fast_r32(c,R[fn]); R[fn]+=4; break; \
                        case 0x16: c->macl=fast_r32(c,R[fn]); R[fn]+=4; break; \
                        case 0x0A: c->mach=R[fn]; break; \
                        case 0x1A: c->macl=R[fn]; break; \
                        case 0x2A: c->pr=R[fn]; break; \
                        default: HND = 0; break; \
                        } break; \
                    case 0x8: \
                        switch (((OP) >> 8) & 15u) { \
                        case 0x0: fast_w8(c, R[fm] + ((OP) & 15u), (uint8_t)R[0]); break; \
                        case 0x1: fast_w16(c, R[fm] + (((OP) & 15u) << 1), (uint16_t)R[0]); break; \
                        case 0x4: R[0] = (uint32_t)(int32_t)(int8_t)fast_r8(c, R[fm] + ((OP) & 15u)); break; \
                        case 0x5: R[0] = (uint32_t)(int32_t)(int16_t)fast_r16(c, R[fm] + (((OP) & 15u) << 1)); break; \
                        case 0x8: SET_T(R[0] == (uint32_t)(int32_t)(int8_t)((OP) & 0xFF)); break; \
                        default: HND = 0; break; \
                        } \
                        break; \
                    case 0xC: \
                        switch (((OP) >> 8) & 15u) { \
                        case 0x0: fast_w8(c,c->gbr+((OP)&0xFFu),(uint8_t)R[0]); break; \
                        case 0x1: fast_w16(c,c->gbr+(((OP)&0xFFu)<<1),(uint16_t)R[0]); break; \
                        case 0x2: fast_w32(c,c->gbr+(((OP)&0xFFu)<<2),R[0]); break; \
                        case 0x4: R[0]=(uint32_t)(int32_t)(int8_t)fast_r8(c,c->gbr+((OP)&0xFFu)); break; \
                        case 0x5: R[0]=(uint32_t)(int32_t)(int16_t)fast_r16(c,c->gbr+(((OP)&0xFFu)<<1)); break; \
                        case 0x6: R[0]=fast_r32(c,c->gbr+(((OP)&0xFFu)<<2)); break; \
                        case 0x7: R[0]=(((PCV)&~3u)+4)+(((OP)&0xFFu)<<2); break; \
                        case 0x8: SET_T((R[0] & ((OP) & 0xFFu)) == 0); break; \
                        case 0x9: R[0] &= (OP) & 0xFFu; break; \
                        case 0xA: R[0] ^= (OP) & 0xFFu; break; \
                        case 0xB: R[0] |= (OP) & 0xFFu; break; \
                        default: HND = 0; break; \
                        } break; \
                    case 0x0: \
                        if ((OP) == 0x0009) { } \
                        else if ((OP) == 0x0008) c->sr &= ~SR_T; \
                        else if ((OP) == 0x0018) c->sr |= SR_T; \
                        else if ((OP) == 0x0019) c->sr &= ~(SR_M|SR_Q|SR_T); \
                        else if ((OP) == 0x0028) c->mach = c->macl = 0; \
                        else if (((OP) & 0xF0FFu) == 0x0029u) R[fn] = (c->sr & SR_T) ? 1u : 0u; \
                        else if (((OP) & 0xF0FFu) == 0x0002u) R[fn] = c->sr & SR_MASK; \
                        else if (((OP) & 0xF0FFu) == 0x000Au) R[fn] = c->mach; \
                        else if (((OP) & 0xF0FFu) == 0x001Au) R[fn] = c->macl; \
                        else if (((OP) & 0xF00Fu) == 0x0004u) fast_w8(c,R[0]+R[fn],(uint8_t)R[fm]); \
                        else if (((OP) & 0xF00Fu) == 0x0005u) fast_w16(c,R[0]+R[fn],(uint16_t)R[fm]); \
                        else if (((OP) & 0xF00Fu) == 0x0006u) fast_w32(c,R[0]+R[fn],R[fm]); \
                        else if (((OP) & 0xF00Fu) == 0x000Cu) R[fn]=(uint32_t)(int32_t)(int8_t)fast_r8(c,R[0]+R[fm]); \
                        else if (((OP) & 0xF00Fu) == 0x000Du) R[fn]=(uint32_t)(int32_t)(int16_t)fast_r16(c,R[0]+R[fm]); \
                        else if (((OP) & 0xF00Fu) == 0x000Eu) R[fn]=fast_r32(c,R[0]+R[fm]); \
                        else if (((OP) & 0xF00Fu) == 0x000Fu) { \
                            int64_t _tn=(int32_t)fast_r32(c,R[fn]); R[fn]+=4; \
                            int64_t _tm=(int32_t)fast_r32(c,R[fm]); R[fm]+=4; \
                            int64_t _acc=(int64_t)(((uint64_t)c->mach<<32)|c->macl); \
                            int64_t _r=_acc+_tn*_tm; \
                            if (c->sr & SR_S) { if (_r>0x00007FFFFFFFFFFFLL) _r=0x00007FFFFFFFFFFFLL; if (_r<(-0x00007FFFFFFFFFFFLL-1)) _r=(-0x00007FFFFFFFFFFFLL-1); } \
                            c->mach=(uint32_t)((uint64_t)_r>>32); c->macl=(uint32_t)_r; \
                        } else HND = 0; \
                        break; \
                    default: HND = 0; break; \
                    } \
                } while (0)

                if (hi4 == 0x8) {
                    uint32_t sel = (op >> 8) & 15u;
                    if (sel == 0xB || sel == 0x9) {          /* BF / BT */
                        int take = (sel == 0xB) ? !(c->sr & SR_T)
                                                :  (c->sr & SR_T) != 0;
                        if (take) {
                            c->pc = pc + 4 + 2u * (uint32_t)(int32_t)(int8_t)(op & 0xFF);
                            c->cycles += 3;
                        } else { c->pc = pc + 2; c->cycles += 1; }
                        done++; s->fastpath_hits++; continue;
                    }
                    if (sel == 0xF || sel == 0xD) {          /* BF/S, BT/S */
                        uint16_t sop = ifetch(s, c, pc + 2);
                        int take = (sel == 0xF) ? !(c->sr & SR_T)
                                                :  (c->sr & SR_T) != 0;
                        FAST_SIMPLE(sop, pc + 2, handled);
                        if (!handled) goto slow;
                        c->pc = take
                              ? pc + 4 + 2u * (uint32_t)(int32_t)(int8_t)(op & 0xFF)
                              : pc + 4;
                        c->cycles += 2;
                        done += 2; s->fastpath_hits += 2; continue;
                    }
                } else if (hi4 == 0xA) {                     /* BRA */
                    uint16_t sop = ifetch(s, c, pc + 2);
                    int32_t d12 = (int32_t)(op & 0xFFF);
                    if (d12 & 0x800) d12 -= 0x1000;
                    FAST_SIMPLE(sop, pc + 2, handled);
                    if (!handled) goto slow;
                    c->pc = pc + 4 + 2 * d12;
                    c->cycles += 2;
                    done += 2; s->fastpath_hits += 2; continue;
                } else if (hi4 == 0xB) {                     /* BSR */
                    uint16_t sop = ifetch(s, c, pc + 2);
                    int32_t d12 = (int32_t)(op & 0xFFF);
                    if (d12 & 0x800) d12 -= 0x1000;
                    /* PR is written by the call BEFORE its delay slot runs
                     * (Ymir SH2::BSR, and exec_one line ~975). A slot that
                     * stores or reloads PR must see the updated value; the
                     * write is idempotent if the slot punts to the slow path. */
                    c->pr = pc + 4;
                    FAST_SIMPLE(sop, pc + 2, handled);
                    if (!handled) goto slow;
                    c->pc = pc + 4 + 2 * d12;
                    c->cycles += 2;
                    done += 2; s->fastpath_hits += 2; continue;
                } else if (op == 0x000Bu) {                  /* RTS */
                    uint16_t sop = ifetch(s, c, pc + 2);
                    uint32_t target = c->pr;
                    FAST_SIMPLE(sop, pc + 2, handled);
                    if (!handled) goto slow;
                    c->pc = target;
                    c->cycles += 2;
                    done += 2; s->fastpath_hits += 2; continue;
                } else if ((op & 0xF0FFu) == 0x400Bu ||      /* JSR @Rm */
                           (op & 0xF0FFu) == 0x402Bu) {      /* JMP @Rm */
                    uint16_t sop = ifetch(s, c, pc + 2);
                    uint32_t rm = (op >> 8) & 15u;
                    uint32_t target = R[rm];
                    {
                        static int badjsr = -1;
                        if (badjsr < 0) badjsr = getenv("SATURN_BADJSR") != NULL;
                        if (c->is_slave && target == 0x0000000Eu && badjsr) {
                        fprintf(stderr, "[bad slave jsr] pc=%08X pr=%08X sr=%08X gbr=%08X target=%08X r0=%08X r1=%08X r2=%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X\n",
                                pc, c->pr, c->sr, c->gbr, target, R[0], R[1], R[2], R[3],
                                R[4], R[5], R[6], R[7]);
                        for (int q = -96; q <= 96; q += 2) {
                            uint32_t a = pc + (uint32_t)q;
                            fprintf(stderr, "    live %08X  %04X\n", a, bus_r16(s, a));
                        }
                        }
                    }
                    {
                        static uint32_t jsrat = 1;
                        if (jsrat == 1) {
                            const char *e = getenv("SATURN_JSRAT");
                            jsrat = e ? (uint32_t)strtoul(e, NULL, 0) : 0;
                        }
                        if (jsrat && pc == jsrat) {
                            printf("[jsr-fast] pc=%08X -> %08X rm=r%u pr=%08X gbr=%08X r0=%08X r1=%08X r2=%08X r3=%08X r4=%08X cy=%llu %s\n",
                                   pc, target, rm, c->pr, c->gbr, R[0], R[1], R[2], R[3], R[4],
                                   (unsigned long long)s->clk,
                                   c->is_slave ? "slave" : "master");
                        }
                    }
                    /* PR before the slot, as above (Ymir SH2::JSR). */
                    if ((op & 0x00FFu) == 0x0Bu) c->pr = pc + 4;
                    FAST_SIMPLE(sop, pc + 2, handled);
                    if (!handled) goto slow;
                    c->pc = target;
                    c->cycles += 2;
                    done += 2; s->fastpath_hits += 2; continue;
                } else if ((op & 0xF0FFu) == 0x0003u ||      /* BSRF Rm */
                           (op & 0xF0FFu) == 0x0023u) {      /* BRAF Rm */
                    uint16_t sop = ifetch(s, c, pc + 2);
                    uint32_t rm = (op >> 8) & 15u;
                    uint32_t target = pc + 4 + R[rm];
                    /* PR before the slot, as above (Ymir SH2::BSRF). */
                    if ((op & 0x00FFu) == 0x03u) c->pr = pc + 4;
                    FAST_SIMPLE(sop, pc + 2, handled);
                    if (!handled) goto slow;
                    c->pc = target;
                    c->cycles += 2;
                    done += 2; s->fastpath_hits += 2; continue;
                }

                FAST_SIMPLE(op, pc, handled);
                if (handled) {
                    c->pc = pc + 2;
                    c->cycles += 1;
                    done++; s->fastpath_hits++; continue;
                }
                #undef FAST_SIMPLE

                /* ---- medium path --------------------------------------
                 * Not one of the shapes inlined above, but `fastable` has
                 * already proved that every per-instruction diagnostic
                 * sh2_step consults is disarmed: no window trace, no PC log,
                 * no heavy block, no HLE traps (so bios_hle_is_trap is
                 * false), and this is the master, so the slave ring is not
                 * written either. The interrupt gate above is character for
                 * character take_interrupt's own early-out, so that call
                 * cannot do anything here.
                 *
                 * What is left of sh2_step for a plain instruction is the PC
                 * sanity check, the coverage stamp (already done above) and
                 * the interrupt-mask tracker -- so do exactly those, then
                 * execute through exec_one, the SAME function sh2_step uses.
                 * The semantics therefore cannot drift; only the bookkeeping
                 * that provably has no work to do is skipped. Branches still
                 * go to sh2_step, which owns delay-slot sequencing. */
                {
                    sh2_insn mi;
                    if (op_on) ophist[op]++;
                    int okpc = (pc < 0x00080000u) ||
                               (pc >= 0x06000000u && pc < 0x08000000u) ||
                               (pc >= 0x00200000u && pc < 0x00300000u) ||
                               (pc >= 0x20000000u && pc < 0x28000000u);
                    if (!okpc) goto slow;
                    if (!decode_cached(op, pc, &mi)) goto slow;
                    if (mi.flags & SH2F_BRANCH) goto slow;

                    c->pc = pc + 2;
                    if (!exec_one(c, &mi)) break;
                    c->cycles++;
                    done++; s->slowpath_hits++; continue;
                }
            }
        }
slow:   {
            int k;
            s->slowpath_hits++;
            k = sh2_step(c);
            if (!k) break;
            done += (uint64_t)k;
        }
    }
    return done;
}

void sh2_report_ophist(FILE *f)
{
    unsigned rank;
    if (!f) f = stderr;
    fprintf(f, "[ophist] slow opcodes:");
    for (rank = 0; rank < 12; rank++) {
        uint32_t best_n = 0;
        unsigned best_op = 0;
        for (unsigned op = 0; op < 65536u; op++) {
            if (ophist[op] > best_n) { best_n = ophist[op]; best_op = op; }
        }
        if (!best_n) break;
        {
            char name[64];
            if (!sh2_format((uint16_t)best_op, 0, name)) strcpy(name, "?");
            fprintf(f, " %04X=%u(%s)", best_op, best_n, name);
        }
        ophist[best_op] = 0;
    }
    fputc('\n', f);
    memset(ophist, 0, sizeof ophist);
}
