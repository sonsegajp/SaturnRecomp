#!/usr/bin/env python3
"""
SH-2 disassembler and function finder for Sega Saturn binaries.

Ported from dcrecomp's tools/sh4_disasm.py (MIT, sp00nznet) to the Saturn's
SH-2. The pipeline shape is the same -- decode every halfword, find function
entry points from call targets and prologues, emit a function map -- but the
decoder is a different instruction set on a different-endian machine.

WHAT CHANGED FROM THE SH-4 ORIGINAL, and why each matters:

  * ENDIANNESS. Dreamcast SH-4 runs little-endian; Saturn SH-2 runs
    BIG-endian. dcrecomp reads opcodes with struct '<H'. Doing that here
    byte-swaps every instruction: 0x4F22 (sts.l pr,@-r15, the prologue the
    function finder keys on) reads as 0x224F (mov.l r4,@r2), so the finder
    reports almost no functions and the ones it does find are noise. Every
    read in this file is '>H' / '>I'. This is the single highest-consequence
    difference in the whole port.

  * NO FPU. SH-2 has no floating-point unit at all -- no FR/XF banks, no
    FPSCR, no FMOV/FADD/FTRC. About a third of the SH-4 decoder is dropped
    rather than translated, and encodings in the 0xF000 block are simply
    invalid on SH-2.

  * NO MMU, no store queues, no register banking. SH-2's SR holds only
    T, S, I[3:0], Q and M -- there is no RB/MD/BL, so no SSR/SPC/SGR/DBR
    and no `ldc Rm,SSR`-style instructions.

  * SMALLER CONTROL SET. SH-2 has STC/LDC for SR, GBR and VBR only.
    SH-3/4 additions (SHAD, SHLD, PREF, OCBI/OCBP/OCBWB, MOVCA.L, TRAPA's
    SH-4 semantics) do not exist here.

  * ADDRESSES. Saturn code runs from WRAM-H at 0x06000000 (1 MB) or WRAM-L
    at 0x00200000, not 0x8C010000. The SH-2 also mirrors all of memory
    through the cache partition in bits 31-29, so 0x06004000, 0x26004000
    and 0x00004000-ish forms all name the same bytes; normalise() folds
    them together before anything is looked up.

Usage:
    python tools/sh2_disasm.py 0NIGHTS.BIN --base 0x06004000
    python tools/sh2_disasm.py 0NIGHTS.BIN --base 0x06004000 --json map.json
    python tools/sh2_disasm.py 0NIGHTS.BIN --base 0x06004000 --list 0x06004000 64
"""

import argparse
import json
import struct
import sys
from collections import defaultdict

# NiGHTS's 1st-read load address; a sensible Saturn default. Any game's real
# value comes from IP.BIN, which recompiler/src/disc.c already parses.
LOAD_ADDR = 0x06004000

# The SH-2 selects a cache partition with address bits 31-29 and decodes only
# the low 27 for the bus. runner/src/bus.c does exactly this (`a & 0x07FFFFFF`),
# so the tools must agree or a jump through 0x2xxxxxxx will not match the
# function recorded at 0x0xxxxxxx.
ADDR_MASK = 0x07FFFFFF


def sext8(v):
    return v if v < 0x80 else v - 0x100


def sext12(v):
    return v if v < 0x800 else v - 0x1000


def normalize(addr):
    """Fold an SH-2 address to its cache-partition-independent form."""
    return addr & ADDR_MASK


# ---------------------------------------------------------------- decoder

def decode_sh2(opcode, pc):
    """
    Decode one SH-2 instruction.

    Returns (mnemonic, operands, is_branch, target) where target is:
        int      - a resolved absolute branch target
        'rts'    - return from subroutine
        'rte'    - return from exception
        'indirect' - target comes from a register (jmp/jsr/braf/bsrf)
        None     - not a branch

    `pc` is the address of the instruction itself. SH-2 PC-relative operands
    are measured from pc+4 because the pipeline has already fetched two more
    halfwords by the time the operand is formed.
    """
    n = (opcode >> 8) & 0xF
    m = (opcode >> 4) & 0xF
    d = opcode & 0xF
    i = opcode & 0xFF
    d8 = opcode & 0xFF
    d12 = opcode & 0xFFF

    # ---- fixed encodings ------------------------------------------------
    fixed = {
        0x0008: ("clrt", ""),
        0x0018: ("sett", ""),
        0x0028: ("clrmac", ""),
        0x0009: ("nop", ""),
        0x0019: ("div0u", ""),
        0x001B: ("sleep", ""),
    }
    if opcode in fixed:
        mn, ops = fixed[opcode]
        return mn, ops, False, None
    if opcode == 0x000B:
        return "rts", "", True, "rts"
    if opcode == 0x002B:
        return "rte", "", True, "rte"

    hi = opcode >> 12

    # ---- 0x0... ---------------------------------------------------------
    if hi == 0x0:
        lo = opcode & 0xFF
        if (opcode & 0xF00F) == 0x0004:
            return "mov.b", f"r{m}, @(r0, r{n})", False, None
        if (opcode & 0xF00F) == 0x0005:
            return "mov.w", f"r{m}, @(r0, r{n})", False, None
        if (opcode & 0xF00F) == 0x0006:
            return "mov.l", f"r{m}, @(r0, r{n})", False, None
        if (opcode & 0xF00F) == 0x0007:
            return "mul.l", f"r{m}, r{n}", False, None
        if (opcode & 0xF00F) == 0x000C:
            return "mov.b", f"@(r0, r{m}), r{n}", False, None
        if (opcode & 0xF00F) == 0x000D:
            return "mov.w", f"@(r0, r{m}), r{n}", False, None
        if (opcode & 0xF00F) == 0x000E:
            return "mov.l", f"@(r0, r{m}), r{n}", False, None
        if (opcode & 0xF00F) == 0x000F:
            return "mac.l", f"@r{m}+, @r{n}+", False, None
        if lo == 0x02:
            return "stc", f"sr, r{n}", False, None
        if lo == 0x12:
            return "stc", f"gbr, r{n}", False, None
        if lo == 0x22:
            return "stc", f"vbr, r{n}", False, None
        if lo == 0x03:
            return "bsrf", f"r{n}", True, "indirect"
        if lo == 0x23:
            return "braf", f"r{n}", True, "indirect"
        if lo == 0x0A:
            return "sts", f"mach, r{n}", False, None
        if lo == 0x1A:
            return "sts", f"macl, r{n}", False, None
        if lo == 0x2A:
            return "sts", f"pr, r{n}", False, None
        if lo == 0x29:
            return "movt", f"r{n}", False, None
        return None, "", False, None

    # ---- 0x1... mov.l Rm,@(disp,Rn) -------------------------------------
    if hi == 0x1:
        return "mov.l", f"r{m}, @({d * 4}, r{n})", False, None

    # ---- 0x2... ---------------------------------------------------------
    if hi == 0x2:
        t = {
            0x0: ("mov.b", f"r{m}, @r{n}"),
            0x1: ("mov.w", f"r{m}, @r{n}"),
            0x2: ("mov.l", f"r{m}, @r{n}"),
            0x4: ("mov.b", f"r{m}, @-r{n}"),
            0x5: ("mov.w", f"r{m}, @-r{n}"),
            0x6: ("mov.l", f"r{m}, @-r{n}"),
            0x7: ("div0s", f"r{m}, r{n}"),
            0x8: ("tst", f"r{m}, r{n}"),
            0x9: ("and", f"r{m}, r{n}"),
            0xA: ("xor", f"r{m}, r{n}"),
            0xB: ("or", f"r{m}, r{n}"),
            0xC: ("cmp/str", f"r{m}, r{n}"),
            0xD: ("xtrct", f"r{m}, r{n}"),
            0xE: ("mulu.w", f"r{m}, r{n}"),
            0xF: ("muls.w", f"r{m}, r{n}"),
        }.get(d)
        return (t[0], t[1], False, None) if t else (None, "", False, None)

    # ---- 0x3... ---------------------------------------------------------
    if hi == 0x3:
        t = {
            0x0: "cmp/eq", 0x2: "cmp/hs", 0x3: "cmp/ge", 0x4: "div1",
            0x5: "dmulu.l", 0x6: "cmp/hi", 0x7: "cmp/gt", 0x8: "sub",
            0xA: "subc", 0xB: "subv", 0xC: "add", 0xD: "dmuls.l",
            0xE: "addc", 0xF: "addv",
        }.get(d)
        return (t, f"r{m}, r{n}", False, None) if t else (None, "", False, None)

    # ---- 0x4... ---------------------------------------------------------
    if hi == 0x4:
        lo = opcode & 0xFF
        if (opcode & 0xF00F) == 0x400F:
            return "mac.w", f"@r{m}+, @r{n}+", False, None
        if lo == 0x0B:
            return "jsr", f"@r{n}", True, "indirect"
        if lo == 0x2B:
            return "jmp", f"@r{n}", True, "indirect"
        one = {
            0x00: ("shll", f"r{n}"),   0x01: ("shlr", f"r{n}"),
            0x04: ("rotl", f"r{n}"),   0x05: ("rotr", f"r{n}"),
            0x08: ("shll2", f"r{n}"),  0x09: ("shlr2", f"r{n}"),
            0x10: ("dt", f"r{n}"),     0x11: ("cmp/pz", f"r{n}"),
            0x15: ("cmp/pl", f"r{n}"), 0x18: ("shll8", f"r{n}"),
            0x19: ("shlr8", f"r{n}"),  0x1B: ("tas.b", f"@r{n}"),
            0x20: ("shal", f"r{n}"),   0x21: ("shar", f"r{n}"),
            0x24: ("rotcl", f"r{n}"),  0x25: ("rotcr", f"r{n}"),
            0x28: ("shll16", f"r{n}"), 0x29: ("shlr16", f"r{n}"),
            # NOTE: every instruction in the 0x4xxx group takes its register
            # from bits 11-8, i.e. `n` -- NOT bits 7-4. The SH-2 manual writes
            # LDS as "LDS Rm,MACH" and LDC as "LDC Rm,SR", and using `m` here
            # because of that naming is WRONG: for these encodings bits 7-4 are
            # part of the opcode, so `m` is always 0 and every one of them
            # misprints as r0. That bug silently corrupted whole disassembly
            # listings -- notably every function epilogue (lds.l @rN+,pr) and
            # every `ldc rN,gbr`.
            0x02: ("sts.l", f"mach, @-r{n}"), 0x12: ("sts.l", f"macl, @-r{n}"),
            0x22: ("sts.l", f"pr, @-r{n}"),
            0x03: ("stc.l", f"sr, @-r{n}"),   0x13: ("stc.l", f"gbr, @-r{n}"),
            0x23: ("stc.l", f"vbr, @-r{n}"),
            0x06: ("lds.l", f"@r{n}+, mach"), 0x16: ("lds.l", f"@r{n}+, macl"),
            0x26: ("lds.l", f"@r{n}+, pr"),
            0x07: ("ldc.l", f"@r{n}+, sr"),   0x17: ("ldc.l", f"@r{n}+, gbr"),
            0x27: ("ldc.l", f"@r{n}+, vbr"),
            0x0A: ("lds", f"r{n}, mach"),     0x1A: ("lds", f"r{n}, macl"),
            0x2A: ("lds", f"r{n}, pr"),
            0x0E: ("ldc", f"r{n}, sr"),       0x1E: ("ldc", f"r{n}, gbr"),
            0x2E: ("ldc", f"r{n}, vbr"),
        }.get(lo)
        return (one[0], one[1], False, None) if one else (None, "", False, None)

    # ---- 0x5... mov.l @(disp,Rm),Rn -------------------------------------
    if hi == 0x5:
        return "mov.l", f"@({d * 4}, r{m}), r{n}", False, None

    # ---- 0x6... ---------------------------------------------------------
    if hi == 0x6:
        t = {
            0x0: "mov.b", 0x1: "mov.w", 0x2: "mov.l",
        }.get(d)
        if t:
            return t, f"@r{m}, r{n}", False, None
        if d == 0x3:
            return "mov", f"r{m}, r{n}", False, None
        t = {0x4: "mov.b", 0x5: "mov.w", 0x6: "mov.l"}.get(d)
        if t:
            return t, f"@r{m}+, r{n}", False, None
        t = {
            0x7: "not", 0x8: "swap.b", 0x9: "swap.w", 0xA: "negc",
            0xB: "neg", 0xC: "extu.b", 0xD: "extu.w", 0xE: "exts.b",
            0xF: "exts.w",
        }.get(d)
        return (t, f"r{m}, r{n}", False, None) if t else (None, "", False, None)

    # ---- 0x7... add #imm,Rn ---------------------------------------------
    if hi == 0x7:
        return "add", f"#{sext8(i)}, r{n}", False, None

    # ---- 0x8... ---------------------------------------------------------
    if hi == 0x8:
        sub = (opcode >> 8) & 0xF
        if sub == 0x0:
            return "mov.b", f"r0, @({d}, r{m})", False, None
        if sub == 0x1:
            return "mov.w", f"r0, @({d * 2}, r{m})", False, None
        if sub == 0x4:
            return "mov.b", f"@({d}, r{m}), r0", False, None
        if sub == 0x5:
            return "mov.w", f"@({d * 2}, r{m}), r0", False, None
        if sub == 0x8:
            return "cmp/eq", f"#{sext8(i)}, r0", False, None
        if sub in (0x9, 0xB, 0xD, 0xF):
            target = pc + 4 + sext8(d8) * 2
            mn = {0x9: "bt", 0xB: "bf", 0xD: "bt/s", 0xF: "bf/s"}[sub]
            return mn, f"0x{target:08X}", True, target
        return None, "", False, None

    # ---- 0x9... mov.w @(disp,PC),Rn -------------------------------------
    if hi == 0x9:
        addr = pc + 4 + d8 * 2
        return "mov.w", f"@(0x{addr:08X}), r{n}", False, None

    # ---- 0xA/0xB bra / bsr ----------------------------------------------
    if hi == 0xA:
        target = pc + 4 + sext12(d12) * 2
        return "bra", f"0x{target:08X}", True, target
    if hi == 0xB:
        target = pc + 4 + sext12(d12) * 2
        return "bsr", f"0x{target:08X}", True, target

    # ---- 0xC... ---------------------------------------------------------
    if hi == 0xC:
        sub = (opcode >> 8) & 0xF
        t = {
            0x0: ("mov.b", f"r0, @({d8}, gbr)"),
            0x1: ("mov.w", f"r0, @({d8 * 2}, gbr)"),
            0x2: ("mov.l", f"r0, @({d8 * 4}, gbr)"),
            0x3: ("trapa", f"#{i}"),
            0x4: ("mov.b", f"@({d8}, gbr), r0"),
            0x5: ("mov.w", f"@({d8 * 2}, gbr), r0"),
            0x6: ("mov.l", f"@({d8 * 4}, gbr), r0"),
            0x8: ("tst", f"#{i}, r0"),
            0x9: ("and", f"#{i}, r0"),
            0xA: ("xor", f"#{i}, r0"),
            0xB: ("or", f"#{i}, r0"),
            0xC: ("tst.b", f"#{i}, @(r0, gbr)"),
            0xD: ("and.b", f"#{i}, @(r0, gbr)"),
            0xE: ("xor.b", f"#{i}, @(r0, gbr)"),
            0xF: ("or.b", f"#{i}, @(r0, gbr)"),
        }.get(sub)
        if sub == 0x7:
            addr = (pc & ~3) + 4 + d8 * 4
            return "mova", f"@(0x{addr:08X}), r0", False, None
        return (t[0], t[1], False, None) if t else (None, "", False, None)

    # ---- 0xD... mov.l @(disp,PC),Rn -------------------------------------
    if hi == 0xD:
        addr = (pc & ~3) + 4 + d8 * 4
        return "mov.l", f"@(0x{addr:08X}), r{n}", False, None

    # ---- 0xE... mov #imm,Rn ---------------------------------------------
    if hi == 0xE:
        return "mov", f"#{sext8(i)}, r{n}", False, None

    # 0xF... is the SH-4 FPU block. SH-2 has no FPU: these are invalid.
    return None, "", False, None


# ------------------------------------------------------- function finding

# sts.l pr, @-r15 -- the standard SH-2 non-leaf function prologue. Same
# encoding as SH-4, but see the endianness note at the top of this file:
# read big-endian or this constant never matches.
PROLOGUE = 0x4F22

# mov.l r14,@-r15 / mov.l r13,@-r15 ... A leaf function that calls nothing
# does not save PR, so keying only on PROLOGUE misses it. Saving a
# callee-saved register at the very top is the next-best entry signal.
CALLEE_SAVE_PUSH = {0x2FE6, 0x2FD6, 0x2FC6, 0x2FB6, 0x2FA6, 0x2F96, 0x2F86}


def find_functions(data, base_addr):
    """
    Identify function entry points and boundaries.

    Two independent signals, exactly as dcrecomp does it:
      1. BSR targets -- an address something calls is a function, full stop.
      2. Prologues -- `sts.l pr,@-r15` (and callee-saved pushes, for leaves).

    Everything found is unioned and sorted; each function runs until the next
    entry point. That over-segments (a function with an internal label that
    happens to be a BSR target elsewhere gets split), which the recompiler
    then repairs via mid-function entry wrappers.
    """
    size = len(data)
    call_targets = set()
    branch_targets = set()
    prologues = set()
    indirect_sites = []

    for off in range(0, size - 1, 2):
        opcode = struct.unpack_from('>H', data, off)[0]   # BIG-endian
        pc = base_addr + off
        mn, _, is_branch, target = decode_sh2(opcode, pc)

        if opcode == PROLOGUE or opcode in CALLEE_SAVE_PUSH:
            prologues.add(pc)

        if is_branch and isinstance(target, int):
            if mn == "bsr":
                call_targets.add(target)
            else:
                branch_targets.add(target)
        elif target == "indirect":
            indirect_sites.append((pc, mn))

    entries = sorted((call_targets | prologues | {base_addr}))
    entries = [a for a in entries if base_addr <= a < base_addr + size]

    functions = {}
    for idx, entry in enumerate(entries):
        end = entries[idx + 1] if idx + 1 < len(entries) else base_addr + size
        functions[entry] = {
            'addr': entry,
            'end': end,
            'size': end - entry,
            'has_prologue': entry in prologues,
            'is_call_target': entry in call_targets,
        }

    return {
        'functions': functions,
        'call_targets': call_targets,
        'branch_targets': branch_targets,
        'indirect_sites': indirect_sites,
    }


def analyze_binary(path, base_addr=LOAD_ADDR, limit=None):
    with open(path, 'rb') as f:
        data = f.read()
    if limit:
        data = data[:limit]

    size = len(data)
    print(f"Binary        {path}")
    print(f"Size          {size:,} bytes ({size // 1024} KB)")
    print(f"Load address  0x{base_addr:08X}")
    print(f"Range         0x{base_addr:08X} - 0x{base_addr + size:08X}")
    print(f"Endianness    big (Saturn SH-2)")

    counts = defaultdict(int)
    total = branches = unknown = 0
    for off in range(0, size - 1, 2):
        opcode = struct.unpack_from('>H', data, off)[0]
        mn, _, is_branch, _ = decode_sh2(opcode, base_addr + off)
        total += 1
        if mn is None:
            unknown += 1
        else:
            counts[mn] += 1
            if is_branch:
                branches += 1

    print(f"\n=== Instruction analysis ===")
    print(f"Halfwords decoded   {total:,}")
    print(f"Undecodable         {unknown:,} ({100.0 * unknown / max(1, total):.1f}%)")
    print(f"Branches            {branches:,}")
    print(f"\nTop 20 mnemonics:")
    for mn, c in sorted(counts.items(), key=lambda kv: -kv[1])[:20]:
        print(f"  {mn:<10} {c:>8,}")

    info = find_functions(data, base_addr)
    funcs = info['functions']
    with_pro = sum(1 for v in funcs.values() if v['has_prologue'])
    called = sum(1 for v in funcs.values() if v['is_call_target'])

    print(f"\n=== Functions ===")
    print(f"Entry points        {len(funcs):,}")
    print(f"  with prologue     {with_pro:,}")
    print(f"  BSR targets       {called:,}")
    print(f"Indirect jmp/jsr    {len(info['indirect_sites']):,}")

    return data, info


def disassemble_range(data, base_addr, start, count):
    """Print `count` instructions starting at address `start`."""
    off = start - base_addr
    for _ in range(count):
        if off < 0 or off + 1 >= len(data):
            break
        opcode = struct.unpack_from('>H', data, off)[0]
        pc = base_addr + off
        mn, ops, _, _ = decode_sh2(opcode, pc)
        text = ".invalid" if mn is None else (f"{mn} {ops}".strip())
        print(f"  {pc:08X}  {opcode:04X}  {text}")
        off += 2


def main():
    ap = argparse.ArgumentParser(description="SH-2 disassembler / function finder (Sega Saturn)")
    ap.add_argument("binary", help="raw SH-2 binary (e.g. a 1st-read file)")
    ap.add_argument("--base", default=hex(LOAD_ADDR),
                    help="load address (default 0x06004000)")
    ap.add_argument("--json", help="write the function map to this path")
    ap.add_argument("--list", nargs=2, metavar=("ADDR", "COUNT"),
                    help="disassemble COUNT instructions at ADDR")
    ap.add_argument("--limit", type=lambda v: int(v, 0),
                    help="only analyse the first N bytes")
    args = ap.parse_args()

    base = int(args.base, 0)
    data, info = analyze_binary(args.binary, base, args.limit)

    if args.list:
        addr = int(args.list[0], 0)
        count = int(args.list[1], 0)
        print(f"\n=== Disassembly at 0x{addr:08X} ===")
        disassemble_range(data, base, addr, count)

    if args.json:
        out = {
            'binary': args.binary,
            'base': base,
            'size': len(data),
            'endian': 'big',
            'functions': {f"0x{a:08X}": v for a, v in sorted(info['functions'].items())},
            'indirect_sites': [{'pc': f"0x{pc:08X}", 'op': mn}
                               for pc, mn in info['indirect_sites']],
        }
        with open(args.json, 'w') as f:
            json.dump(out, f, indent=1)
        print(f"\nWrote {args.json} ({len(info['functions'])} functions)")

    return 0


if __name__ == '__main__':
    sys.exit(main())
