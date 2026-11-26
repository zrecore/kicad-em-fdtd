#!/usr/bin/env python3
"""Sub-bin resonance estimates from a Touchstone RI S11 file: the reactance zero crossing nearest the dip
(linear interpolation between the two bins that bracket it) and a parabola through the three dB points around
the |S11| minimum. subbin.py LOG_OR_RUNDIR... (the dip bin spacing is the frequency resolution, about 8 MHz)."""
import math
import re
import sys


def load(path):
    out = []
    for line in open(path):
        if line[0] in "!#":
            continue
        p = line.split()
        out.append((float(p[0]) / 1e9, complex(float(p[1]), float(p[2]))))
    return out


for a in sys.argv[1:]:
    name = a.split("/")[-1]
    if a.endswith(".log"):
        a = re.search(r"^results: (\S+)", open(a).read(), re.M).group(1)
    d = load(a + "/s11.s1p")
    k = min(range(len(d)), key=lambda i: abs(d[i][1]))
    db = [20 * math.log10(abs(s)) for _, s in d]
    # parabola through (k-1, k, k+1)
    y0, y1, y2 = db[k - 1], db[k], db[k + 1]
    den = y0 - 2 * y1 + y2
    shift = 0.5 * (y0 - y2) / den if den else 0.0
    df = d[k + 1][0] - d[k][0]
    fpar = d[k][0] + shift * df
    # reactance zero crossing nearest the dip
    zs = [(f, 50 * (1 + s) / (1 - s)) for f, s in d]
    fx = None
    for i in range(max(1, k - 40), min(len(zs) - 1, k + 40)):
        (fa, za), (fb, zb) = zs[i], zs[i + 1]
        if za.imag < 0 <= zb.imag:
            fz = fa + (fb - fa) * (-za.imag) / (zb.imag - za.imag)
            if fx is None or abs(fz - d[k][0]) < abs(fx - d[k][0]):
                fx = fz
    print(
        "%-14s dip bin %.4f  parabola %.4f GHz  series res %s  (bin %.1f MHz)"
        % (name, d[k][0], fpar, ("%.4f" % fx) if fx else "none", df * 1e3)
    )
