#!/usr/bin/env python3
"""Datasheet marker check: |S11| in dB from impedance markers, and the reverse. A datasheet that prints both a dB value
and an impedance at the same frequency lets you check that you read the right curve and the right reference impedance.

Usage: s11_from_impedance.py R+jX [R+jX ...]        e.g.  27.6+22.4j  62.1+6.9j  45.0-40.9j   (ohm, 50 ohm reference)
       s11_from_impedance.py --z0 75 ...             another reference impedance
"""
import math
import sys


def main():
    args = sys.argv[1:]
    z0 = 50.0
    if args[:1] == ["--z0"]:
        z0, args = float(args[1]), args[2:]
    if not args:
        raise SystemExit(__doc__)
    for a in args:
        z = complex(a.replace(" ", ""))
        g = (z - z0) / (z + z0)
        print(f"Z = {z.real:.2f}{z.imag:+.2f}j ohm -> |S11| {20 * math.log10(abs(g)):.2f} dB, VSWR {(1 + abs(g)) / (1 - abs(g)):.2f}, "
              f"mismatch loss {-10 * math.log10(1 - abs(g) ** 2):.2f} dB")


if __name__ == "__main__":
    main()
