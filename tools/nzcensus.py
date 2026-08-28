import sys, os, glob
base = sys.argv[1]
dirs = sorted(glob.glob(os.path.join(base, 't*')))
print(f"{'dir':6} {'ms':>8} {'wramL':>9} {'wramH':>9} {'vdp2':>8} {'vdp1':>8} {'cdb':>8}")
for d in dirs:
    t = ''
    tf = os.path.join(d,'time.txt')
    if os.path.exists(tf): t = open(tf).read().strip()
    row=[]
    for f in ('wram-lo.bin','wram-hi.bin','vdp2-vram.bin','vdp1-vram.bin','cdb-dram.bin'):
        p = os.path.join(d,f)
        if os.path.exists(p):
            b = open(p,'rb').read()
            row.append(sum(1 for x in b if x))
        else:
            row.append(-1)
    print(f"{os.path.basename(d):6} {t:>8} " + " ".join(f"{v:9d}" for v in row))
