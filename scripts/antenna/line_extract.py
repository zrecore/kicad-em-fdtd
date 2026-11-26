#!/usr/bin/env python3
"""Effective permittivity (with a dispersion slope) and open-end extension of a microstrip line from S11 of the same
open-ended line at two lengths, solved on the same board, port and mesh (needs numpy).

  python3 scripts/antenna/line_extract.py KEY_SHORT KEY_LONG LEN_SHORT_MM LEN_LONG_MM [--fmin GHZ] [--fmax GHZ]
      KEY_* are names in out/lines-check/last.json (validate_lines.py writes them: case name + --tag).

Method (textbook line-line idea, derived here; not a reproduction of any paper's method): a lossless uniform line of
length L, characteristic impedance Z0 and phase constant beta(f) = 2 pi f sqrt(eeff(f)) / c, open at the far end, has
Zin = -j Z0 cot(beta (L + d)), where d is the open end's equivalent extra length. The port adds a series inductance
Lp. S11 = (Zin + j w Lp - 50) / (Zin + j w Lp + 50). The two lines share Z0, eeff(f) = e0 + e1 f/GHz, d and Lp; only
L differs, so the port, the end and any reference-plane error are common to both and the length difference fixes
beta. All five are fitted to the complex S11 of both lines at once (Levenberg-Marquardt, restarts). The estimate's
spread is read from the Jacobian, a lower bound on its uncertainty (it ignores the mesh).
"""
import json
import math
import os
import sys

import numpy as np

C0 = 299792458.0
ZREF = 50.0


def gamma(p, f, length_mm):
    z0, e0, e1, d_mm, lp_nh = p
    w = 2 * math.pi * f
    eff = e0 + e1 * f / 1e9
    theta = w * np.sqrt(eff) / C0 * (length_mm + d_mm) * 1e-3
    z = -1j * z0 / np.tan(theta) + 1j * w * lp_nh * 1e-9
    return (z - ZREF) / (z + ZREF)


def _residual(p, data):
    out = []
    for f, g, length in data:
        r = gamma(p, f, length) - g
        out.append(r.real)
        out.append(r.imag)
    return np.concatenate(out)


def fit(data, dispersion=True, starts=None):
    """data: list of (f array Hz, S11 complex array, length mm). Returns (params, rms, sigma) with params
    [Z0, e0, e1, d_mm, Lp_nH]."""
    free = [0, 1, 2, 3, 4] if dispersion else [0, 1, 3, 4]
    scale = np.array([50.0, 1.0, 0.1, 1.0, 0.5])
    best = None
    inits = starts or [(z0, e0, 0.0, d, lp) for z0 in (50, 85) for e0 in (2.8, 3.3, 3.8) for d in (0.3, 1.0, 2.0) for lp in (0.1, 0.4)]
    for init in inits:
        p = np.array(init, float)
        lam = 1e-2
        r = _residual(p, data)
        cost = r @ r
        for _ in range(80):
            jac = np.empty((len(r), len(free)))
            for k, idx in enumerate(free):
                h = 1e-5 * scale[idx]
                q = p.copy()
                q[idx] += h
                jac[:, k] = (_residual(q, data) - r) / h
            a = jac.T @ jac
            g = jac.T @ r
            moved = False
            for _ in range(12):
                try:
                    step = np.linalg.solve(a + lam * np.diag(np.diag(a) + 1e-12), -g)
                except np.linalg.LinAlgError:
                    lam *= 10
                    continue
                q = p.copy()
                q[free] += step
                if q[0] <= 1 or q[1] <= 1 or q[3] < -5 or q[3] > 10:
                    lam *= 10
                    continue
                rq = _residual(q, data)
                cq = rq @ rq
                if cq < cost:
                    p, r, cost, lam, moved = q, rq, cq, max(lam / 5, 1e-9), True
                    break
                lam *= 10
            if not moved or abs(g @ g) < 1e-24:
                break
        if best is None or cost < best[1]:
            best = (p, cost)
    p, cost = best
    r = _residual(p, data)
    jac = np.empty((len(r), len(free)))
    for k, idx in enumerate(free):
        h = 1e-5 * scale[idx]
        q = p.copy()
        q[idx] += h
        jac[:, k] = (_residual(q, data) - r) / h
    n, m = len(r), len(free)
    s2 = cost / max(n - m, 1)
    try:
        sig = np.sqrt(np.diag(np.linalg.inv(jac.T @ jac)) * s2)
    except np.linalg.LinAlgError:
        sig = np.full(m, np.nan)
    sigma = np.zeros(5)
    sigma[free] = sig
    return p, math.sqrt(cost / n), sigma


def load(key, results_root=None):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import touchstone as ts

    root = results_root or os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "out", "lines-check")
    saved = json.load(open(os.path.join(root, "last.json")))
    freqs, s11, _ = ts.read(os.path.join(saved[key], "s11.s1p"))
    return np.array(freqs), np.array(s11)


def main():
    a = sys.argv[1:]
    if len(a) < 4:
        print(__doc__)
        sys.exit(2)
    fmin, fmax = 0.8, 2.2
    if "--fmin" in a:
        fmin = float(a[a.index("--fmin") + 1])
    if "--fmax" in a:
        fmax = float(a[a.index("--fmax") + 1])
    data = []
    for key, length in ((a[0], float(a[2])), (a[1], float(a[3]))):
        f, g = load(key)
        sel = (f >= fmin * 1e9) & (f <= fmax * 1e9)
        data.append((f[sel], g[sel], length))
    for disp in (False, True):
        p, rms, sig = fit(data, dispersion=disp)
        print("dispersion slope %s: Z0 %.2f ohm, eeff(0) %.4f +- %.4f, slope %.4f per GHz +- %.4f, "
              "open end d %.3f mm +- %.3f, port inductance %.3f nH, rms residual %.4f"
              % ("fitted" if disp else "fixed at 0", p[0], p[1], sig[1], p[2], sig[2], p[3], sig[3], p[4], rms))
        if disp:
            for f in (0.5, 1.0, 1.5, 2.0, 2.5):
                print("    eeff(%.1f GHz) = %.4f" % (f, p[1] + p[2] * f))


if __name__ == "__main__":
    main()
