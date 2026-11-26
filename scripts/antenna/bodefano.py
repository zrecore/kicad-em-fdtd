#!/usr/bin/env python3
"""Numerical demonstration of the Bode-Fano limit for a parallel RC load (needs numpy).

  python3 scripts/antenna/bodefano.py [--orders 0,1,2,3,4,5,6]

The load is R = 1, C = 1 (so the bound is pi: the area under ln(1/|Gamma|) over all angular frequency cannot exceed
pi / (RC) for any lossless matching network, Fano's statement of Bode's result, MIT RLE TR-41, read in full; the
journal version is unread). Two things are shown. (1) The area over all frequency is pi exactly for a reflection
coefficient with no right-half-plane zeros: a bare resistor R0 = R already has it, so a network cannot increase the
area, only move it. (2) The use of that area: over a band 0..wc the best a network can do is
wc * ln(1/|Gamma|max) <= pi. Lossless ladders of n elements (series L first from the load, then shunt C,
alternating) between the load and a source resistance R0 are optimised for the smallest worst |Gamma| over the band;
the product approaches pi as n grows and never exceeds it. A demonstration, not a proof (docs/methods/bode-fano.md).
"""
import math
import sys

import numpy as np

W = np.concatenate([np.linspace(1e-4, 8, 16000), np.linspace(8, 400, 12000)[1:]])  # rad/s, R = C = 1
WC = 1.0  # the band 0..WC
BAND = np.linspace(1e-3, WC, 400)


def gamma_of(params, n, w):
    r0 = min(max(abs(params[0]), 0.1), 10.0)
    z = 1.0 / (1.0 + 1j * w)  # R || C
    for k in range(n):
        v = min(max(abs(params[1 + k]), 0.02), 20.0)
        if k % 2 == 0:
            z = z + 1j * w * v  # series L
        else:
            z = 1.0 / (1.0 / z + 1j * w * v)  # shunt C
    return (z - r0) / (z + r0)


def total_area(params, n):
    mag = np.abs(gamma_of(params, n, W))
    return float(np.trapezoid(np.log(1.0 / np.maximum(mag, 1e-9)), W))


def band_worst(params, n):
    return float(np.max(np.abs(gamma_of(params, n, BAND))))


def nelder_mead(f, x0, iters=1200, step=0.3):
    n = len(x0)
    pts = [np.array(x0, float)] + [np.array(x0, float) + step * np.eye(n)[i] for i in range(n)]
    vals = [f(p) for p in pts]
    for _ in range(iters):
        order = np.argsort(vals)
        pts = [pts[i] for i in order]
        vals = [vals[i] for i in order]
        cen = np.mean(pts[:-1], axis=0)
        xr = cen + (cen - pts[-1])
        fr = f(xr)
        if fr < vals[0]:
            xe = cen + 2 * (cen - pts[-1])
            fe = f(xe)
            pts[-1], vals[-1] = (xe, fe) if fe < fr else (xr, fr)
        elif fr < vals[-2]:
            pts[-1], vals[-1] = xr, fr
        else:
            xc = cen + 0.5 * (pts[-1] - cen)
            fc = f(xc)
            if fc < vals[-1]:
                pts[-1], vals[-1] = xc, fc
            else:
                pts = [pts[0]] + [pts[0] + 0.5 * (p - pts[0]) for p in pts[1:]]
                vals = [vals[0]] + [f(p) for p in pts[1:]]
    i = int(np.argmin(vals))
    return pts[i], vals[i]


def best(n, tries=10, seed=0):
    """Smallest worst |Gamma| over the band, by minimising a high p-norm and keeping the best true maximum."""
    rng = np.random.default_rng(seed)
    top = (None, 1.0)
    for _ in range(tries):
        x0 = rng.uniform(0.3, 2.5, 1 + n)

        def cost(p):
            g = np.abs(gamma_of(p, n, BAND))
            return float(np.mean(g**30) ** (1 / 30))

        x, _ = nelder_mead(cost, x0)
        worst = band_worst(x, n)
        if worst < top[1]:
            top = (x, worst)
    return top


def main():
    orders = [0, 1, 2, 3, 4, 5, 6]
    if "--orders" in sys.argv:
        orders = [int(v) for v in sys.argv[sys.argv.index("--orders") + 1].split(",")]
    print("bound pi/(RC) = %.4f; the band is 0 to %.1f rad/s" % (math.pi, WC))
    direct = total_area([1.0], 0)
    print("a bare resistor R0 = R: area over all frequency %.4f = %.2f percent of pi (equality for a minimum-phase "
          "reflection coefficient)" % (direct, 100 * direct / math.pi))
    rng = np.random.default_rng(7)
    worst = 0.0
    for _ in range(3000):  # no network exceeds the bound
        n = int(rng.integers(0, 7))
        worst = max(worst, total_area(rng.uniform(0.2, 5.0, 1 + n), n))
    print("largest total area over 3000 random networks: %.4f (%.2f percent of pi)" % (worst, 100 * worst / math.pi))
    print("order: worst |Gamma| in the band, wc * ln(1/worst) as a percent of pi, total area over all frequency")
    for n in orders:
        p, w = best(n)
        prod = WC * math.log(1.0 / w)
        print("  %d: %.4f (%.1f dB), %.1f percent of pi, %.4f; R0 = %.3f, elements %s"
              % (n, w, 20 * math.log10(w), 100 * prod / math.pi, total_area(p, n), min(max(abs(p[0]), 0.1), 10),
                 np.round([min(max(abs(v), 0.02), 20) for v in p[1:]], 3).tolist()))


if __name__ == "__main__":
    main()
