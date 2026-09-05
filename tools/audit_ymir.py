#!/usr/bin/env python3
"""Diff our register maps against Ymir's, subsystem by subsystem.

Ymir documents every register in a header comment shaped like

    // 104  R/W  32       ud        DVDNT   Dividend register L for 32-bit ...
    // 1800AC  W    16      ud   PRISA   Priority number A

so the offset and mnemonic can be lifted mechanically. We name the same
registers in our own sources, either as `#define NAME 0x...` or in a comment
`/* NAME */` beside the offset. This prints, per subsystem: registers whose
offsets DISAGREE, registers Ymir documents that we never mention, and the
handful we mention that Ymir does not.

    Set YMIR_CORE to the Ymir checkout libs/ymir-core directory.
    python tools/audit_ymir.py [subsystem ...]
"""
import os, re, sys, collections

YMIR = os.environ.get("YMIR_CORE", "")
if not os.path.isdir(os.path.join(YMIR, "include", "ymir")):
    sys.exit("Set YMIR_CORE to a Ymir checkout libs/ymir-core directory.")
OURS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "runner")

# subsystem -> (ymir include subdir, our source files)
UNITS = {
    "sh2":     ("hw/sh2",     ["src/sh2_interp.c", "src/bus.c"]),
    "vdp1":    ("hw/vdp",     ["src/vdp1.c"]),
    "vdp2":    ("hw/vdp",     ["src/vdp2.c"]),
    "scu":     ("hw/scu",     ["src/bus.c"]),
    "smpc":    ("hw/smpc",    ["src/smpc.c"]),
    "scsp":    ("hw/scsp",    ["src/scsp.c", "src/sound.c"]),
    "cdblock": ("hw/cdblock", ["src/cdblock.c"]),
    "m68k":    ("hw/m68k",    ["src/m68k.c", "src/m68k_bus.c"]),
}

# `// <hex>  <rw>  <sizes>  <init>  <NAME>  <description>`
RE_YMIR = re.compile(
    r"^\s*//\s*([0-9A-Fa-f]{2,6})\s+[RW/]{1,3}\s+[0-9,]+\s+\S+\s+([A-Z][A-Z0-9_]{1,9})\b")

def ymir_regs(subdir):
    regs = {}
    base = os.path.join(YMIR, "include/ymir", subdir)
    for root, _, files in os.walk(base):
        for fn in files:
            if not fn.endswith((".hpp", ".h")):
                continue
            for line in open(os.path.join(root, fn), encoding="utf-8", errors="replace"):
                m = RE_YMIR.match(line)
                if m:
                    off, name = int(m.group(1), 16), m.group(2)
                    # first definition wins; later files repeat mirrors
                    regs.setdefault(name, (off, fn))
    return regs

def our_regs(files):
    regs = collections.defaultdict(set)
    for rel in files:
        p = os.path.join(OURS, rel)
        if not os.path.exists(p):
            continue
        txt = open(p, encoding="utf-8", errors="replace").read()
        # #define NAME 0x1234   /  NAME = 0x1234
        for m in re.finditer(r"#define\s+([A-Z][A-Z0-9_]{1,9})\s+\(?0x([0-9A-Fa-f]+)", txt):
            regs[m.group(1)].add(int(m.group(2), 16))
        # 0x1234  ...  /* NAME */   and   NAME  FF80   (table comments)
        for m in re.finditer(r"([A-Z][A-Z0-9_]{2,9})\s+(?:=\s*)?0x([0-9A-Fa-f]{2,6})", txt):
            regs[m.group(1)].add(int(m.group(2), 16))
        for m in re.finditer(r"\b([A-Z][A-Z0-9_]{2,9})\s+([0-9A-F]{3,4})\b", txt):
            regs[m.group(1)].add(int(m.group(2), 16))
    return regs

def norm(off):
    """Compare on low bits: we write absolute addresses, Ymir writes offsets."""
    return off & 0xFFFF

def main():
    want = sys.argv[1:] or list(UNITS)
    grand = collections.Counter()
    for unit in want:
        subdir, files = UNITS[unit]
        y = ymir_regs(subdir)
        o = our_regs(files)
        if not y:
            print(f"\n### {unit}: no documented registers found in {subdir}")
            continue
        mismatch, missing = [], []
        for name, (off, fn) in sorted(y.items(), key=lambda kv: kv[1][0]):
            if name not in o:
                missing.append((off, name, fn))
            elif not any(norm(v) == norm(off) or norm(v) == norm(off) & 0x1FF
                         or (norm(v) & 0x1FF) == norm(off) for v in o[name]):
                mismatch.append((off, name, sorted(hex(v) for v in o[name]), fn))
        print(f"\n### {unit}: {len(y)} documented, "
              f"{len(mismatch)} MISMATCH, {len(missing)} not referenced by us")
        for off, name, ours, fn in mismatch:
            print(f"  MISMATCH {name:9s} ymir 0x{off:04X}  ours {','.join(ours)}   [{fn}]")
        grand[unit] = len(mismatch)
        if missing:
            print(f"  not referenced: " +
                  " ".join(f"{n}@{o:X}" for o, n, _ in missing[:40]) +
                  (" ..." if len(missing) > 40 else ""))
    print("\n=== mismatch totals ===")
    for k, v in grand.items():
        print(f"  {k:8s} {v}")

main()
