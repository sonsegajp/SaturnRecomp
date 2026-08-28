/* sh2_decoder.c — table-driven SH-2 decoder.
 *
 * The SH-2 (SH7604) instruction set is the base of the SH family: SH-4 minus
 * the FPU, minus SHAD/SHLD, minus the SSR/SPC/DBR/SGR control registers and
 * banked-register forms, minus the cache ops (PREF/OCBx/MOVCA), minus CLRS/
 * SETS/LDTLB. Everything left is bit-identical to its SH-4 encoding, so the
 * pattern table below is the crazytaxirecomp SH-4 table restricted to that
 * subset and re-tagged with semantic opcodes and flags.
 *
 * Ordering rule: patterns are matched linearly, most-specific mask first
 * (FFFF -> F0FF -> FF00 -> F00F -> F000). Do not reorder within a mask group
 * without checking for overlap.
 *
 * Operand template escapes (expanded by sh2_format):
 *   %n %m    general register from bits 11-8 / 7-4    -> "r3"
 *   %i %u    imm8 signed / unsigned                   -> "#-128" / "#255"
 *   %a %b %c disp4 scaled x1/x2/x4
 *   %e %f %g disp8 scaled x1/x2/x4 (GBR forms)
 *   %w %l    PC-relative word / long load target      -> "0x10024"
 *   %8 %j    branch target, disp8 / disp12            -> "0xff04"
 */
#include "sh2_isa.h"
#include <stdio.h>
#include <string.h>

#define FB    SH2F_BRANCH
#define FC    SH2F_COND
#define FD    SH2F_DELAY
#define FCL   SH2F_CALL
#define FRT   SH2F_RET
#define FIN   SH2F_INDIRECT
#define FEN   SH2F_ENDBLOCK
#define FPC   SH2F_PCREL
#define FLD   SH2F_LOAD
#define FST   SH2F_STORE
#define FNS   SH2F_NOSLOT

/* Illegal-slot set per the SH-2 manual: every branch, TRAPA, and the three
 * PC-relative instructions (PC is ambiguous inside a delay slot). */
#define SLOT_BR  (FB | FNS)

static const sh2_pat pats[] = {
    /* ---------------- exact encodings (mask FFFF) ---------------- */
    { 0xFFFF, 0x0008, SH2_OP_CLRT,    0,                              "clrt"   },
    { 0xFFFF, 0x0018, SH2_OP_SETT,    0,                              "sett"   },
    { 0xFFFF, 0x0028, SH2_OP_CLRMAC,  0,                              "clrmac" },
    { 0xFFFF, 0x0009, SH2_OP_NOP,     0,                              "nop"    },
    { 0xFFFF, 0x0019, SH2_OP_DIV0U,   0,                              "div0u"  },
    { 0xFFFF, 0x000B, SH2_OP_RTS,     SLOT_BR|FD|FRT|FEN,             "rts"    },
    { 0xFFFF, 0x001B, SH2_OP_SLEEP,   0,                              "sleep"  },
    { 0xFFFF, 0x002B, SH2_OP_RTE,     SLOT_BR|FD|FRT|FEN,             "rte"    },

    /* ---------------- one-register (mask F0FF), 0x0xxx ---------------- */
    { 0xF0FF, 0x0002, SH2_OP_STC_SR,   0,                             "stc sr,%n"   },
    { 0xF0FF, 0x0012, SH2_OP_STC_GBR,  0,                             "stc gbr,%n"  },
    { 0xF0FF, 0x0022, SH2_OP_STC_VBR,  0,                             "stc vbr,%n"  },
    { 0xF0FF, 0x0003, SH2_OP_BSRF,     SLOT_BR|FD|FCL|FIN,            "bsrf %n"     },
    { 0xF0FF, 0x0023, SH2_OP_BRAF,     SLOT_BR|FD|FEN|FIN,            "braf %n"     },
    { 0xF0FF, 0x0029, SH2_OP_MOVT,     0,                             "movt %n"     },
    { 0xF0FF, 0x000A, SH2_OP_STS_MACH, 0,                             "sts mach,%n" },
    { 0xF0FF, 0x001A, SH2_OP_STS_MACL, 0,                             "sts macl,%n" },
    { 0xF0FF, 0x002A, SH2_OP_STS_PR,   0,                             "sts pr,%n"   },

    /* ---------------- one-register (mask F0FF), 0x4xxx ---------------- */
    { 0xF0FF, 0x4000, SH2_OP_SHLL,     0,                             "shll %n"        },
    { 0xF0FF, 0x4001, SH2_OP_SHLR,     0,                             "shlr %n"        },
    { 0xF0FF, 0x4002, SH2_OP_STSL_MACH,FST,                           "sts.l mach,@-%n"},
    { 0xF0FF, 0x4003, SH2_OP_STCL_SR,  FST,                           "stc.l sr,@-%n"  },
    { 0xF0FF, 0x4004, SH2_OP_ROTL,     0,                             "rotl %n"        },
    { 0xF0FF, 0x4005, SH2_OP_ROTR,     0,                             "rotr %n"        },
    { 0xF0FF, 0x4006, SH2_OP_LDSL_MACH,FLD,                           "lds.l @%n+,mach"},
    { 0xF0FF, 0x4007, SH2_OP_LDCL_SR,  FLD,                           "ldc.l @%n+,sr"  },
    { 0xF0FF, 0x4008, SH2_OP_SHLL2,    0,                             "shll2 %n"       },
    { 0xF0FF, 0x4009, SH2_OP_SHLR2,    0,                             "shlr2 %n"       },
    { 0xF0FF, 0x400A, SH2_OP_LDS_MACH, 0,                             "lds %n,mach"    },
    { 0xF0FF, 0x400B, SH2_OP_JSR,      SLOT_BR|FD|FCL|FIN,            "jsr @%n"        },
    { 0xF0FF, 0x400E, SH2_OP_LDC_SR,   0,                             "ldc %n,sr"      },
    { 0xF0FF, 0x4010, SH2_OP_DT,       0,                             "dt %n"          },
    { 0xF0FF, 0x4011, SH2_OP_CMPPZ,    0,                             "cmp/pz %n"      },
    { 0xF0FF, 0x4012, SH2_OP_STSL_MACL,FST,                           "sts.l macl,@-%n"},
    { 0xF0FF, 0x4013, SH2_OP_STCL_GBR, FST,                           "stc.l gbr,@-%n" },
    { 0xF0FF, 0x4015, SH2_OP_CMPPL,    0,                             "cmp/pl %n"      },
    { 0xF0FF, 0x4016, SH2_OP_LDSL_MACL,FLD,                           "lds.l @%n+,macl"},
    { 0xF0FF, 0x4017, SH2_OP_LDCL_GBR, FLD,                           "ldc.l @%n+,gbr" },
    { 0xF0FF, 0x4018, SH2_OP_SHLL8,    0,                             "shll8 %n"       },
    { 0xF0FF, 0x4019, SH2_OP_SHLR8,    0,                             "shlr8 %n"       },
    { 0xF0FF, 0x401A, SH2_OP_LDS_MACL, 0,                             "lds %n,macl"    },
    { 0xF0FF, 0x401B, SH2_OP_TAS,      FLD|FST,                       "tas.b @%n"      },
    { 0xF0FF, 0x401E, SH2_OP_LDC_GBR,  0,                             "ldc %n,gbr"     },
    { 0xF0FF, 0x4020, SH2_OP_SHAL,     0,                             "shal %n"        },
    { 0xF0FF, 0x4021, SH2_OP_SHAR,     0,                             "shar %n"        },
    { 0xF0FF, 0x4022, SH2_OP_STSL_PR,  FST,                           "sts.l pr,@-%n"  },
    { 0xF0FF, 0x4023, SH2_OP_STCL_VBR, FST,                           "stc.l vbr,@-%n" },
    { 0xF0FF, 0x4024, SH2_OP_ROTCL,    0,                             "rotcl %n"       },
    { 0xF0FF, 0x4025, SH2_OP_ROTCR,    0,                             "rotcr %n"       },
    { 0xF0FF, 0x4026, SH2_OP_LDSL_PR,  FLD,                           "lds.l @%n+,pr"  },
    { 0xF0FF, 0x4027, SH2_OP_LDCL_VBR, FLD,                           "ldc.l @%n+,vbr" },
    { 0xF0FF, 0x4028, SH2_OP_SHLL16,   0,                             "shll16 %n"      },
    { 0xF0FF, 0x4029, SH2_OP_SHLR16,   0,                             "shlr16 %n"      },
    { 0xF0FF, 0x402A, SH2_OP_LDS_PR,   0,                             "lds %n,pr"      },
    { 0xF0FF, 0x402B, SH2_OP_JMP,      SLOT_BR|FD|FEN|FIN,            "jmp @%n"        },
    { 0xF0FF, 0x402E, SH2_OP_LDC_VBR,  0,                             "ldc %n,vbr"     },

    /* ---------------- imm/disp rows (mask FF00) ---------------- */
    { 0xFF00, 0x8000, SH2_OP_MOVB_STD, FST,                           "mov.b r0,@(%a,%m)"    },
    { 0xFF00, 0x8100, SH2_OP_MOVW_STD, FST,                           "mov.w r0,@(%b,%m)"    },
    { 0xFF00, 0x8400, SH2_OP_MOVB_LDD, FLD,                           "mov.b @(%a,%m),r0"    },
    { 0xFF00, 0x8500, SH2_OP_MOVW_LDD, FLD,                           "mov.w @(%b,%m),r0"    },
    { 0xFF00, 0x8800, SH2_OP_CMPEQ_I,  0,                             "cmp/eq %i,r0"         },
    { 0xFF00, 0x8900, SH2_OP_BT,       SLOT_BR|FC,                    "bt %8"                },
    { 0xFF00, 0x8B00, SH2_OP_BF,       SLOT_BR|FC,                    "bf %8"                },
    { 0xFF00, 0x8D00, SH2_OP_BTS,      SLOT_BR|FC|FD,                 "bt/s %8"              },
    { 0xFF00, 0x8F00, SH2_OP_BFS,      SLOT_BR|FC|FD,                 "bf/s %8"              },
    { 0xFF00, 0xC000, SH2_OP_MOVB_STG, FST,                           "mov.b r0,@(%e,gbr)"   },
    { 0xFF00, 0xC100, SH2_OP_MOVW_STG, FST,                           "mov.w r0,@(%f,gbr)"   },
    { 0xFF00, 0xC200, SH2_OP_MOVL_STG, FST,                           "mov.l r0,@(%g,gbr)"   },
    { 0xFF00, 0xC300, SH2_OP_TRAPA,    FB|FNS|FEN,                    "trapa %u"             },
    { 0xFF00, 0xC400, SH2_OP_MOVB_LDG, FLD,                           "mov.b @(%e,gbr),r0"   },
    { 0xFF00, 0xC500, SH2_OP_MOVW_LDG, FLD,                           "mov.w @(%f,gbr),r0"   },
    { 0xFF00, 0xC600, SH2_OP_MOVL_LDG, FLD,                           "mov.l @(%g,gbr),r0"   },
    { 0xFF00, 0xC700, SH2_OP_MOVA,     FPC|FNS,                       "mova %l,r0"           },
    { 0xFF00, 0xC800, SH2_OP_TST_I,    0,                             "tst %u,r0"            },
    { 0xFF00, 0xC900, SH2_OP_AND_I,    0,                             "and %u,r0"            },
    { 0xFF00, 0xCA00, SH2_OP_XOR_I,    0,                             "xor %u,r0"            },
    { 0xFF00, 0xCB00, SH2_OP_OR_I,     0,                             "or %u,r0"             },
    { 0xFF00, 0xCC00, SH2_OP_TSTB_G,   FLD,                           "tst.b %u,@(r0,gbr)"   },
    { 0xFF00, 0xCD00, SH2_OP_ANDB_G,   FLD|FST,                       "and.b %u,@(r0,gbr)"   },
    { 0xFF00, 0xCE00, SH2_OP_XORB_G,   FLD|FST,                       "xor.b %u,@(r0,gbr)"   },
    { 0xFF00, 0xCF00, SH2_OP_ORB_G,    FLD|FST,                       "or.b %u,@(r0,gbr)"    },

    /* ---------------- two-register (mask F00F) ---------------- */
    { 0xF00F, 0x0004, SH2_OP_MOVB_ST0, FST,                           "mov.b %m,@(r0,%n)"    },
    { 0xF00F, 0x0005, SH2_OP_MOVW_ST0, FST,                           "mov.w %m,@(r0,%n)"    },
    { 0xF00F, 0x0006, SH2_OP_MOVL_ST0, FST,                           "mov.l %m,@(r0,%n)"    },
    { 0xF00F, 0x0007, SH2_OP_MULL,     0,                             "mul.l %m,%n"          },
    { 0xF00F, 0x000C, SH2_OP_MOVB_LD0, FLD,                           "mov.b @(r0,%m),%n"    },
    { 0xF00F, 0x000D, SH2_OP_MOVW_LD0, FLD,                           "mov.w @(r0,%m),%n"    },
    { 0xF00F, 0x000E, SH2_OP_MOVL_LD0, FLD,                           "mov.l @(r0,%m),%n"    },
    { 0xF00F, 0x000F, SH2_OP_MACL,     FLD,                           "mac.l @%m+,@%n+"      },
    { 0xF00F, 0x2000, SH2_OP_MOVB_ST,  FST,                           "mov.b %m,@%n"         },
    { 0xF00F, 0x2001, SH2_OP_MOVW_ST,  FST,                           "mov.w %m,@%n"         },
    { 0xF00F, 0x2002, SH2_OP_MOVL_ST,  FST,                           "mov.l %m,@%n"         },
    { 0xF00F, 0x2004, SH2_OP_MOVB_STP, FST,                           "mov.b %m,@-%n"        },
    { 0xF00F, 0x2005, SH2_OP_MOVW_STP, FST,                           "mov.w %m,@-%n"        },
    { 0xF00F, 0x2006, SH2_OP_MOVL_STP, FST,                           "mov.l %m,@-%n"        },
    { 0xF00F, 0x2007, SH2_OP_DIV0S,    0,                             "div0s %m,%n"          },
    { 0xF00F, 0x2008, SH2_OP_TST,      0,                             "tst %m,%n"            },
    { 0xF00F, 0x2009, SH2_OP_AND,      0,                             "and %m,%n"            },
    { 0xF00F, 0x200A, SH2_OP_XOR,      0,                             "xor %m,%n"            },
    { 0xF00F, 0x200B, SH2_OP_OR,       0,                             "or %m,%n"             },
    { 0xF00F, 0x200C, SH2_OP_CMPSTR,   0,                             "cmp/str %m,%n"        },
    { 0xF00F, 0x200D, SH2_OP_XTRCT,    0,                             "xtrct %m,%n"          },
    { 0xF00F, 0x200E, SH2_OP_MULUW,    0,                             "mulu.w %m,%n"         },
    { 0xF00F, 0x200F, SH2_OP_MULSW,    0,                             "muls.w %m,%n"         },
    { 0xF00F, 0x3000, SH2_OP_CMPEQ,    0,                             "cmp/eq %m,%n"         },
    { 0xF00F, 0x3002, SH2_OP_CMPHS,    0,                             "cmp/hs %m,%n"         },
    { 0xF00F, 0x3003, SH2_OP_CMPGE,    0,                             "cmp/ge %m,%n"         },
    { 0xF00F, 0x3004, SH2_OP_DIV1,     0,                             "div1 %m,%n"           },
    { 0xF00F, 0x3005, SH2_OP_DMULU,    0,                             "dmulu.l %m,%n"        },
    { 0xF00F, 0x3006, SH2_OP_CMPHI,    0,                             "cmp/hi %m,%n"         },
    { 0xF00F, 0x3007, SH2_OP_CMPGT,    0,                             "cmp/gt %m,%n"         },
    { 0xF00F, 0x3008, SH2_OP_SUB,      0,                             "sub %m,%n"            },
    { 0xF00F, 0x300A, SH2_OP_SUBC,     0,                             "subc %m,%n"           },
    { 0xF00F, 0x300B, SH2_OP_SUBV,     0,                             "subv %m,%n"           },
    { 0xF00F, 0x300C, SH2_OP_ADD,      0,                             "add %m,%n"            },
    { 0xF00F, 0x300D, SH2_OP_DMULS,    0,                             "dmuls.l %m,%n"        },
    { 0xF00F, 0x300E, SH2_OP_ADDC,     0,                             "addc %m,%n"           },
    { 0xF00F, 0x300F, SH2_OP_ADDV,     0,                             "addv %m,%n"           },
    { 0xF00F, 0x400F, SH2_OP_MACW,     FLD,                           "mac.w @%m+,@%n+"      },
    { 0xF00F, 0x6000, SH2_OP_MOVB_LD,  FLD,                           "mov.b @%m,%n"         },
    { 0xF00F, 0x6001, SH2_OP_MOVW_LD,  FLD,                           "mov.w @%m,%n"         },
    { 0xF00F, 0x6002, SH2_OP_MOVL_LD,  FLD,                           "mov.l @%m,%n"         },
    { 0xF00F, 0x6003, SH2_OP_MOV_RR,   0,                             "mov %m,%n"            },
    { 0xF00F, 0x6004, SH2_OP_MOVB_LDP, FLD,                           "mov.b @%m+,%n"        },
    { 0xF00F, 0x6005, SH2_OP_MOVW_LDP, FLD,                           "mov.w @%m+,%n"        },
    { 0xF00F, 0x6006, SH2_OP_MOVL_LDP, FLD,                           "mov.l @%m+,%n"        },
    { 0xF00F, 0x6007, SH2_OP_NOT,      0,                             "not %m,%n"            },
    { 0xF00F, 0x6008, SH2_OP_SWAPB,    0,                             "swap.b %m,%n"         },
    { 0xF00F, 0x6009, SH2_OP_SWAPW,    0,                             "swap.w %m,%n"         },
    { 0xF00F, 0x600A, SH2_OP_NEGC,     0,                             "negc %m,%n"           },
    { 0xF00F, 0x600B, SH2_OP_NEG,      0,                             "neg %m,%n"            },
    { 0xF00F, 0x600C, SH2_OP_EXTUB,    0,                             "extu.b %m,%n"         },
    { 0xF00F, 0x600D, SH2_OP_EXTUW,    0,                             "extu.w %m,%n"         },
    { 0xF00F, 0x600E, SH2_OP_EXTSB,    0,                             "exts.b %m,%n"         },
    { 0xF00F, 0x600F, SH2_OP_EXTSW,    0,                             "exts.w %m,%n"         },

    /* ---------------- full-row (mask F000) ---------------- */
    { 0xF000, 0x1000, SH2_OP_MOVL_STD, FST,                           "mov.l %m,@(%c,%n)"    },
    { 0xF000, 0x5000, SH2_OP_MOVL_LDD, FLD,                           "mov.l @(%c,%m),%n"    },
    { 0xF000, 0x7000, SH2_OP_ADD_I,    0,                             "add %i,%n"            },
    { 0xF000, 0x9000, SH2_OP_MOVW_PC,  FLD|FPC|FNS,                   "mov.w %w,%n"          },
    { 0xF000, 0xA000, SH2_OP_BRA,      SLOT_BR|FD|FEN,                "bra %j"               },
    { 0xF000, 0xB000, SH2_OP_BSR,      SLOT_BR|FD|FCL,                "bsr %j"               },
    { 0xF000, 0xD000, SH2_OP_MOVL_PC,  FLD|FPC|FNS,                   "mov.l %l,%n"          },
    { 0xF000, 0xE000, SH2_OP_MOV_I,    0,                             "mov %i,%n"            },
};

#define NPATS (sizeof(pats) / sizeof(pats[0]))

/* The linear scan is correct but costs ~NPATS mask-compares per call, and the
 * interpreter calls this for every instruction executed. Memoise the result
 * per opcode: the pattern table is immutable, so the first lookup of each of
 * the 65536 encodings is the only one that pays for the scan. Index+1 is
 * stored so the zero-initialised table reads as "unfilled"; -1 is "invalid".
 */
static int32_t match_memo[65536];

const sh2_pat *sh2_match(uint16_t op)
{
    int32_t e = match_memo[op];
    size_t i;
    if (e > 0)  return &pats[e - 1];
    if (e < 0)  return NULL;
    for (i = 0; i < NPATS; i++)
        if ((op & pats[i].mask) == pats[i].val) {
            match_memo[op] = (int32_t)(i + 1);
            return &pats[i];
        }
    match_memo[op] = -1;
    return NULL;
}

/* PC-relative bases. For a word load the base is PC+4; for a long load and
 * for MOVA the low two bits of PC are masked off first. */
static uint32_t pcrel_w(uint16_t op, uint32_t pc) { return pc + 4 + (op & 0xFF) * 2u; }
static uint32_t pcrel_l(uint16_t op, uint32_t pc) { return (pc & ~3u) + 4 + (op & 0xFF) * 4u; }

const sh2_pat *sh2_format(uint16_t op, uint32_t pc, char *buf)
{
    const sh2_pat *p = sh2_match(op);
    const char *f;
    char *o = buf;

    if (!p)
        return NULL;

    for (f = p->fmt; *f; f++) {
        if (*f != '%') { *o++ = *f; continue; }
        f++;
        switch (*f) {
        case 'n': o += sprintf(o, "r%u", (op >> 8) & 15); break;
        case 'm': o += sprintf(o, "r%u", (op >> 4) & 15); break;
        case 'i': o += sprintf(o, "#%d", (int)(int8_t)op); break;
        case 'u': o += sprintf(o, "#%u", op & 0xFF); break;
        case 'a': o += sprintf(o, "%u", op & 15); break;
        case 'b': o += sprintf(o, "%u", (op & 15) * 2); break;
        case 'c': o += sprintf(o, "%u", (op & 15) * 4); break;
        case 'e': o += sprintf(o, "%u", op & 0xFF); break;
        case 'f': o += sprintf(o, "%u", (op & 0xFF) * 2); break;
        case 'g': o += sprintf(o, "%u", (op & 0xFF) * 4); break;
        case 'w': o += sprintf(o, "0x%x", pcrel_w(op, pc)); break;
        case 'l': o += sprintf(o, "0x%x", pcrel_l(op, pc)); break;
        case '8': o += sprintf(o, "0x%x", pc + 4 + (uint32_t)((int8_t)op * 2)); break;
        case 'j': o += sprintf(o, "0x%x",
                      pc + 4 + (uint32_t)(((int16_t)(op << 4) >> 4) * 2)); break;
        default:  *o++ = *f; break;
        }
    }
    *o = 0;
    return p;
}

/* Memory access size implied by the opcode, in bytes. */
static uint8_t op_size(uint16_t o)
{
    switch (o) {
    case SH2_OP_MOVB_LD: case SH2_OP_MOVB_ST:  case SH2_OP_MOVB_LDP:
    case SH2_OP_MOVB_STP: case SH2_OP_MOVB_LD0: case SH2_OP_MOVB_ST0:
    case SH2_OP_MOVB_LDD: case SH2_OP_MOVB_STD: case SH2_OP_MOVB_LDG:
    case SH2_OP_MOVB_STG: case SH2_OP_TAS:
    case SH2_OP_TSTB_G: case SH2_OP_ANDB_G: case SH2_OP_XORB_G: case SH2_OP_ORB_G:
        return 1;
    case SH2_OP_MOVW_LD: case SH2_OP_MOVW_ST:  case SH2_OP_MOVW_LDP:
    case SH2_OP_MOVW_STP: case SH2_OP_MOVW_LD0: case SH2_OP_MOVW_ST0:
    case SH2_OP_MOVW_LDD: case SH2_OP_MOVW_STD: case SH2_OP_MOVW_LDG:
    case SH2_OP_MOVW_STG: case SH2_OP_MOVW_PC:  case SH2_OP_MACW:
        return 2;
    case SH2_OP_MOVL_LD: case SH2_OP_MOVL_ST:  case SH2_OP_MOVL_LDP:
    case SH2_OP_MOVL_STP: case SH2_OP_MOVL_LD0: case SH2_OP_MOVL_ST0:
    case SH2_OP_MOVL_LDD: case SH2_OP_MOVL_STD: case SH2_OP_MOVL_LDG:
    case SH2_OP_MOVL_STG: case SH2_OP_MOVL_PC:  case SH2_OP_MACL:
        return 4;
    default:
        return 0;
    }
}

int sh2_decode(uint16_t op, uint32_t pc, sh2_insn *out)
{
    const sh2_pat *p = sh2_match(op);
    const char *f;

    memset(out, 0, sizeof(*out));
    out->raw  = op;
    out->addr = pc;

    if (!p) {
        out->op = SH2_OP_INVALID;
        return 0;
    }

    out->op    = p->op;
    out->flags = p->flags;
    out->n     = (uint8_t)((op >> 8) & 15);
    out->m     = (uint8_t)((op >> 4) & 15);
    out->size  = op_size(p->op);

    /* Derive operand usage from the template so it can never disagree with
     * the printed form. */
    for (f = p->fmt; *f; f++) {
        if (f[0] == '%' && f[1] == 'n') out->flags |= SH2F_USES_RN;
        if (f[0] == '%' && f[1] == 'm') out->flags |= SH2F_USES_RM;
        if (f[0] == 'r' && f[1] == '0' && (f[2] == ',' || f[2] == ')' || f[2] == 0))
            out->flags |= SH2F_USES_R0;
    }

    /* Immediates, displacements and targets. */
    switch (p->op) {
    case SH2_OP_MOV_I: case SH2_OP_ADD_I: case SH2_OP_CMPEQ_I:
        out->imm = (int8_t)op;
        break;
    case SH2_OP_TST_I: case SH2_OP_AND_I: case SH2_OP_OR_I: case SH2_OP_XOR_I:
    case SH2_OP_TSTB_G: case SH2_OP_ANDB_G: case SH2_OP_ORB_G: case SH2_OP_XORB_G:
    case SH2_OP_TRAPA:
        out->imm = op & 0xFF;
        break;

    case SH2_OP_MOVB_LDD: case SH2_OP_MOVB_STD: out->disp = op & 15;       break;
    case SH2_OP_MOVW_LDD: case SH2_OP_MOVW_STD: out->disp = (op & 15) * 2; break;
    case SH2_OP_MOVL_LDD: case SH2_OP_MOVL_STD: out->disp = (op & 15) * 4; break;

    case SH2_OP_MOVB_LDG: case SH2_OP_MOVB_STG: out->disp = op & 0xFF;       break;
    case SH2_OP_MOVW_LDG: case SH2_OP_MOVW_STG: out->disp = (op & 0xFF) * 2; break;
    case SH2_OP_MOVL_LDG: case SH2_OP_MOVL_STG: out->disp = (op & 0xFF) * 4; break;

    case SH2_OP_MOVW_PC:
        out->disp   = (op & 0xFF) * 2;
        out->target = pcrel_w(op, pc);
        break;
    case SH2_OP_MOVL_PC:
    case SH2_OP_MOVA:
        out->disp   = (op & 0xFF) * 4;
        out->target = pcrel_l(op, pc);
        break;

    case SH2_OP_BT: case SH2_OP_BF: case SH2_OP_BTS: case SH2_OP_BFS:
        out->disp   = (int8_t)op * 2;
        out->target = pc + 4 + (uint32_t)out->disp;
        break;
    case SH2_OP_BRA: case SH2_OP_BSR:
        out->disp   = ((int16_t)(op << 4) >> 4) * 2;
        out->target = pc + 4 + (uint32_t)out->disp;
        break;

    default:
        break;
    }

    return 1;
}

static const char *const op_names[SH2_OP_COUNT] = {
    [SH2_OP_INVALID]   = ".invalid",
    [SH2_OP_MOV_RR]    = "mov",       [SH2_OP_MOV_I]     = "mov#",
    [SH2_OP_MOVW_PC]   = "mov.w@pc",  [SH2_OP_MOVL_PC]   = "mov.l@pc",
    [SH2_OP_MOVB_LD]   = "mov.b@ld",  [SH2_OP_MOVW_LD]   = "mov.w@ld",
    [SH2_OP_MOVL_LD]   = "mov.l@ld",  [SH2_OP_MOVB_ST]   = "mov.b@st",
    [SH2_OP_MOVW_ST]   = "mov.w@st",  [SH2_OP_MOVL_ST]   = "mov.l@st",
    [SH2_OP_MOVB_LDP]  = "mov.b@ld+", [SH2_OP_MOVW_LDP]  = "mov.w@ld+",
    [SH2_OP_MOVL_LDP]  = "mov.l@ld+", [SH2_OP_MOVB_STP]  = "mov.b@-st",
    [SH2_OP_MOVW_STP]  = "mov.w@-st", [SH2_OP_MOVL_STP]  = "mov.l@-st",
    [SH2_OP_MOVB_LD0]  = "mov.b@r0l", [SH2_OP_MOVW_LD0]  = "mov.w@r0l",
    [SH2_OP_MOVL_LD0]  = "mov.l@r0l", [SH2_OP_MOVB_ST0]  = "mov.b@r0s",
    [SH2_OP_MOVW_ST0]  = "mov.w@r0s", [SH2_OP_MOVL_ST0]  = "mov.l@r0s",
    [SH2_OP_MOVB_LDD]  = "mov.b@dl",  [SH2_OP_MOVW_LDD]  = "mov.w@dl",
    [SH2_OP_MOVL_LDD]  = "mov.l@dl",  [SH2_OP_MOVB_STD]  = "mov.b@ds",
    [SH2_OP_MOVW_STD]  = "mov.w@ds",  [SH2_OP_MOVL_STD]  = "mov.l@ds",
    [SH2_OP_MOVB_LDG]  = "mov.b@gl",  [SH2_OP_MOVW_LDG]  = "mov.w@gl",
    [SH2_OP_MOVL_LDG]  = "mov.l@gl",  [SH2_OP_MOVB_STG]  = "mov.b@gs",
    [SH2_OP_MOVW_STG]  = "mov.w@gs",  [SH2_OP_MOVL_STG]  = "mov.l@gs",
    [SH2_OP_MOVA]      = "mova",      [SH2_OP_MOVT]      = "movt",
    [SH2_OP_SWAPB]     = "swap.b",    [SH2_OP_SWAPW]     = "swap.w",
    [SH2_OP_XTRCT]     = "xtrct",
    [SH2_OP_ADD]       = "add",       [SH2_OP_ADD_I]     = "add#",
    [SH2_OP_ADDC]      = "addc",      [SH2_OP_ADDV]      = "addv",
    [SH2_OP_CMPEQ_I]   = "cmp/eq#",   [SH2_OP_CMPEQ]     = "cmp/eq",
    [SH2_OP_CMPHS]     = "cmp/hs",    [SH2_OP_CMPGE]     = "cmp/ge",
    [SH2_OP_CMPHI]     = "cmp/hi",    [SH2_OP_CMPGT]     = "cmp/gt",
    [SH2_OP_CMPPZ]     = "cmp/pz",    [SH2_OP_CMPPL]     = "cmp/pl",
    [SH2_OP_CMPSTR]    = "cmp/str",   [SH2_OP_DIV1]      = "div1",
    [SH2_OP_DIV0S]     = "div0s",     [SH2_OP_DIV0U]     = "div0u",
    [SH2_OP_DMULS]     = "dmuls.l",   [SH2_OP_DMULU]     = "dmulu.l",
    [SH2_OP_DT]        = "dt",        [SH2_OP_EXTSB]     = "exts.b",
    [SH2_OP_EXTSW]     = "exts.w",    [SH2_OP_EXTUB]     = "extu.b",
    [SH2_OP_EXTUW]     = "extu.w",    [SH2_OP_MACL]      = "mac.l",
    [SH2_OP_MACW]      = "mac.w",     [SH2_OP_MULL]      = "mul.l",
    [SH2_OP_MULSW]     = "muls.w",    [SH2_OP_MULUW]     = "mulu.w",
    [SH2_OP_NEG]       = "neg",       [SH2_OP_NEGC]      = "negc",
    [SH2_OP_SUB]       = "sub",       [SH2_OP_SUBC]      = "subc",
    [SH2_OP_SUBV]      = "subv",
    [SH2_OP_AND]       = "and",       [SH2_OP_AND_I]     = "and#",
    [SH2_OP_ANDB_G]    = "and.b",     [SH2_OP_NOT]       = "not",
    [SH2_OP_OR]        = "or",        [SH2_OP_OR_I]      = "or#",
    [SH2_OP_ORB_G]     = "or.b",      [SH2_OP_TAS]       = "tas.b",
    [SH2_OP_TST]       = "tst",       [SH2_OP_TST_I]     = "tst#",
    [SH2_OP_TSTB_G]    = "tst.b",     [SH2_OP_XOR]       = "xor",
    [SH2_OP_XOR_I]     = "xor#",      [SH2_OP_XORB_G]    = "xor.b",
    [SH2_OP_ROTL]      = "rotl",      [SH2_OP_ROTR]      = "rotr",
    [SH2_OP_ROTCL]     = "rotcl",     [SH2_OP_ROTCR]     = "rotcr",
    [SH2_OP_SHAL]      = "shal",      [SH2_OP_SHAR]      = "shar",
    [SH2_OP_SHLL]      = "shll",      [SH2_OP_SHLR]      = "shlr",
    [SH2_OP_SHLL2]     = "shll2",     [SH2_OP_SHLR2]     = "shlr2",
    [SH2_OP_SHLL8]     = "shll8",     [SH2_OP_SHLR8]     = "shlr8",
    [SH2_OP_SHLL16]    = "shll16",    [SH2_OP_SHLR16]    = "shlr16",
    [SH2_OP_BF]        = "bf",        [SH2_OP_BFS]       = "bf/s",
    [SH2_OP_BT]        = "bt",        [SH2_OP_BTS]       = "bt/s",
    [SH2_OP_BRA]       = "bra",       [SH2_OP_BRAF]      = "braf",
    [SH2_OP_BSR]       = "bsr",       [SH2_OP_BSRF]      = "bsrf",
    [SH2_OP_JMP]       = "jmp",       [SH2_OP_JSR]       = "jsr",
    [SH2_OP_RTS]       = "rts",
    [SH2_OP_CLRMAC]    = "clrmac",    [SH2_OP_CLRT]      = "clrt",
    [SH2_OP_LDC_SR]    = "ldc sr",    [SH2_OP_LDC_GBR]   = "ldc gbr",
    [SH2_OP_LDC_VBR]   = "ldc vbr",   [SH2_OP_LDCL_SR]   = "ldc.l sr",
    [SH2_OP_LDCL_GBR]  = "ldc.l gbr", [SH2_OP_LDCL_VBR]  = "ldc.l vbr",
    [SH2_OP_LDS_MACH]  = "lds mach",  [SH2_OP_LDS_MACL]  = "lds macl",
    [SH2_OP_LDS_PR]    = "lds pr",    [SH2_OP_LDSL_MACH] = "lds.l mach",
    [SH2_OP_LDSL_MACL] = "lds.l macl",[SH2_OP_LDSL_PR]   = "lds.l pr",
    [SH2_OP_NOP]       = "nop",       [SH2_OP_RTE]       = "rte",
    [SH2_OP_SETT]      = "sett",      [SH2_OP_SLEEP]     = "sleep",
    [SH2_OP_STC_SR]    = "stc sr",    [SH2_OP_STC_GBR]   = "stc gbr",
    [SH2_OP_STC_VBR]   = "stc vbr",   [SH2_OP_STCL_SR]   = "stc.l sr",
    [SH2_OP_STCL_GBR]  = "stc.l gbr", [SH2_OP_STCL_VBR]  = "stc.l vbr",
    [SH2_OP_STS_MACH]  = "sts mach",  [SH2_OP_STS_MACL]  = "sts macl",
    [SH2_OP_STS_PR]    = "sts pr",    [SH2_OP_STSL_MACH] = "sts.l mach",
    [SH2_OP_STSL_MACL] = "sts.l macl",[SH2_OP_STSL_PR]   = "sts.l pr",
    [SH2_OP_TRAPA]     = "trapa",
};

const char *sh2_op_name(uint16_t op)
{
    if (op >= SH2_OP_COUNT || !op_names[op])
        return "?";
    return op_names[op];
}
