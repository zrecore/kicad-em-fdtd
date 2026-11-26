"""One-port Touchstone (.s1p) reading, shared by the antenna scripts. Standard library only.

parse(text) -> (freqs_hz, s11 list of complex, z0). Handles HZ/KHZ/MHZ/GHZ and RI, MA, DB; extra ports' columns
are ignored (S11 is the first pair). Also: gamma_to_z, z_to_gamma, db.
"""

import cmath
import math

UNITS = {"HZ": 1.0, "KHZ": 1e3, "MHZ": 1e6, "GHZ": 1e9}


def parse(text):
    unit, fmt, z0 = 1e9, "MA", 50.0  # the Touchstone default option line
    freqs, s11 = [], []
    for raw in text.splitlines():
        line = raw.split("!", 1)[0].strip()
        if not line:
            continue
        if line.startswith("#"):
            tok = line[1:].upper().split()
            for i, t in enumerate(tok):
                if t in UNITS:
                    unit = UNITS[t]
                elif t in ("RI", "MA", "DB"):
                    fmt = t
                elif t == "R" and i + 1 < len(tok):
                    z0 = float(tok[i + 1])
            continue
        v = line.split()
        if len(v) < 3:
            continue
        a, b = float(v[1]), float(v[2])
        if fmt == "RI":
            g = complex(a, b)
        elif fmt == "MA":
            g = cmath.rect(a, math.radians(b))
        else:
            g = cmath.rect(10 ** (a / 20), math.radians(b))
        freqs.append(float(v[0]) * unit)
        s11.append(g)
    if not freqs:
        raise ValueError("no data lines in the Touchstone text")
    return freqs, s11, z0


def read(path):
    with open(path) as f:
        return parse(f.read())


def gamma_to_z(g, z0=50.0):
    return z0 * (1 + g) / (1 - g) if abs(1 - g) > 1e-12 else complex(1e12, 0)


def z_to_gamma(z, z0=50.0):
    return (z - z0) / (z + z0)


def db(g):
    return 20 * math.log10(max(abs(g), 1e-12))
