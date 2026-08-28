/* sh2_isa.h — SH-2 (Hitachi SH7604) instruction set definitions.
 *
 * Shared decoder core for SaturnRecomp. Mirrors the role of
 * segagenesisrecomp's external/m68k-recomp-core/common/m68k_decoder.h.
 *
 * Two consumers, one table:
 *   - sh2_format()  emits capstone-identical text, so the whole 16-bit opcode
 *                   space can be diffed against an oracle (CS_MODE_SH2 |
 *                   CS_MODE_BIG_ENDIAN) and proven 0-wrong.
 *   - sh2_decode()  emits a structured sh2_insn for analysis and codegen.
 *
 * Saturn SH-2 is BIG-ENDIAN. Opcodes are read as (hi << 8) | lo.
 */
#ifndef SH2_ISA_H
#define SH2_ISA_H

#include <stdint.h>

/* ---------------------------------------------------------------- opcodes */

typedef enum {
    SH2_OP_INVALID = 0,

    /* data transfer */
    SH2_OP_MOV_RR,        /* mov    Rm,Rn            */
    SH2_OP_MOV_I,         /* mov    #imm,Rn          */
    SH2_OP_MOVW_PC,       /* mov.w  @(d,PC),Rn       */
    SH2_OP_MOVL_PC,       /* mov.l  @(d,PC),Rn       */
    SH2_OP_MOVB_LD,       /* mov.b  @Rm,Rn           */
    SH2_OP_MOVW_LD,
    SH2_OP_MOVL_LD,
    SH2_OP_MOVB_ST,       /* mov.b  Rm,@Rn           */
    SH2_OP_MOVW_ST,
    SH2_OP_MOVL_ST,
    SH2_OP_MOVB_LDP,      /* mov.b  @Rm+,Rn          */
    SH2_OP_MOVW_LDP,
    SH2_OP_MOVL_LDP,
    SH2_OP_MOVB_STP,      /* mov.b  Rm,@-Rn          */
    SH2_OP_MOVW_STP,
    SH2_OP_MOVL_STP,
    SH2_OP_MOVB_LD0,      /* mov.b  @(R0,Rm),Rn      */
    SH2_OP_MOVW_LD0,
    SH2_OP_MOVL_LD0,
    SH2_OP_MOVB_ST0,      /* mov.b  Rm,@(R0,Rn)      */
    SH2_OP_MOVW_ST0,
    SH2_OP_MOVL_ST0,
    SH2_OP_MOVB_LDD,      /* mov.b  @(d,Rm),R0       */
    SH2_OP_MOVW_LDD,
    SH2_OP_MOVL_LDD,      /* mov.l  @(d,Rm),Rn       */
    SH2_OP_MOVB_STD,      /* mov.b  R0,@(d,Rn)       */
    SH2_OP_MOVW_STD,
    SH2_OP_MOVL_STD,      /* mov.l  Rm,@(d,Rn)       */
    SH2_OP_MOVB_LDG,      /* mov.b  @(d,GBR),R0      */
    SH2_OP_MOVW_LDG,
    SH2_OP_MOVL_LDG,
    SH2_OP_MOVB_STG,      /* mov.b  R0,@(d,GBR)      */
    SH2_OP_MOVW_STG,
    SH2_OP_MOVL_STG,
    SH2_OP_MOVA,          /* mova   @(d,PC),R0       */
    SH2_OP_MOVT,          /* movt   Rn               */
    SH2_OP_SWAPB,
    SH2_OP_SWAPW,
    SH2_OP_XTRCT,

    /* arithmetic */
    SH2_OP_ADD,
    SH2_OP_ADD_I,
    SH2_OP_ADDC,
    SH2_OP_ADDV,
    SH2_OP_CMPEQ_I,
    SH2_OP_CMPEQ,
    SH2_OP_CMPHS,
    SH2_OP_CMPGE,
    SH2_OP_CMPHI,
    SH2_OP_CMPGT,
    SH2_OP_CMPPZ,
    SH2_OP_CMPPL,
    SH2_OP_CMPSTR,
    SH2_OP_DIV1,
    SH2_OP_DIV0S,
    SH2_OP_DIV0U,
    SH2_OP_DMULS,
    SH2_OP_DMULU,
    SH2_OP_DT,
    SH2_OP_EXTSB,
    SH2_OP_EXTSW,
    SH2_OP_EXTUB,
    SH2_OP_EXTUW,
    SH2_OP_MACL,
    SH2_OP_MACW,
    SH2_OP_MULL,
    SH2_OP_MULSW,
    SH2_OP_MULUW,
    SH2_OP_NEG,
    SH2_OP_NEGC,
    SH2_OP_SUB,
    SH2_OP_SUBC,
    SH2_OP_SUBV,

    /* logic */
    SH2_OP_AND,
    SH2_OP_AND_I,
    SH2_OP_ANDB_G,
    SH2_OP_NOT,
    SH2_OP_OR,
    SH2_OP_OR_I,
    SH2_OP_ORB_G,
    SH2_OP_TAS,
    SH2_OP_TST,
    SH2_OP_TST_I,
    SH2_OP_TSTB_G,
    SH2_OP_XOR,
    SH2_OP_XOR_I,
    SH2_OP_XORB_G,

    /* shift */
    SH2_OP_ROTL,
    SH2_OP_ROTR,
    SH2_OP_ROTCL,
    SH2_OP_ROTCR,
    SH2_OP_SHAL,
    SH2_OP_SHAR,
    SH2_OP_SHLL,
    SH2_OP_SHLR,
    SH2_OP_SHLL2,
    SH2_OP_SHLR2,
    SH2_OP_SHLL8,
    SH2_OP_SHLR8,
    SH2_OP_SHLL16,
    SH2_OP_SHLR16,

    /* branch */
    SH2_OP_BF,
    SH2_OP_BFS,
    SH2_OP_BT,
    SH2_OP_BTS,
    SH2_OP_BRA,
    SH2_OP_BRAF,
    SH2_OP_BSR,
    SH2_OP_BSRF,
    SH2_OP_JMP,
    SH2_OP_JSR,
    SH2_OP_RTS,

    /* system */
    SH2_OP_CLRMAC,
    SH2_OP_CLRT,
    SH2_OP_LDC_SR,
    SH2_OP_LDC_GBR,
    SH2_OP_LDC_VBR,
    SH2_OP_LDCL_SR,
    SH2_OP_LDCL_GBR,
    SH2_OP_LDCL_VBR,
    SH2_OP_LDS_MACH,
    SH2_OP_LDS_MACL,
    SH2_OP_LDS_PR,
    SH2_OP_LDSL_MACH,
    SH2_OP_LDSL_MACL,
    SH2_OP_LDSL_PR,
    SH2_OP_NOP,
    SH2_OP_RTE,
    SH2_OP_SETT,
    SH2_OP_SLEEP,
    SH2_OP_STC_SR,
    SH2_OP_STC_GBR,
    SH2_OP_STC_VBR,
    SH2_OP_STCL_SR,
    SH2_OP_STCL_GBR,
    SH2_OP_STCL_VBR,
    SH2_OP_STS_MACH,
    SH2_OP_STS_MACL,
    SH2_OP_STS_PR,
    SH2_OP_STSL_MACH,
    SH2_OP_STSL_MACL,
    SH2_OP_STSL_PR,
    SH2_OP_TRAPA,

    SH2_OP_COUNT
} sh2_op;

/* ------------------------------------------------------------------ flags */

enum {
    /* control flow */
    SH2F_BRANCH   = 1u << 0,   /* transfers control                        */
    SH2F_COND     = 1u << 1,   /* conditional (on T)                       */
    SH2F_DELAY    = 1u << 2,   /* has a delay slot                         */
    SH2F_CALL     = 1u << 3,   /* writes PR (subroutine call)              */
    SH2F_RET      = 1u << 4,   /* rts / rte                                */
    SH2F_INDIRECT = 1u << 5,   /* target computed from a register          */
    SH2F_ENDBLOCK = 1u << 6,   /* unconditional end of basic block         */

    /* operand shape */
    SH2F_PCREL    = 1u << 7,   /* .target is a PC-relative literal address */
    SH2F_USES_RN  = 1u << 8,
    SH2F_USES_RM  = 1u << 9,
    SH2F_USES_R0  = 1u << 10,  /* implicit R0 operand                      */

    /* memory */
    SH2F_LOAD     = 1u << 11,
    SH2F_STORE    = 1u << 12,

    /* placement restrictions */
    SH2F_NOSLOT   = 1u << 13,  /* illegal inside a delay slot              */
    SH2F_PRIV     = 1u << 14,  /* privileged                               */
};

/* Access size for SH2F_LOAD / SH2F_STORE, in bytes. 0 when not a memory op. */

typedef struct {
    uint16_t raw;       /* the 16-bit opcode                               */
    uint32_t addr;      /* address it was decoded at                       */
    uint16_t op;        /* sh2_op                                          */
    uint32_t flags;     /* SH2F_*                                          */
    uint8_t  n, m;      /* register fields (bits 11-8 / 7-4)               */
    uint8_t  size;      /* memory access size in bytes, else 0             */
    int32_t  imm;       /* immediate, sign- or zero-extended per opcode    */
    int32_t  disp;      /* displacement, already scaled to bytes           */
    uint32_t target;    /* absolute branch target or PC-relative pool addr */
} sh2_insn;

/* --------------------------------------------------------------- pattern */

typedef struct {
    uint16_t    mask, val;
    uint16_t    op;
    uint32_t    flags;
    const char *fmt;    /* mnemonic + operand template, see sh2_decoder.c  */
} sh2_pat;

/* ------------------------------------------------------------------- API */

/* Returns the matching pattern for op, or NULL if the encoding is not a
 * valid SH-2 instruction. */
const sh2_pat *sh2_match(uint16_t op);

/* Formats op (decoded at address pc) into buf (>= 64 bytes) using capstone's
 * SH syntax. Returns the pattern, or NULL if invalid (buf untouched). */
const sh2_pat *sh2_format(uint16_t op, uint32_t pc, char *buf);

/* Fully decodes op at pc into *out. Returns 1 on success, 0 if invalid
 * (in which case out->op == SH2_OP_INVALID and out->raw/addr are set). */
int sh2_decode(uint16_t op, uint32_t pc, sh2_insn *out);

/* Mnemonic name for an sh2_op, for diagnostics. */
const char *sh2_op_name(uint16_t op);

#endif /* SH2_ISA_H */
