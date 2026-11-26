#!/usr/bin/env python3
"""Transmission-line and wavelength helpers for feed and placement checks. Standard library only.

  lines.py microstrip --w MM --h MM --er ER                 Z0 and effective permittivity
  lines.py cpwg --w MM --gap MM --h MM --er ER              coplanar waveguide with ground below
  lines.py solve (microstrip|cpwg) --z0 50 --h MM --er ER [--gap MM]    track width for a target Z0
  lines.py lambda --f HZ [--er ER]                          wavelengths and quarter-wave lengths

Zero-thickness conductors, lossless, quasi-static: good to a few percent, enough to see that a feed is near 50 ohm or
far from it.

Sources and what was read. Microstrip: E. Hammerstad and O. Jensen, "Accurate models for microstrip
computer-aided design", IEEE MTT-S Int. Microwave Symp. Digest 1980, pp. 407-409, doi:10.1109/MWSYM.1980.1124303
(DOI checked on Crossref). The paper is behind a paywall and was NOT read; the equations were taken from two open
secondary sources that agree (Qucs Technical Documentation, "Single microstrip line", and F4INX, "Microstrip formulas
comparison", 2024). They differ in one place: the second page prints the last term of a(u) without the cube; the
first, and this code, use (u/18.1)^3; the effect is at most 0.3 percent of the effective permittivity. Coplanar
waveguide with ground: the conformal-mapping equations (filling factor q, k1 = W/(W+2s), k3 with tanh) were checked
against the Qucs Technical Documentation, "Coplanar waveguides (CPW)", which borrows them from its references; they
are the form in R. N. Simons, Coplanar Waveguide Circuits, Components, and Systems (Wiley 2001), chapter 3,
doi:10.1002/0471224758.ch3 (DOI checked on Crossref); the book is behind a paywall and was NOT read. The K/K' ratio
here uses exact elliptic integrals instead of the Qucs piecewise approximation (accurate to about 1e-5). The coplanar
formula has no strip fringing term, so with a very wide gap it reads about 10 to 15 percent below the microstrip
formula: trust it for gaps of the order of the strip width or smaller, and check a feed with a field solver.
Checked against this project's FDTD solver (scripts/antenna/validate_lines.py, results in docs/PLAN.md):
microstrip Z0 within about 2 percent on 50 and 87 ohm lines once the solver's air next to the board is resolved;
coplanar with ground consistent with the solver's convergence, not confirmed better than about 8 percent.
The 2D field solve (src/fdtd/line2d.*) agrees with the coplanar closed form to 0.1 to 0.4 percent for a sheet
of copper and shows that 35 um of copper lowers a coplanar line's impedance by 6 to 9 percent, which no closed form
here includes. The self-test checks the microstrip against the well known 50 ohm point and the coplanar line against the air
coplanar line at k = 1/sqrt(2), 30 pi ohm (Qucs equation 12.7 with effective permittivity 1).
"""

import argparse
import math

ETA0 = 376.730313668
C0 = 299792458.0


def microstrip(w, h, er):
    """Hammerstad and Jensen (1980), zero thickness: effective permittivity with the a(u), b(er) exponent."""
    u = w / h
    a = 1 + math.log((u**4 + (u / 52) ** 2) / (u**4 + 0.432)) / 49 + math.log(1 + (u / 18.1) ** 3) / 18.7
    b = 0.564 * ((er - 0.9) / (er + 3)) ** 0.053
    eff = (er + 1) / 2 + (er - 1) / 2 * (1 + 10 / u) ** (-a * b)
    f1 = 6 + (2 * math.pi - 6) * math.exp(-((30.666 / u) ** 0.7528))
    zl = ETA0 / (2 * math.pi) * math.log(f1 / u + math.sqrt(1 + (2 / u) ** 2))
    return zl / math.sqrt(eff), eff


def _agm(a, b):
    for _ in range(40):
        a, b = (a + b) / 2, math.sqrt(a * b)
    return a


def ellk(k):
    """Complete elliptic integral of the first kind, modulus k (arithmetic-geometric mean)."""
    return math.pi / (2 * _agm(1.0, math.sqrt(1 - k * k)))


def kratio(k):
    """K(k)/K'(k)."""
    return ellk(k) / ellk(math.sqrt(1 - k * k))


def cpwg(w, gap, h, er):
    a, b = w / 2, w / 2 + gap
    k = a / b
    k1 = math.tanh(math.pi * a / (2 * h)) / math.tanh(math.pi * b / (2 * h))
    r, r1 = kratio(k), kratio(k1)
    # K'/K = 1/ratio
    eff = (1 + er * (1 / r) * r1) / (1 + (1 / r) * r1)
    z0 = ETA0 / 2 / math.sqrt(eff) / (r + r1)
    return z0, eff


def solve(kind, z0, h, er, gap=None):
    lo, hi = 0.02 * h, 20 * h
    for _ in range(80):
        mid = math.sqrt(lo * hi)
        z = microstrip(mid, h, er)[0] if kind == "microstrip" else cpwg(mid, gap, h, er)[0]
        if z > z0:
            lo = mid
        else:
            hi = mid
    return math.sqrt(lo * hi)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("microstrip")
    p.add_argument("--w", type=float, required=True)
    p.add_argument("--h", type=float, required=True)
    p.add_argument("--er", type=float, required=True)
    p = sub.add_parser("cpwg")
    p.add_argument("--w", type=float, required=True)
    p.add_argument("--gap", type=float, required=True)
    p.add_argument("--h", type=float, required=True)
    p.add_argument("--er", type=float, required=True)
    p = sub.add_parser("solve")
    p.add_argument("kind", choices=["microstrip", "cpwg"])
    p.add_argument("--z0", type=float, default=50.0)
    p.add_argument("--h", type=float, required=True)
    p.add_argument("--er", type=float, required=True)
    p.add_argument("--gap", type=float)
    p = sub.add_parser("lambda")
    p.add_argument("--f", type=float, required=True)
    p.add_argument("--er", type=float, default=1.0)
    a = ap.parse_args()
    if a.cmd == "microstrip":
        z, e = microstrip(a.w, a.h, a.er)
        print(f"Z0 {z:.1f} ohm, effective permittivity {e:.2f}")
    elif a.cmd == "cpwg":
        z, e = cpwg(a.w, a.gap, a.h, a.er)
        print(f"Z0 {z:.1f} ohm, effective permittivity {e:.2f}")
    elif a.cmd == "solve":
        if a.kind == "cpwg" and a.gap is None:
            raise SystemExit("cpwg needs --gap")
        w = solve(a.kind, a.z0, a.h, a.er, a.gap)
        print(f"width {w:.3f} mm for {a.z0:g} ohm")
    else:
        lam = C0 / a.f * 1e3
        print(f"free space: lambda {lam:.1f} mm, lambda/4 {lam / 4:.1f} mm, lambda/2 {lam / 2:.1f} mm")
        if a.er != 1.0:
            g = lam / math.sqrt(a.er)
            print(f"in er {a.er:g} (a bound; a line sees less): lambda {g:.1f} mm, lambda/4 {g / 4:.1f} mm")


if __name__ == "__main__":
    main()
