#!/usr/bin/env python3
"""Phase of S11 extrapolated to zero frequency, for a known-load control.

Over a lossless line, S11 = G * exp(-j*2*pi*f*tau) with G = +1 for an open end and -1 for a short, whatever the line
length. Fitting the unwrapped phase to a line and reading its value at f = 0 therefore gives ~0 degrees (open) or
~180 degrees (short) and the slope gives the electrical length (tau). A wrong answer means the model, not the
device under test, is broken.

Usage: phase_fit.py [--band 2.2 2.8] PATH [PATH ...]   (GHz; each PATH is an .s1p, a run folder with s11.s1p, or a
project folder with em-fdtd/results/run-*/s11.s1p; the newest run is used)
"""
import cmath
import glob
import math
import pathlib
import sys


def find(path):
    p = pathlib.Path(path)
    if p.is_file():
        return p
    runs = sorted(glob.glob(str(p / "s11.s1p"))) or sorted(glob.glob(str(p / "em-fdtd/results/run-*/s11.s1p")))
    if not runs:
        raise SystemExit(f"{path}: no s11.s1p")
    return pathlib.Path(runs[-1])


def fit(path, lo, hi):
    xs, ph = [], []
    for line in open(find(path)):
        if not line.strip() or line[0] in "!#":
            continue
        a = line.split()
        f = float(a[0]) / 1e9
        if lo <= f <= hi:
            xs.append(f)
            ph.append(cmath.phase(complex(float(a[1]), float(a[2]))))
    if len(xs) < 3:
        raise SystemExit(f"{path}: fewer than 3 points in {lo}..{hi} GHz")
    un = [ph[0]]
    for p in ph[1:]:
        while p - un[-1] > math.pi:
            p -= 2 * math.pi
        while p - un[-1] < -math.pi:
            p += 2 * math.pi
        un.append(p)
    n = len(xs)
    mx, mp = sum(xs) / n, sum(un) / n
    slope = sum((x - mx) * (p - mp) for x, p in zip(xs, un)) / sum((x - mx) ** 2 for x in xs)
    return math.degrees(slope), math.degrees(mp - slope * mx) % 360


def main():
    args = sys.argv[1:]
    lo, hi = 2.2, 2.8
    if args[:1] == ["--band"]:
        lo, hi, args = float(args[1]), float(args[2]), args[3:]
    for a in args:
        slope, phi0 = fit(a, lo, hi)
        kind = "open" if min(phi0, 360 - phi0) < 45 else "short" if abs(phi0 - 180) < 45 else "neither"
        print(f"{a:40s} slope {slope:7.1f} deg/GHz  phase at f=0 {phi0:6.1f} deg  reads as: {kind}")


if __name__ == "__main__":
    main()
