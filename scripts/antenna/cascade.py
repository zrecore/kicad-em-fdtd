#!/usr/bin/env python3
"""How much a short step, neck or open stub on a feed line matters, by cascading ideal lossless line sections
(textbook ABCD matrices, e.g. Pozar, Microwave Engineering, ch. 2 and 4). Standard library only.

  python3 scripts/antenna/cascade.py steps --f HZ --eeff E --load R [--z0 50] SECTION...
      SECTION is Z0,LENGTH_MM (from the source towards the load); prints the input impedance and the return
      loss against --z0 at the source.
  python3 scripts/antenna/cascade.py stub --f HZ --eeff E --stub-z0 OHM [--z0 50] LENGTH_MM...
      An open stub of the given characteristic impedance across a --z0 line; prints the return loss it adds.

Ideal sections: no dispersion, no corner or junction capacitance, no loss. Use it to size an effect (is the
neck 0.4 mm or 4 mm long? is the stub a twentieth of a wavelength or a quarter?), then check the real geometry
with the field solver.
"""

import argparse
import math

C = 299792458.0


def section(z0, eeff, length_m, f):
    beta_l = 2 * math.pi * f * math.sqrt(eeff) / C * length_m
    return [[math.cos(beta_l), 1j * z0 * math.sin(beta_l)], [1j * math.sin(beta_l) / z0, math.cos(beta_l)]]


def _mul(a, b):
    return [
        [a[0][0] * b[0][0] + a[0][1] * b[1][0], a[0][0] * b[0][1] + a[0][1] * b[1][1]],
        [a[1][0] * b[0][0] + a[1][1] * b[1][0], a[1][0] * b[0][1] + a[1][1] * b[1][1]],
    ]


def zin(sections, load, f, eeff):
    """Input impedance of (z0, length_m) sections from the source to `load` (ohm, complex)."""
    m = [[1, 0], [0, 1]]
    for z0, length in sections:
        m = _mul(m, section(z0, eeff, length, f))
    return (m[0][0] * load + m[0][1]) / (m[1][0] * load + m[1][1])


def return_loss_db(z, z0=50.0):
    g = abs((z - z0) / (z + z0))
    return 20 * math.log10(max(g, 1e-16))  # S11 in dB (negative)


def open_stub_load(load_z0, stub_z0, stub_length, f, eeff):
    """A `load_z0` termination with an open stub of `stub_z0` across it."""
    beta_l = 2 * math.pi * f * math.sqrt(eeff) / C * stub_length
    return 1 / (1 / load_z0 + 1j * math.tan(beta_l) / stub_z0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("steps", "stub"):
        p = sub.add_parser(name)
        p.add_argument("--f", type=float, required=True, help="Hz")
        p.add_argument("--eeff", type=float, required=True)
        p.add_argument("--z0", type=float, default=50.0)
        if name == "steps":
            p.add_argument("--load", type=float, required=True)
            p.add_argument("sections", nargs="+", help="Z0,LENGTH_MM")
        else:
            p.add_argument("--stub-z0", type=float, required=True)
            p.add_argument("lengths", nargs="+", type=float, help="stub length, mm")
    a = ap.parse_args()
    lam = C / a.f / math.sqrt(a.eeff) * 1e3
    print(f"guided wavelength {lam:.1f} mm, quarter wave {lam / 4:.1f} mm")
    if a.cmd == "steps":
        secs = []
        for s in a.sections:
            z, l_mm = s.split(",")
            secs.append((float(z), float(l_mm) * 1e-3))
        z = zin(secs, complex(a.load), a.f, a.eeff)
        print(f"Zin {z.real:.1f}{z.imag:+.1f}j ohm, S11 {return_loss_db(z, a.z0):.1f} dB against {a.z0:g} ohm")
    else:
        for l_mm in a.lengths:
            z = open_stub_load(a.z0, a.stub_z0, l_mm * 1e-3, a.f, a.eeff)
            print(f"stub {l_mm:g} mm ({l_mm / lam:.3f} wavelengths): S11 {return_loss_db(z, a.z0):.1f} dB")


if __name__ == "__main__":
    main()
