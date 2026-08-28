/* sound.c -- ties the sound CPU and the SCSP to the machine clock.
 *
 * The SH-2s run at 28.6364 MHz (or 26.8741 in 320 mode); the sound 68000 runs
 * at 11.2896 MHz, and the SCSP produces one stereo sample per 256 of its own
 * clocks -- 44100 Hz. Rather than carry two more clock domains through the
 * scheduler, both are driven from the master's cycle count with a fixed-point
 * accumulator, which keeps the sample rate exact over time even though any one
 * call rounds.
 *
 * The 68000 stays in reset until the SMPC issues SNDON. That matters: sound
 * RAM holds whatever the host last DMA'd there, and letting the core run
 * before the driver is loaded executes garbage.
 */
#include "saturn.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* SH-2 cycles per SCSP output sample, in 16.16 fixed point.
 * 28,636,360 / 44,100 = 649.35... */
#define SH2_PER_SAMPLE_FP  ((uint64_t)(649.351 * 65536.0))

/* The 68000 gets 11.2896/28.63636 of the master's cycles. Reduced by 40 so
 * the accumulator stays compact without throwing away fractional clocks. */
#define M68K_NUM   282240u
#define M68K_DEN   715909u

void sound_init(saturn *s)
{
    scsp_reset(s);
    s->sound_cpu.sys = s;
    s->sound_on = 0;
    s->m68k_acc = 0;
    s->m68k_target = 0;
    s->scsp_acc = 0;
    s->sound_deferred = 0;
    s->snd_wp = 0;
    s->snd_rp = 0;
}

/* Called by the SMPC on SNDON / SNDOFF. */
void sound_set_on(saturn *s, int on)
{
    /* Finish clocks accrued under the old reset state before changing it. */
    sound_sync(s);
    if (on && !s->sound_on) {
        m68k_reset(&s->sound_cpu, s);
        s->m68k_acc = 0;
        s->m68k_target = 0;
        s->sound_on = 1;
        if (getenv("SATURN_SNDDBG"))
            printf("[snd] SNDON: 68000 released, PC=%06X SP=%06X\n",
                   s->sound_cpu.pc, s->sound_cpu.a[7]);
    } else if (!on) {
        s->sound_on = 0;
    }
}

/* SCSP::Reset(false), used by an SMPC clock change. Ymir resets sound WRAM,
 * the 68000, all slots/registers/DSP state and disables the sound CPU. */
void sound_clock_change_reset(saturn *s)
{
    sound_sync(s);
    memset(s->sound_ram, 0, sizeof s->sound_ram);
    scsp_reset(s);
    m68k_reset(&s->sound_cpu, s);
    s->sound_on = 0;
    s->m68k_acc = 0;
    s->m68k_target = 0;
    s->scsp_acc = 0;
    s->sound_deferred = 0;
    s->snd_wp = s->snd_rp = 0;
    s->cdda_wp = s->cdda_rp = 0;
    s->cdda_ready = 0;
}

/* SATURN_WAV capture. The header carries the total length, which is only
 * known at the end, so patch it on the way out rather than periodically --
 * a header written every second reports a size rounded down to the last whole
 * second, and anything reading it then sees a TRUNCATED file. */
static FILE    *wav;
static uint32_t wav_frames;

static void wav_finish(void)
{
    uint32_t v; uint16_t w;
    if (!wav) return;
    fflush(wav);
    fseek(wav, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, wav);
    v = 36u + wav_frames * 4u;      fwrite(&v, 4, 1, wav);
    fwrite("WAVEfmt ", 1, 8, wav);
    v = 16;         fwrite(&v, 4, 1, wav);
    w = 1;          fwrite(&w, 2, 1, wav);
    w = 2;          fwrite(&w, 2, 1, wav);
    v = 44100;      fwrite(&v, 4, 1, wav);
    v = 44100 * 4;  fwrite(&v, 4, 1, wav);
    w = 4;          fwrite(&w, 2, 1, wav);
    w = 16;         fwrite(&w, 2, 1, wav);
    fwrite("data", 1, 4, wav);
    v = wav_frames * 4u;            fwrite(&v, 4, 1, wav);
    fclose(wav);
    wav = NULL;
}

/* Advance the sound domain by an exact span. The public sound_run() below
 * batches scheduler quanta, while sound_sync() uses this directly before an
 * SH-2 observes shared sound RAM. */
static void sound_run_exact(saturn *s, uint32_t sh2_cycles)
{
    static int disabled = -1, snddbg = -1;
    if (disabled < 0) {
        disabled = getenv("SATURN_NOSOUND") != NULL;
        snddbg   = getenv("SATURN_SNDDBG")  != NULL;
    }
    if (disabled) return;

    /* Run the 68000 for its share of the elapsed time. If it halts -- an
     * unimplemented opcode in a driver we don't cover yet -- the SCSP still
     * renders whatever slots are keyed on, so sound degrades rather than
     * taking the machine down with it. */
    if (s->sound_on && !s->sound_cpu.halted) {
        uint64_t whole;
        s->m68k_acc += (uint64_t)sh2_cycles * M68K_NUM;
        whole = s->m68k_acc / M68K_DEN;
        s->m68k_acc %= M68K_DEN;
        s->m68k_target += whole;
        /* m68k_run completes the last instruction and can exceed its budget.
         * Compare against an absolute target so that overshoot is paid back
         * on later scheduler slices instead of overclocking the sound driver.
         * SATURN_M68KOLD=1 restores the pre-target behaviour (run the raw
         * quotient every call, overshoot accumulates as overclock) for A/B. */
        {
            static int oldpace = -1;
            if (oldpace < 0) oldpace = getenv("SATURN_M68KOLD") != NULL;
            if (oldpace) {
                uint32_t c = (uint32_t)whole;
                if (c) m68k_run(&s->sound_cpu, c);
            } else if (s->sound_cpu.cycles < s->m68k_target) {
                uint64_t due = s->m68k_target - s->sound_cpu.cycles;
                uint32_t c = due > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)due;
                m68k_run(&s->sound_cpu, c);
            }
        }
    }
    if (snddbg && s->sound_on) {
        static uint64_t next;
        if (s->master.cycles >= next) {
            int i, act = 0;
            for (i = 0; i < 32; i++) if (s->scsp_slot[i].active) act++;
            printf("[snd] cy=%llu 68k pc=%06X sr=%04X halted=%d stopped=%d "
                   "slots=%d MVOL=%04X SCIEB=%04X SCIPD=%04X TA=%04X "
                   "cmd700=%02X%s", (unsigned long long)s->master.cycles,
                   s->sound_cpu.pc, s->sound_cpu.sr, s->sound_cpu.halted,
                   s->sound_cpu.stopped, act,
                   s->scsp_reg[0x400 >> 1], s->scsp_reg[0x41E >> 1],
                   s->scsp_reg[0x420 >> 1], s->scsp_reg[0x418 >> 1],
                   s->sound_ram[0x700],
                   "\n");
            {
                uint32_t q, nz = 0;
                for (q = 0; q < 0x10000u; q++) if (s->sound_ram[q]) nz++;
                printf("[snd]   sound RAM: %u/65536 non-zero in the first 64K; "
                       "at PC: %02X %02X %02X %02X%s", nz,
                       s->sound_ram[0x1000], s->sound_ram[0x1001],
                       s->sound_ram[0x1002], s->sound_ram[0x1003],
                       "\n");
            }
            next = s->master.cycles + 40000000ull;
        }
    }

    /* Generate however many output samples that span covers. The SCSP runs
     * whether or not the 68000 does -- a slot the host key-oned directly still
     * has to play. */
    s->scsp_acc += (uint64_t)sh2_cycles << 16;
    while (s->scsp_acc >= SH2_PER_SAMPLE_FP) {
        int16_t l, r;
        uint32_t next;
        s->scsp_acc -= SH2_PER_SAMPLE_FP;
        scsp_render(s, &l, &r);
        if (l || r) {
            static int told;
            s->snd_nonsilent++;
            if (!told && snddbg) {
                told = 1;
                printf("[snd] first audible sample at master cycle %llu (L=%d R=%d)%s",
                       (unsigned long long)s->master.cycles, l, r, "\n");
            }
        }
        /* SATURN_WAV=path: capture the mix to a RIFF/WAVE file. Counting
         * non-silent samples proves the chip is producing SOMETHING; only
         * listening to it, or measuring it, proves it is producing music
         * rather than noise. */
        {
            static int tried;
            if (!tried) {
                const char *p = getenv("SATURN_WAV");
                tried = 1;
                if (p && (wav = fopen(p, "wb"))) {
                    static const char hdr[44] = { 'R','I','F','F' };
                    fwrite(hdr, 1, 44, wav);
                    atexit(wav_finish);
                }
            }
            if (wav) {
                int16_t pair[2]; pair[0] = l; pair[1] = r;
                fwrite(pair, 2, 2, wav);
                wav_frames++;
            }
        }

        next = (s->snd_wp + 1u) % SND_RING;
        if (next != s->snd_rp) {          /* drop rather than overwrite */
            s->snd_buf[s->snd_wp * 2u + 0u] = l;
            s->snd_buf[s->snd_wp * 2u + 1u] = r;
            s->snd_wp = next;
        }
    }
}

/* The two SH-2s need the scheduler's 128-clock interleave for their hardware
 * handshakes. The sound domain's periodic boundary is an SCSP sample roughly
 * every 649 SH-2 clocks, so entering the MC68000 runner every 128 clocks only
 * adds host overhead. Accumulate four quanta and advance them together.
 *
 * bus.c calls sound_sync() before every SH-2 sound-RAM access. A command,
 * upload or acknowledgement therefore observes exactly the sound state due
 * at that machine-clock boundary; batching never moves the sound CPU past an
 * externally visible access. SATURN_SOUNDBATCH=128 restores the old cadence
 * for deterministic A/B testing. */
void sound_run(saturn *s, uint32_t sh2_cycles)
{
    static uint32_t batch;
    if (!batch) {
        const char *e = getenv("SATURN_SOUNDBATCH");
        batch = e ? (uint32_t)strtoul(e, NULL, 0) : 512u;
        if (batch < 128u) batch = 128u;
    }

    s->sound_deferred += sh2_cycles;
    if (s->sound_deferred < batch) return;
    sh2_cycles = s->sound_deferred;
    s->sound_deferred = 0;
    sound_run_exact(s, sh2_cycles);
}

void sound_sync(saturn *s)
{
    uint32_t due = s->sound_deferred;
    if (!due) return;
    s->sound_deferred = 0;
    sound_run_exact(s, due);
}

/* Drain up to `frames` stereo frames into `out`; returns how many were
 * available. The audio callback runs on another thread, so this only ever
 * moves the read pointer -- the writer only ever moves the write pointer. */
uint32_t sound_drain(saturn *s, int16_t *out, uint32_t frames)
{
    uint32_t n = 0;
    while (n < frames && s->snd_rp != s->snd_wp) {
        out[n * 2u + 0u] = s->snd_buf[s->snd_rp * 2u + 0u];
        out[n * 2u + 1u] = s->snd_buf[s->snd_rp * 2u + 1u];
        s->snd_rp = (s->snd_rp + 1u) % SND_RING;
        n++;
    }
    return n;
}
