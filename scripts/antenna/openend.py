"""Open-end equivalent length of a microstrip, from the Qucs technical documentation, "Microstrip open"
(https://qucs.sourceforge.net/tech/node78.html, eqs. 11.185 to 11.191, read in full, formulas transcribed from
the page's own equation images). Provenance: the formulas are the documentation's statement of Kirschning,
Jansen and Koster, Electronics Letters 17 (1981) 123-125 (paper unread, DOI 10.1049/el:19810088) and of
Hammerstad, MTT-S Digest 1981 pp. 54-56 (unread). The page states the numerical error of the first model as
under 2.5 percent for 0.01 <= W/h <= 100 and 1 <= er <= 50, of the second as under 1.7 percent for W/h < 20.
The effective permittivity they need is the static Hammerstad-Jensen one (lines.microstrip).

    python3 scripts/antenna/openend.py W_MM H_MM ER
"""
import math
import sys

import lines


def kjk(u, er, ee):
    """Kirschning, Jansen and Koster open-end extension dl / h; u = W/h, ee = effective permittivity."""
    q1 = 0.434907 * (ee**0.81 + 0.26) / (ee**0.81 - 0.189) * (u**0.8544 + 0.236) / (u**0.8544 + 0.87)
    q2 = 1 + u**0.371 / (2.358 * er + 1)
    q3 = 1 + 0.5274 / ee**0.9236 * math.atan(0.084 * u ** (1.9413 / q2))
    q4 = 1 + 0.0377 * (6 - 5 * math.exp(0.036 * (1 - er))) * math.atan(0.067 * u**1.456)
    q5 = 1 - 0.218 * math.exp(-7.5 * u)
    return q1 * q3 * q5 / q4


def hammerstad(u, er):
    """Hammerstad's open-end extension dl / h (the page's second model)."""
    return 0.102 * (u + 0.106) / (u + 0.264) * (1.166 + (er + 1) / er * (0.9 + math.log(u + 2.475)))


def extension_mm(w, h, er):
    """(KJK, Hammerstad) equivalent open-end lengths in mm, and the static effective permittivity."""
    z0, ee = lines.microstrip(w, h, er)
    u = w / h
    return kjk(u, er, ee) * h, hammerstad(u, er) * h, ee


def main():
    w, h, er = (float(a) for a in sys.argv[1:4])
    k, ha, ee = extension_mm(w, h, er)
    print("W %.3g h %.3g er %.3g: eeff %.3f, open-end length %.3f mm (KJK), %.3f mm (Hammerstad)" % (w, h, er, ee, k, ha))


if __name__ == "__main__":
    main()
