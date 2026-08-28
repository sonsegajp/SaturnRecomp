"""Decoder conformance test: our SH-2 decoder vs capstone CS_MODE_SH2 (big-endian).

PASS requires:
  1. Both dumps cover the full 16-bit opcode space (guards against a silent
     empty-input pass -- a vacuous green is worse than no test).
  2. Every encoding that is real SH-2 decodes to identical text.
  3. Every encoding on the lenient allowlist: capstone accepts, we reject.
  4. No unlisted divergence in either direction.
"""
import sys

SPACE = 0x10000


def load(path):
    with open(path, encoding="utf-8-sig") as f:
        rows = {}
        for line in f:
            line = line.rstrip("\r\n")
            if not line:
                continue
            k, _, v = line.partition("\t")
            rows[k] = v
        return rows


oracle = load("tests/oracle_sh2.txt")
ours = load("tests/ours_sh2.txt")

problems = []
if len(oracle) != SPACE:
    problems.append(f"oracle dump has {len(oracle)} entries, expected {SPACE}")
if len(ours) != SPACE:
    problems.append(f"our dump has {len(ours)} entries, expected {SPACE}")
if problems:
    for p in problems:
        print("FATAL " + p)
    sys.exit(1)

allow = {}
with open("tests/oracle/capstone_lenient.txt", encoding="utf-8-sig") as f:
    for line in f:
        if line.startswith("#"):
            continue
        line = line.rstrip("\r\n")
        if not line:
            continue
        k, _, v = line.partition("\t")
        allow[k] = v

fail = matched = lenient = both_invalid = 0
unused_allow = set(allow)

for i in range(SPACE):
    k = f"{i:04X}"
    ot, mt = oracle[k], ours[k]
    if ot == mt:
        if ot == ".invalid":
            both_invalid += 1
        else:
            matched += 1
        continue
    if allow.get(k) == ot and mt == ".invalid":
        lenient += 1
        unused_allow.discard(k)
        continue
    print(f"FAIL {k}: capstone={ot!r} ours={mt!r}")
    fail += 1

# A stale allowlist entry means the decoder changed behaviour silently.
for k in sorted(unused_allow):
    print(f"FAIL {k}: allowlisted as capstone-lenient but no longer diverges")
    fail += 1

print(f"opcode space     : {SPACE}")
print(f"identical        : {matched}")
print(f"both reject      : {both_invalid}")
print(f"capstone-lenient : {lenient}  (SH-2A/SH-4A encodings we correctly reject)")
print(f"FAILURES         : {fail}")

if matched + both_invalid + lenient + fail != SPACE:
    print("FATAL accounting mismatch")
    sys.exit(1)

sys.exit(1 if fail else 0)
