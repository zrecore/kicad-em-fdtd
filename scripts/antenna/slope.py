#!/usr/bin/env python3
"""The reactance slope of an antenna's impedance, defined once, so two runs (or two codes) are compared like for
like.  slope.py RUNDIR_OR_S1P_OR_LOG... [--window F0 F1] [--half-width MHZ]

For every run: the series resonance (reactance zero crossing nearest the |S11| minimum, linear interpolation),
R there, and dX/df in ohm per 50 MHz measured two ways: (a) over a fixed window (default 2.30 to 2.35 GHz, the
window the PLAN used) by a least-squares line, and (b) over +-half-width (default 25 MHz) around the run's OWN
series resonance.  (a) compares different resonance frequencies at the same frequencies; (b) compares the
property of the resonance.  The loaded Q of an ideal series resonance is f0 (dX/df) / (2 R), printed as q_eff.
Also the -10 dB band around the dip.  Standard library only (touchstone.py is its reader).
"""

import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import touchstone as ts  # noqa: E402

FIXED_WINDOW = (2.30e9, 2.35e9)
HALF_WIDTH = 25e6


def load(path):
    """(freqs, S11) from a run folder, an .s1p file or a solver log (its 'results:' line)."""
    if path.endswith(".log"):
        m = re.search(r"^results: (\S+)", open(path).read(), re.M)
        if not m:
            raise ValueError(f"{path}: no 'results:' line")
        path = m.group(1)
    if os.path.isdir(path):
        path = os.path.join(path, "s11.s1p")
    freqs, s11, _ = ts.parse(open(path).read())[:3]
    return freqs, s11


def z_of(s):
    return 50.0 * (1 + s) / (1 - s)


def line_slope(f, x, lo, hi):
    """Least-squares dX/df over lo <= f <= hi, in ohm per Hz; None with fewer than 3 points."""
    pts = [(a, b) for a, b in zip(f, x) if lo <= a <= hi]
    if len(pts) < 3:
        return None
    n = len(pts)
    mf, mx = sum(a for a, _ in pts) / n, sum(b for _, b in pts) / n
    den = sum((a - mf) ** 2 for a, _ in pts)
    return sum((a - mf) * (b - mx) for a, b in pts) / den if den else None


def analyze(freqs, s11, window=FIXED_WINDOW, half_width=HALF_WIDTH):
    z = [z_of(s) for s in s11]
    x = [v.imag for v in z]
    k = min(range(len(s11)), key=lambda i: abs(s11[i]))
    fdip = freqs[k]
    fx = None
    for i in range(max(0, k - 60), min(len(z) - 1, k + 60)):
        if x[i] < 0 <= x[i + 1]:
            fz = freqs[i] + (freqs[i + 1] - freqs[i]) * (-x[i]) / (x[i + 1] - x[i])
            if fx is None or abs(fz - fdip) < abs(fx - fdip):
                fx = fz
    out = {"dip_hz": fdip, "dip_db": 20 * math.log10(abs(s11[k])), "series_hz": fx}
    if fx is not None:
        j = min(range(len(freqs)), key=lambda i: abs(freqs[i] - fx))
        out["r_ohm"] = z[j].real
    s_fixed = line_slope(freqs, x, *window)
    out["slope_fixed"] = None if s_fixed is None else s_fixed * 50e6
    f0 = fx if fx is not None else fdip
    s_own = line_slope(freqs, x, f0 - half_width, f0 + half_width)
    out["slope_own"] = None if s_own is None else s_own * 50e6
    if s_own is not None and "r_ohm" in out and out["r_ohm"] > 0:
        out["q_eff"] = f0 * s_own / (2 * out["r_ohm"])
    lo = hi = None
    for i in range(k, 0, -1):
        if 20 * math.log10(abs(s11[i])) > -10:
            lo = freqs[i]
            break
    for i in range(k, len(s11)):
        if 20 * math.log10(abs(s11[i])) > -10:
            hi = freqs[i]
            break
    out["band10_hz"] = (lo, hi) if lo and hi else None
    return out


def fmt(v, spec):
    return "-" if v is None else spec % v


def main(argv):
    window, half = FIXED_WINDOW, HALF_WIDTH
    paths = []
    it = iter(argv)
    for a in it:
        if a == "--window":
            window = (float(next(it)) * 1e9, float(next(it)) * 1e9)
        elif a == "--half-width":
            half = float(next(it)) * 1e6
        else:
            paths.append(a)
    print(f"{'run':<22}{'series GHz':>11}{'R ohm':>8}{'dip dB':>8}{'slope fixed':>13}{'slope own':>11}{'q_eff':>7}  -10 dB band GHz")
    for p in paths:
        name = os.path.basename(p.rstrip("/")) or p
        try:
            r = analyze(*load(p), window=window, half_width=half)
        except (OSError, ValueError) as e:
            print(f"{name:<22}{e}")
            continue
        band = r["band10_hz"]
        print(
            f"{name:<22}{fmt(r['series_hz'] and r['series_hz'] / 1e9, '%.4f'):>11}{fmt(r.get('r_ohm'), '%.1f'):>8}"
            f"{r['dip_db']:>8.1f}{fmt(r['slope_fixed'], '%.1f'):>13}{fmt(r['slope_own'], '%.1f'):>11}"
            f"{fmt(r.get('q_eff'), '%.1f'):>7}  "
            + (f"{band[0] / 1e9:.3f} to {band[1] / 1e9:.3f}" if band else "-")
        )
    print(f"(slope: ohm per 50 MHz; fixed window {window[0] / 1e9:.2f} to {window[1] / 1e9:.2f} GHz; own = +-{half / 1e6:g} MHz around the series resonance)")


if __name__ == "__main__":
    main(sys.argv[1:])
