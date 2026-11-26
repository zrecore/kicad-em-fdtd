#!/usr/bin/env python3
"""Z(f) at fixed frequencies and the deepest dip from a Touchstone RI S11 file: z_at.py RUNDIR_OR_LOG..."""
import sys, re, glob
FS = (2.20, 2.25, 2.30, 2.35, 2.40, 2.45)
def load(path):
    out = []
    for l in open(path):
        if l[0] in "!#": continue
        p = l.split()
        out.append((float(p[0]) / 1e9, complex(float(p[1]), float(p[2]))))
    return out
for a in sys.argv[1:]:
    if a.endswith(".log"):
        a = re.search(r"^results: (\S+)", open(a).read(), re.M).group(1)
    d = load(a + "/s11.s1p")
    f0, s0 = min(d, key=lambda x: abs(x[1]))
    z0 = 50 * (1 + s0) / (1 - s0)
    print(a.split("/")[-3] if False else a[-30:], "dip %.2f dB at %.4f GHz Z %.1f%+.1fj" % (20 * __import__("math").log10(abs(s0)), f0, z0.real, z0.imag))
    for t in FS:
        f, s = min(d, key=lambda x: abs(x[0] - t)); z = 50 * (1 + s) / (1 - s)
        print("   %.3f GHz Z=%.1f%+.1fj" % (f, z.real, z.imag))
