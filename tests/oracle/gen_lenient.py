"""Record the encodings where capstone CS_MODE_SH2 accepts a non-SH-2
instruction (SH-2A / SH-4A leniency). These are the ONLY permitted
divergences: for each, capstone decodes and we must reject."""
o = [l.rstrip("\r\n") for l in open("tests/oracle_sh2.txt",encoding="utf-8-sig") if l.strip()]
m = [l.rstrip("\r\n") for l in open("tests/ours_sh2.txt",encoding="utf-8-sig") if l.strip()]
with open("tests/oracle/capstone_lenient.txt","w") as f:
    f.write("# capstone CS_MODE_SH2 accepts these non-SH-2 encodings; sh2_match() must reject each.\n")
    n=0
    for a,b in zip(o,m):
        oa=a.split("\t",1); mb=b.split("\t",1)
        if oa[1]!=mb[1]:
            assert mb[1]==".invalid", f"real disagreement at {oa[0]}: {oa[1]} vs {mb[1]}"
            f.write(f"{oa[0]}\t{oa[1]}\n"); n+=1
print(f"recorded {n} lenient encodings")
