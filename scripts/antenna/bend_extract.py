#!/usr/bin/env python3
"""Equivalent circuit (series inductance, shunt capacitance) of a microstrip right-angle bend, plain or mitred, from the
S11 of an open-ended line with one corner (needs numpy).

  python3 scripts/antenna/bend_extract.py BEND_KEY --straight KEY_SHORT KEY_LONG LEN_SHORT LEN_LONG --legs LEG1 LEG2
         [--validate KEY LEN_MM] [--fmin GHZ] [--fmax GHZ]
      keys are names in out/lines-check/last.json (validate_lines.py writes them).

Method (derived here, standard network analysis; not a reproduction of any paper's method): the straight-line
parameters (Z0, eeff, the common end/port offset d, port inductance Lp) come from the two-length fit of
line_extract.py on straight lines of the same width, board and mesh. The bent line is leg1, a T network (Lb/2, shunt
Cb, Lb/2) at the crossing of the two centre lines, leg2 and the open end (extension d). Only Cb and Lb are fitted,
to the complex S11 of the bent line. Optionally a straight line of the same total length is predicted with the straight
parameters alone to show how well the straight model carries over (the baseline to read the bend against).
"""
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import line_extract as le  # noqa: E402

C0, ZREF = le.C0, le.ZREF


def line_in(z0, beta, length_m, zl):
    t = np.tan(beta * length_m)
    return z0 * (zl + 1j * z0 * t) / (z0 + 1j * zl * t)


def gamma_bend(f, straight, leg1_mm, leg2_mm, cb_pf, lb_nh):
    z0, e0, d_mm, lp_nh = straight
    w = 2 * math.pi * f
    beta = w * np.sqrt(e0) / C0
    z2 = -1j * z0 / np.tan(beta * (leg2_mm + d_mm) * 1e-3)  # line 2 and the open end, seen at the corner
    zb = z2 + 1j * w * lb_nh * 1e-9 / 2
    zc = 1.0 / (1.0 / zb + 1j * w * cb_pf * 1e-12)
    zd = zc + 1j * w * lb_nh * 1e-9 / 2
    z = line_in(z0, beta, leg1_mm * 1e-3, zd) + 1j * w * lp_nh * 1e-9
    return (z - ZREF) / (z + ZREF)


def gamma_bend_len(f, straight, leg1_mm, leg2_mm, cb_pf, dl_mm):
    """The corner as a shunt capacitance at the crossing of the centre lines and a signed length shift of the line
    after it (a pure delay, which does not reflect)."""
    z0, e0, d_mm, lp_nh = straight
    w = 2 * math.pi * f
    beta = w * np.sqrt(e0) / C0
    z2 = -1j * z0 / np.tan(beta * (leg2_mm + dl_mm + d_mm) * 1e-3)
    zc = 1.0 / (1.0 / z2 + 1j * w * cb_pf * 1e-12)
    z = line_in(z0, beta, leg1_mm * 1e-3, zc) + 1j * w * lp_nh * 1e-9
    return (z - ZREF) / (z + ZREF)


def fit_bend(f, g, straight, leg1, leg2, model=gamma_bend):
    scale = np.array([0.5, 0.5])

    def res(q):
        r = model(f, straight, leg1, leg2, q[0], q[1]) - g
        return np.concatenate([r.real, r.imag])

    best = None
    for c0 in (0.01, 0.05, 0.2, 0.5, -0.02, -0.05):
        for l0 in (0.05, 0.3, 1.0, -0.3, -1.0):
            q = np.array([c0, l0])
            r = res(q)
            cost = r @ r
            lam = 1e-2
            for _ in range(100):
                jac = np.empty((len(r), 2))
                for k in range(2):
                    h = 1e-5 * scale[k]
                    qq = q.copy()
                    qq[k] += h
                    jac[:, k] = (res(qq) - r) / h
                a, gr = jac.T @ jac, jac.T @ r
                moved = False
                for _ in range(10):
                    step = np.linalg.solve(a + lam * np.diag(np.diag(a) + 1e-12), -gr)
                    qq = q + step
                    rr = res(qq)
                    cc = rr @ rr
                    if cc < cost:
                        q, r, cost, lam, moved = qq, rr, cc, max(lam / 5, 1e-9), True
                        break
                    lam *= 10
                if not moved:
                    break
            if best is None or cost < best[1]:
                best = (q, cost)
    q, cost = best
    r = res(q)
    jac = np.empty((len(r), 2))
    for k in range(2):
        h = 1e-5 * scale[k]
        qq = q.copy()
        qq[k] += h
        jac[:, k] = (res(qq) - r) / h
    s2 = cost / max(len(r) - 2, 1)
    try:
        sig = np.sqrt(np.diag(np.linalg.inv(jac.T @ jac)) * s2)
    except np.linalg.LinAlgError:
        sig = np.full(2, np.nan)
    return q, sig, math.sqrt(cost / len(r))


def bend_reflection(cb_pf, lb_nh, z0, f):
    """|S11| of the T network between two lines of impedance Z0 (the bend's own reflection) at frequency f."""
    w = 2 * math.pi * f
    zs = 1j * w * lb_nh * 1e-9 / 2
    yp = 1j * w * cb_pf * 1e-12
    a = np.array([[1, zs], [0, 1]], complex) @ np.array([[1, 0], [yp, 1]], complex) @ np.array([[1, zs], [0, 1]], complex)
    den = a[0, 0] + a[0, 1] / z0 + a[1, 0] * z0 + a[1, 1]
    return abs((a[0, 0] + a[0, 1] / z0 - a[1, 0] * z0 - a[1, 1]) / den)


def main():
    a = sys.argv[1:]
    if len(a) < 1 or "--straight" not in a or "--legs" not in a:
        print(__doc__)
        sys.exit(2)
    key = a[0]
    i = a.index("--straight")
    k1, k2, l1, l2 = a[i + 1], a[i + 2], float(a[i + 3]), float(a[i + 4])
    j = a.index("--legs")
    leg1, leg2 = float(a[j + 1]), float(a[j + 2])
    fmin = float(a[a.index("--fmin") + 1]) if "--fmin" in a else 0.8
    fmax = float(a[a.index("--fmax") + 1]) if "--fmax" in a else 2.2
    data = []
    for kk, length in ((k1, l1), (k2, l2)):
        f, g = le.load(kk)
        sel = (f >= fmin * 1e9) & (f <= fmax * 1e9)
        data.append((f[sel], g[sel], length))
    p, rms, _ = le.fit(data, dispersion=False)
    straight = (p[0], p[1], p[3], p[4])
    print("straight line (no dispersion term): Z0 %.2f ohm, eeff %.4f, common offset d %.3f mm, Lp %.3f nH, rms %.4f"
          % (p[0], p[1], p[3], p[4], rms))
    if "--validate" in a:
        v = a.index("--validate")
        f, g = le.load(a[v + 1])
        sel = (f >= fmin * 1e9) & (f <= fmax * 1e9)
        pred = le.gamma(p, f[sel], float(a[v + 2]))
        print("straight model against the straight %s mm line: rms complex error %.4f" % (a[v + 2], math.sqrt(np.mean(np.abs(pred - g[sel]) ** 2))))
    f, g = le.load(key)
    sel = (f >= fmin * 1e9) & (f <= fmax * 1e9)
    ql, sl, rl = fit_bend(f[sel], g[sel], straight, leg1, leg2, model=gamma_bend_len)
    gc = 2 * math.pi * 2.44e9 * ql[0] * 1e-12 * p[0]
    print("%s: shunt Cb = %.4f pF +- %.4f and a length shift %.3f mm +- %.3f (rms %.4f); the capacitance alone reflects |S11| = %.4f (%.1f dB) at 2.44 GHz"
          % (key, ql[0], sl[0], ql[1], sl[1], rl, abs(gc / (2 + 1j * gc)), 20 * math.log10(max(abs(gc / (2 + 1j * gc)), 1e-9))))
    q, sig, r = fit_bend(f[sel], g[sel], straight, leg1, leg2)
    print("%s: bend Cb = %.4f pF +- %.4f, Lb = %.4f nH +- %.4f, rms %.4f; the bend alone reflects |S11| = %.3f (%.1f dB) at 2.44 GHz"
          % (key, q[0], sig[0], q[1], sig[1], r, bend_reflection(q[0], q[1], p[0], 2.44e9),
             20 * math.log10(max(bend_reflection(q[0], q[1], p[0], 2.44e9), 1e-9))))


if __name__ == "__main__":
    main()
