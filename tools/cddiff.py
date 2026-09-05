"""Diff our CD block command/response stream against a Ymir devlog trace.

Ours:  SATURN_CDLOG=1 runner output.
Ymir:  ymtrace stdout (see apps/ymtrace in the Ymir source tree).

Only the ROM boot chain is compared by default: the BIOS runs a second,
RAM-resident task (pc=0x0602xxxx) that polls GetStatus on its own schedule, and
interleaving it with the boot chain makes every comparison diverge at the first
poll for no real reason. Pass --all to compare everything.

usage: cddiff.py <ours.txt> <ymir.txt> [--all] [--context N]
"""
import re
import sys


def ours(path, rom_only=True):
    seq, pend = [], None
    cmd_re = re.compile(
        r"\[cd>\] op=([0-9A-F]{2})\s+([0-9A-F]{4}) ([0-9A-F]{4}) "
        r"([0-9A-F]{4}) ([0-9A-F]{4}).*pc=([0-9A-F]{8})")
    rsp_re = re.compile(
        r"\[cd<\] ([0-9A-F]{4}) ([0-9A-F]{4}) ([0-9A-F]{4}) ([0-9A-F]{4})")
    for ln in open(path, encoding="utf-8", errors="replace"):
        m = cmd_re.match(ln)
        if m:
            pend = (" ".join(m.groups()[1:5]), m.group(6))
            continue
        m = rsp_re.match(ln)
        if m and pend:
            # A reply belongs to the command that preceded it; later [cd<]
            # lines with no [cd>] are unsolicited periodic reports, skipped.
            if not rom_only or pend[1].startswith("0000"):
                seq.append((pend[0], " ".join(m.groups())))
            pend = None
    return seq


def ymir(path):
    seq, pend = [], None
    cmd_re = re.compile(r"Processing command "
                        r"([0-9A-F]{4}) ([0-9A-F]{4}) ([0-9A-F]{4}) ([0-9A-F]{4})")
    rsp_re = re.compile(r"Command response:\s+"
                        r"([0-9A-F]{4}) ([0-9A-F]{4}) ([0-9A-F]{4}) ([0-9A-F]{4})")
    for ln in open(path, encoding="utf-8", errors="replace"):
        m = cmd_re.search(ln)
        if m:
            pend = " ".join(m.groups())
            continue
        m = rsp_re.search(ln)
        if m and pend:
            seq.append((pend, " ".join(m.groups())))
            pend = None
    return seq


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    rom_only = "--all" not in sys.argv
    ctx = 3
    for a in sys.argv:
        if a.startswith("--context"):
            ctx = int(a.split("=")[1])
    a, b = ours(args[0], rom_only), ymir(args[1])
    scope = "ROM boot chain" if rom_only else "all commands"
    print("%s: ours %d exchanges, ymir %d" % (scope, len(a), len(b)))
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            print("\nfirst divergence at exchange %d" % i)
            for j in range(max(0, i - ctx), min(n, i + ctx)):
                mark = "*" if j == i else " "
                print("%s#%-3d ours %s -> %s" % (mark, j, a[j][0], a[j][1]))
                print("      ymir %s -> %s" % (b[j][0], b[j][1]))
            return
    print("identical for all %d compared exchanges" % n)


if __name__ == "__main__":
    main()
