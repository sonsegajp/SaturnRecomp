/* SCSP register-to-sample regressions, using Ymir's register layout and
 * fixed-width DSP pipeline semantics. No game data or host audio required. */
#include "saturn.h"
#include <stdio.h>
#include <string.h>

static saturn s;
static int failures;
static void check(const char *name, int got, int expected) {
    if (got != expected) { printf("FAIL %s: %d != %d\n", name, got, expected); failures++; }
}
/* This isolated mixer test keeps interrupts disabled. */
void m68k_set_irq(m68k *m, int level, int vector) { (void)m; (void)level; (void)vector; }
static void reset(void) { memset(&s, 0, sizeof s); scsp_reset(&s); }
static int sample(unsigned control, unsigned level, unsigned envelope) {
    int16_t l, r;
    reset();
    s.sound_ram[0] = 0x40;
    scsp_write(&s, 6, 100);
    scsp_write(&s, 0x0A, envelope);
    scsp_write(&s, 0x0C, level);
    scsp_write(&s, 0x16, 7u << 13);
    scsp_write(&s, 0, 0x1800u | control);
    scsp_render(&s, &l, &r);
    return l;
}
int main(void) {
    reset();
    scsp_write(&s, 0xC00, 0xAB);
    scsp_write(&s, 0xC02, 0x8123);
    check("TEMP sign extension", s.dsp.temp[0], (int32_t)0xFF8123ABu);
    s.dsp.temp[0] = 0x456789;
    check("TEMP live read", scsp_read(&s, 0xC02), 0x4567);
    scsp_write8(&s, 0xC03, 0xAA);
    check("TEMP byte merge preserves DSP output", s.dsp.temp[0], 0x45AA89);
    scsp_write(&s, 0xE00, 0x56);
    scsp_write(&s, 0xE02, 0x1234);
    /* MPRO: X=MEMS[0], Y=COEF[0], ZERO; next instruction stores delayed
     * shifter output to EFREG[0]. Verify CPU initialization reaches execution. */
    s.dsp.pc = 0;
    s.dsp.prog_len = 2;
    s.dsp.coef[0] = 0x1000;
    s.dsp.program[0] = (1ull << 47) | (1ull << 45) | (1ull << 17);
    s.dsp.program[1] = 1ull << 28;
    scsp_dsp_step(&s); scsp_dsp_step(&s);
    check("MEMS through negative DSP coefficient", scsp_read(&s, 0xEC0), (uint16_t)-0x1235);
    scsp_write(&s, 0xE80, 0xF);
    scsp_write(&s, 0xE82, 0x1234);
    check("MIXS CPU bank", s.dsp.mixs[16], 0x1234F);
    check("MIXS low nibble", scsp_read(&s, 0xE80), 15);
    scsp_write(&s, 0xEE0, 0xFEDC);
    check("EXTS write", s.dsp.exts[0], -292);
    check("SDIR bypasses silent TL", sample(0, 0x1FF, 0), 0x4000);
    check("SBCTL lower bits", sample(0x200, 0x100, 0), 0x3FFF);
    check("SBCTL sign bit", sample(0x400, 0x100, 0), -0x4000);
    check("EGBYPASS audible at key-on", sample(0, 0, 0x8000), 16256);
    /* A wet-only slot must be audible through MIXS -> program -> EFREG,
     * independently of DISDL and including the pipeline bank swap. */
    reset();
    s.sound_ram[0] = 0x10;
    scsp_write(&s, 6, 100);
    scsp_write(&s, 0x10, 0x400); /* hold sample position */
    scsp_write(&s, 0x0C, 0x100); /* SDIR */
    scsp_write(&s, 0x14, 7); /* MIXS0, unity send */
    scsp_write(&s, 0x16, 7u << 5); /* EFREG0 full, no direct signal */
    scsp_write(&s, 0, 0x1800);
    s.dsp.coef[0] = 0xFFF;
    s.dsp.program[0] = (1ull<<47)|(1ull<<45)|(0x20ull<<38)|(1ull<<17);
    s.dsp.program[1] = 1ull<<28;
    s.dsp.prog_len = 2;
    int16_t l=0,r=0;
    for(int i=0;i<4;i++)scsp_render(&s,&l,&r);
    check("wet-only effect return reaches left mixer", l, 4095);
    check("wet-only effect return reaches right mixer", r, 4095);
    /* A low-byte write must not replay an old high-byte KYONEX strobe. */
    reset();
    scsp_write(&s, 0, 0x1000);
    { int16_t l,r; scsp_render(&s,&l,&r); }
    scsp_write8(&s, 1, 0x10);
    check("low byte does not rearm KYONEX", s.scsp_kyonex, 0);
    /* Stopped wave playback does not stop the envelope generator. Drivers
     * poll its release level to decide whether the voice can be reused. */
    reset();
    scsp_write(&s, 0xA, 0x001F);
    s.scsp_slot[0].phase=SCSP_ENV_RELEASE;
    s.scsp_slot[0].env=0;
    for(int n=0;n<300;n++){int16_t l,r;scsp_render(&s,&l,&r);}
    check("inactive release reaches silence", scsp_read(&s,0x408)&31,31);
    /* The final MRD needs a following DSP NOP to complete its read. */
    reset();
    s.sound_ram[0]=0x12; s.sound_ram[1]=0x34;
    scsp_write(&s,0x804,0xA000); /* TABLE | MRD */
    scsp_write(&s,0x806,0x0100); /* NOFL */
    s.dsp.pc=0;
    scsp_dsp_step(&s);scsp_dsp_step(&s);
    check("last DSP read drains through trailing NOP",s.dsp.read_value,0x123400);
    printf("SCSP effects: %d failure(s)\n", failures);
    return failures != 0;
}
