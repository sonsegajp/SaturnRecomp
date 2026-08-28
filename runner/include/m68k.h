/* m68k.h -- MC68000 sound CPU.
 *
 * The Saturn's sound side is a 68000 running a driver the host uploads into
 * sound RAM, talking to the SCSP. Until now this project had neither: bus.c
 * fakes the driver handshake by clearing the command byte at sound RAM 0x700
 * once per field, which is enough to stop the BIOS wedging but produces no
 * audio at all.
 *
 * The 68000's view of the world:
 *   0x000000-0x07FFFF  sound RAM (512KB, shared with the SH-2s)
 *   0x100000-0x100EE3  SCSP registers
 *   0x100EE4-0x100FFF  SCSP control registers
 * Everything else reads back as zero.
 */
#ifndef M68K_H
#define M68K_H

#include <stdint.h>

/* Status register bits. */
#define M68K_SR_C  0x0001u
#define M68K_SR_V  0x0002u
#define M68K_SR_Z  0x0004u
#define M68K_SR_N  0x0008u
#define M68K_SR_X  0x0010u
#define M68K_SR_I  0x0700u      /* interrupt priority mask */
#define M68K_SR_S  0x2000u      /* supervisor              */
#define M68K_SR_T  0x8000u      /* trace                   */

typedef struct m68k m68k;

struct m68k {
    uint32_t d[8];
    uint32_t a[8];              /* a[7] is the active stack pointer */
    uint32_t pc;
    uint32_t usp, ssp;          /* the inactive one lives here      */
    uint16_t sr;
    int      stopped;           /* STOP executed, waiting for an interrupt */
    int      halted;            /* double fault: nothing more will run     */
    int      in_exception;      /* recursion guard for exception()         */
    int      stepping;          /* 1 while step() is executing             */
    uint64_t cycles;

    /* Pending interrupt level, 0 = none. The SCSP drives this. */
    int      irq_level;
    int      irq_vector;        /* < 0 means autovector                    */

    void    *sys;               /* owning saturn, for the bus callbacks    */
};

/* The 68000's bus. Implemented in m68k_bus.c against the saturn state so the
 * core itself stays free of Saturn specifics. */
uint8_t  m68k_r8 (m68k *m, uint32_t a);
uint16_t m68k_r16(m68k *m, uint32_t a);
uint32_t m68k_r32(m68k *m, uint32_t a);
void     m68k_w8 (m68k *m, uint32_t a, uint8_t v);
void     m68k_w16(m68k *m, uint32_t a, uint16_t v);
void     m68k_w32(m68k *m, uint32_t a, uint32_t v);

/* Reset: load SSP and PC from the vector table at 0. */
void     m68k_reset(m68k *m, void *sys);

/* Run until at least `cycles` have been consumed; returns the number actually
 * consumed (an instruction is never split). */
uint32_t m68k_run(m68k *m, uint32_t cycles);

/* Raise an interrupt. `vector` < 0 requests autovectoring (24 + level). */
void     m68k_set_irq(m68k *m, int level, int vector);

/* Disassemble one instruction for diagnostics; returns its length in bytes. */
int      m68k_disasm(m68k *m, uint32_t pc, char *out, int outsz);

#endif
