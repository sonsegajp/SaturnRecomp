#!/usr/bin/env python3
"""Generate a games/<prefix>/game.toml from a Saturn disc, inferring everything.

Every field we hand-wrote per game is actually on the disc: IP.BIN carries the
product number, area codes, title and the first-read load/entry address, and
the ISO9660 root directory names the first-read file. Reading them means a new
game is "drop in the disc and go" instead of a hand-authored config.

    python tools/mkgame.py "<path to .cue>" [--prefix NAME] [--write]

Without --write it prints the toml and what it inferred. Track 1 is assumed to
be the data track (true for every Saturn disc: the boot sector must be there).
"""
import os, re, sys, argparse

SECT_USER = 2048

def cue_track1(cue_path):
    """Return (bin_path, sector_size) for track 1.

    Accepts a .cue, or a bare .iso/.bin/.img -- a single-file rip has no cue
    and is always the data track, so there is nothing to parse."""
    if not cue_path.lower().endswith(".cue"):
        p = os.path.abspath(cue_path)
        sz = os.path.getsize(p)
        # 2352 only if it divides evenly AND the sync mark is present; some
        # 2048-byte images also happen to be a multiple of 2352.
        if sz % 2352 == 0:
            with open(p, "rb") as f:
                if f.read(12) == b"\x00" + b"\xff" * 10 + b"\x00":
                    return p, 2352
        return p, 2048
    base = os.path.dirname(os.path.abspath(cue_path))
    txt = open(cue_path, encoding="utf-8", errors="replace").read()
    cur = None
    for line in txt.splitlines():
        m = re.match(r'\s*FILE\s+"([^"]+)"', line)
        if m:
            cur = os.path.join(base, m.group(1))
            continue
        m = re.match(r'\s*TRACK\s+0*1\s+(\S+)', line)
        if m and cur:
            mode = m.group(1).upper()
            return cur, (2352 if "2352" in mode or "AUDIO" in mode else 2048)
    # No explicit TRACK 01 line: fall back to the first FILE.
    m = re.search(r'FILE\s+"([^"]+)"', txt)
    if not m:
        sys.exit("no FILE entry in cue")
    p = os.path.join(base, m.group(1))
    return p, (2352 if os.path.getsize(p) % 2352 == 0 else 2048)

class Img:
    def __init__(self, path, sect):
        self.f = open(path, "rb"); self.sect = sect
    def sector(self, lba):
        self.f.seek(lba * self.sect)
        d = self.f.read(self.sect)
        return d[16:16 + SECT_USER] if self.sect == 2352 else d

def read_ip(img):
    """IP.BIN lives in the first sectors; sector 0 holds the header."""
    ip = img.sector(0)
    if not ip.startswith(b"SEGA SEGASATURN"):
        sys.exit("sector 0 is not a Saturn IP.BIN (got %r)" % ip[:16])
    txt = lambda a, n: ip[a:a + n].decode("ascii", "replace").strip()
    u32 = lambda a: int.from_bytes(ip[a:a + 4], "big")
    return {
        "maker":    txt(0x10, 16),
        "product":  txt(0x20, 10),
        "version":  txt(0x2A, 6),
        "date":     txt(0x30, 8),
        "device":   txt(0x38, 8),
        "areas":    txt(0x40, 10),
        "periph":   txt(0x50, 16),
        "title":    " ".join(txt(0x60, 112).split()),
        "stack_m":  u32(0xE8),
        "stack_s":  u32(0xEC),
        "load":     u32(0xF0),   # 1st-read load address
        "size":     u32(0xF4),   # 1st-read size
    }

def first_read_name(img):
    """The 1st-read file is the first real entry in the ISO9660 root dir."""
    pvd = img.sector(16)
    if pvd[1:6] != b"CD001":
        return None
    root = pvd[156:156 + 34]
    rlba = int.from_bytes(root[2:6], "little")
    rlen = int.from_bytes(root[10:14], "little")
    data = b"".join(img.sector(rlba + i) for i in range((rlen + SECT_USER - 1) // SECT_USER))
    i = 0
    files = []
    while i < len(data):
        L = data[i]
        if L == 0:
            i = (i // SECT_USER + 1) * SECT_USER
            continue
        nl = data[i + 32]
        nm = data[i + 33:i + 33 + nl]
        flags = data[i + 25]
        lba = int.from_bytes(data[i + 2:i + 6], "little")
        if nl > 1 and not (flags & 0x02):          # skip "." ".." and dirs
            files.append((lba, nm.split(b";")[0].decode("ascii", "replace")))
        i += L
    if not files:
        return None
    # DIRECTORY ORDER, not LBA order. The IPL loads the first entry in the root
    # directory, and ISO9660 requires those entries to be name-sorted, so this
    # is what the hardware actually does. Sorting by LBA instead looks tidier
    # and is WRONG: Fighting Vipers puts SSFV_CPY.TXT at a lower LBA than
    # AAFV.BIN, so an LBA sort boots the copyright notice.
    return files[0][1]

# IP.BIN area letters -> the BIOS we ship. Order matters: prefer the region
# whose BIOS we actually have.
BIOS = [("U", "saturn_101a_us.bin"), ("J", "saturn_101_jp.bin"),
        ("T", "saturn_101a_us.bin"), ("B", "saturn_101a_us.bin"),
        ("E", "saturn_101_jp.bin"), ("K", "saturn_101_jp.bin"),
        ("A", "saturn_101_jp.bin"), ("L", "saturn_101_jp.bin")]

def pick_bios(areas, root):
    have = lambda b: os.path.exists(os.path.join(root, "bios", b))
    for letter, b in BIOS:
        if letter in areas and have(b):
            return b
    for _, b in BIOS:
        if have(b):
            return b
    return "saturn_101a_us.bin"

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cue")
    ap.add_argument("--prefix")
    ap.add_argument("--write", action="store_true")
    a = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    binp, sect = cue_track1(a.cue)
    img = Img(binp, sect)
    ip = read_ip(img)
    fr = first_read_name(img)
    if not fr:
        sys.exit("could not read the ISO9660 root directory")

    prefix = a.prefix or re.sub(r"[^a-z0-9]+", "", ip["title"].lower())[:12] or "game"
    bios = pick_bios(ip["areas"], root)
    load = ip["load"] or 0x06004000

    print("--- inferred from the disc ---")
    for k in ("title", "product", "version", "date", "areas", "periph"):
        print(f"  {k:9s} {ip[k]}")
    print(f"  1st-read  /{fr}  ({ip['size']} bytes)")
    print(f"  load/entry 0x{load:08X}   stack M 0x{ip['stack_m']:08X} S 0x{ip['stack_s']:08X}")
    print(f"  track1    {os.path.basename(binp)} ({sect}-byte sectors)")
    print(f"  bios      {bios}")

    toml = f'''# {ip["title"]} -- generated by tools/mkgame.py from the disc.
# Every value below is read out of IP.BIN and the ISO9660 root directory.

[game]
name       = "{ip["title"]}"
prefix     = "{prefix}"
product_no = "{ip["product"]}"
disc       = "{a.cue.replace(os.sep, "/")}"
bios       = "../../bios/{bios}"

[[module]]
name        = "main"
file        = "/{fr}"
cpu         = "sh2"
compression = "none"
load_addr   = 0x{load:08X}
entry       = 0x{load:08X}
first_read  = true
'''
    outdir = os.path.join(root, "games", prefix)
    outp = os.path.join(outdir, "game.toml")
    if a.write:
        os.makedirs(outdir, exist_ok=True)
        open(outp, "w", newline="\n").write(toml)
        print(f"\nwrote {outp}")
    else:
        print("\n" + toml)

main()
