#!/usr/bin/env python3
"""Oracle: disassemble every 16-bit SH-2 opcode with capstone, big-endian.

Saturn SH-2 is big-endian, so opcode 0xABCD is the byte sequence AB CD.

Output: one line per opcode value 0x0000..0xFFFF:
    XXXX\tmnemonic op_str        (decodable)
    XXXX\t.invalid               (capstone rejects)
"""
import sys
from capstone import Cs, CS_ARCH_SH, CS_MODE_SH2, CS_MODE_BIG_ENDIAN

ADDR = 0x06004000   # NiGHTS 1st-read base, so PC-relative operands look real

def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else None
    md = Cs(CS_ARCH_SH, CS_MODE_SH2 | CS_MODE_BIG_ENDIAN)
    out = []
    for op in range(0x10000):
        code = bytes([op >> 8, op & 0xFF])      # big-endian
        insns = list(md.disasm(code, ADDR))
        if len(insns) == 1 and insns[0].size == 2:
            i = insns[0]
            text = i.mnemonic if not i.op_str else f"{i.mnemonic} {i.op_str}"
            out.append(f"{op:04X}\t{text}")
        else:
            out.append(f"{op:04X}\t.invalid")
    text = "\n".join(out) + "\n"
    if out_path:
        with open(out_path, "w", newline="\n") as f:
            f.write(text)
    else:
        sys.stdout.write(text)

if __name__ == "__main__":
    main()
