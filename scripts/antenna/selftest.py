#!/usr/bin/env python3
"""Self-test of the antenna helper scripts (closed-form checks). Run: python3 scripts/antenna/selftest.py"""

import cmath
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cascade  # noqa: E402
import lines  # noqa: E402
import match  # noqa: E402
import touchstone as ts  # noqa: E402


def check(name, ok, detail=""):
    print(("ok   " if ok else "FAIL ") + name + (" " + detail if detail else ""))
    if not ok:
        sys.exit(1)


f = 2.44e9
# 25 ohm to 50 ohm: R < Z0 so the series-then-shunt pair exists; X_t = 25, B = 0.02 S
sols = match.lmatch(complex(25, 0), f)
check("lmatch 25 ohm gives solutions", len(sols) >= 2)
check(
    "every L network matches to better than -100 dB",
    all(s["s11_db"] < -100 for s in sols),
    str([round(s["s11_db"]) for s in sols]),
)
ser = [s for s in sols if s["net"][0][0] == "series" and s["net"][0][1] == "L"][0]
check("series L is 25 ohm of reactance", abs(ser["net"][0][2] * 2 * math.pi * f - 25) < 1e-6)
# a datasheet-style complex load, both conditions possible
z = complex(27.6, 22.4)
check(
    "complex load solutions", all(s["s11_db"] < -100 for s in match.lmatch(z, f)) and match.lmatch(z, f) != []
)
# a load above Z0
z = complex(120, -30)
check("load above Z0", all(s["s11_db"] < -100 for s in match.lmatch(z, f)) and match.lmatch(z, f) != [])
# network cascade: a shunt C of 1 pF across 50 ohm at 2.44 GHz
y = 1 / 50 + 1j * 2 * math.pi * f * 1e-12
check("shunt C cascade", abs(match.apply_network(50, [("shunt", "C", 1e-12)], f) - 1 / y) < 1e-9)
check("value suffixes", abs(match.value("0.5p") - 0.5e-12) < 1e-24 and abs(match.value("2n") - 2e-9) < 1e-21)
# Touchstone: the same reflection in RI, MA and DB
g = complex(0.3, -0.4)
ri = f"# GHZ S RI R 50\n2.44 {g.real} {g.imag}\n"
ma = f"# GHZ S MA R 50\n2.44 {abs(g)} {math.degrees(math.atan2(g.imag, g.real))}\n"
dbt = f"# GHZ S DB R 50\n2.44 {20 * math.log10(abs(g))} {math.degrees(math.atan2(g.imag, g.real))}\n"
check("Touchstone RI, MA and DB agree", all(abs(ts.parse(t)[1][0] - g) < 1e-9 for t in (ri, ma, dbt)))
check("Z and gamma round trip", abs(ts.z_to_gamma(ts.gamma_to_z(g)) - g) < 1e-12)
# search finds an exact-ish match for a simple load within E12 parts
zs = [complex(25, 0)]
net = [("series", "L", 25 / (2 * math.pi * f)), ("shunt", "C", 0.02 / (2 * math.pi * f))]
check("hand network matches", ts.db(ts.z_to_gamma(match.apply_network(zs[0], net, f))) < -100)
# lines: the 50 ohm microstrip point, and the coplanar line in its wide-gap (microstrip) limit
w = lines.solve("microstrip", 50, 1.6, 4.4)
check("50 ohm microstrip on 1.6 mm FR-4 is about 3 mm wide", 2.8 < w < 3.2, f"{w:.2f} mm")
zair = lines.cpwg(1.0, 0.5 * (math.sqrt(2) - 1), 1e5, 1.0)[
    0
]  # k = a/b = 1/sqrt(2), no ground, air: 30 pi ohm
check("air coplanar line at k = 1/sqrt(2) is 30 pi ohm", abs(zair - 30 * math.pi) < 0.2, f"{zair:.2f} ohm")
zm = lines.microstrip(w, 1.6, 4.4)[0]
check("a narrow gap lowers Z0", lines.cpwg(w, 0.2, 1.6, 4.4)[0] < zm)
print("antenna scripts: ok")

# cascade: a uniform line into its own impedance is a perfect match, a quarter-wave section transforms
# Z0^2 / ZL, an open stub is invisible when short and a short circuit at a quarter wave
e = 2.8
lam_g = cascade.C / f / math.sqrt(e)
check(
    "uniform 50 ohm line into 50 ohm: perfect match",
    cascade.return_loss_db(cascade.zin([(50, 7e-3)], 50, f, e)) < -100,
)
zq = cascade.zin([(math.sqrt(50 * 100), lam_g / 4)], 100, f, e)
check("quarter-wave transformer: 70.71 ohm turns 100 ohm into 50", abs(zq - 50) < 1e-6, str(zq))
check(
    "a 0.5 mm open stub barely matters, a quarter-wave stub shorts the line",
    cascade.return_loss_db(cascade.open_stub_load(50, 91, 0.5e-3, f, e)) < -30
    and cascade.return_loss_db(cascade.open_stub_load(50, 91, lam_g / 4, f, e)) > -0.1,
)

# resonance finder: a series RLC load (R 17 ohm, L 2 nH, C chosen for 2.14 GHz) crosses X = 0 upwards there
L_, R_ = 2e-9, 17.0
C_ = 1 / ((2 * math.pi * 2.14e9) ** 2 * L_)
fs = [2.0e9 + k * 10e6 for k in range(30)]
g = [ts.z_to_gamma(complex(R_, 2 * math.pi * x * L_ - 1 / (2 * math.pi * x * C_)), 50) for x in fs]
res = match.resonances(fs, g, 50)
check(
    "resonance finder: series RLC at 2.14 GHz, R = 17 ohm",
    len(res) == 1 and abs(res[0][0] - 2.14e9) < 2e6 and abs(res[0][1] - R_) < 0.2 and res[0][2],
    str(res),
)

# line_extract, bend_extract and bodefano need numpy: skipped without it
try:
    import numpy as np
except ImportError:
    np = None
if np is None:
    print("skip line_extract, bend_extract, bodefano (numpy is not installed)")
else:
    import bend_extract  # noqa: E402
    import bodefano  # noqa: E402
    import line_extract  # noqa: E402

    fq = np.linspace(0.8e9, 2.2e9, 121)
    truth = np.array([85.0, 3.25, 0.03, 0.62, 0.22])
    pair = [(fq, line_extract.gamma(truth, fq, ln), ln) for ln in (20.0, 30.0)]
    got, rms, _ = line_extract.fit(pair)
    check(
        "two-length extraction recovers eeff, Z0 and the end offset from exact data",
        abs(got[1] - 3.25) < 1e-3 and abs(got[0] - 85.0) < 0.1 and abs(got[3] - 0.62) < 0.01 and rms < 1e-6,
        str(np.round(got, 4)),
    )
    straight = (63.5, 3.40, 1.94, 0.17)
    gb = bend_extract.gamma_bend_len(fq, straight, 10.0, 14.0, 0.15, -2.0)
    qb, _, rb = bend_extract.fit_bend(fq, gb, straight, 10.0, 14.0, model=bend_extract.gamma_bend_len)
    check(
        "bend extraction recovers a corner capacitance and length shift from exact data",
        abs(qb[0] - 0.15) < 1e-3 and abs(qb[1] + 2.0) < 1e-2,
        "Cb %.4f pF, shift %.3f mm" % (qb[0], qb[1]),
    )
    # Bode-Fano: a bare resistor R0 = R has the whole area pi / (R C); a 3-element ladder cannot exceed it
    check("Bode-Fano: no network exceeds pi / (R C)", bodefano.total_area([1.0], 0) <= math.pi * 1.0005
          and bodefano.total_area([0.9, 1.2, 1.4, 0.5], 3) <= math.pi * 1.0005)

import openend  # noqa: E402

for _w in (1.0, 2.0, 3.06):
    _k, _h, _ = openend.extension_mm(_w, 1.6, 4.4)
    check("open end: the two published models agree within 5 percent (W %.2f mm)" % _w, abs(_k - _h) / _h < 0.05, "%.3f vs %.3f mm" % (_k, _h))

# the reactance slope: an ideal series RLC has dX/df = 4 pi L at resonance and a loaded Q of w0 L / R
import slope  # noqa: E402

_f0, _R, _L = 2.4e9, 50.0, 4.0e-9
_C = 1.0 / ((2 * math.pi * _f0) ** 2 * _L)
_freqs = [2.0e9 + i * 4e6 for i in range(201)]
_s = []
for _fq in _freqs:
    _w = 2 * math.pi * _fq
    _z = complex(_R, _w * _L - 1.0 / (_w * _C))
    _s.append((_z - 50.0) / (_z + 50.0))
_r = slope.analyze(_freqs, _s, window=(2.38e9, 2.42e9), half_width=25e6)
_truth = 4 * math.pi * _L * 50e6
check(
    "slope: a series RLC has dX/df = 4 pi L at its resonance (own window and fixed window)",
    abs(_r["slope_own"] - _truth) / _truth < 0.02 and abs(_r["slope_fixed"] - _truth) / _truth < 0.02,
    "%.2f and %.2f against %.2f ohm per 50 MHz" % (_r["slope_own"], _r["slope_fixed"], _truth),
)
_q = 2 * math.pi * _f0 * _L / _R
check("slope: q_eff is w0 L / R", abs(_r["q_eff"] - _q) / _q < 0.02, "%.2f against %.2f" % (_r["q_eff"], _q))
check("slope: the series resonance is found", abs(_r["series_hz"] - _f0) < 2e6)

import known_answer  # noqa: E402

_w, _l, _ = known_answer.geometry()
check(
    "patch cavity model returns its own design frequency",
    abs(known_answer.patch_resonance(4.5, _w, _l) - 2.45e9) / 2.45e9 < 1e-6,
)
check(
    "lower er raises the patch resonance by about 3.4 percent",
    0.030 < known_answer.expected_shift(_w, _l) < 0.037,
    "%+.2f %%" % (100 * known_answer.expected_shift(_w, _l)),
)
check(
    "same coarse cell at er 4.2 needs fewer cells per wavelength",
    abs(known_answer.cpw_for(4.2, 20) - 20 * math.sqrt(4.2 / 4.5)) < 1e-12,
)

import port_controls  # noqa: E402

_w50, _eeff50 = port_controls.hammerstad(4.5, 1.6e-3)
check(
    "Hammerstad: a 50 ohm strip on 1.6 mm FR-4 (er 4.5) is about 3.0 mm wide with eeff about 3.4",
    2.9e-3 < _w50 < 3.1e-3 and 3.3 < _eeff50 < 3.5,
    "%.3f mm, eeff %.3f" % (_w50 * 1e3, _eeff50),
)
check(
    "line phase slope: 5 mm of eeff 3.39 line is about 22 degrees per GHz",
    abs(port_controls.slope_per_metre(3.394) * 5e-3 - 22.1) < 0.2,
)
check("reflection of 100 ohm on 50 ohm is 1/3", abs(port_controls.gamma(100.0) - 1 / 3) < 1e-12)
_f = [1.5 + 0.01 * i for i in range(201)]
_open = [(x, cmath.exp(-1j * math.radians(22.0 * x))) for x in _f]
_sl, _p0 = port_controls.phase_fit(_open)
check("phase_fit reads an open line: slope -22 deg/GHz, 0 degrees at f = 0", abs(_sl + 22) < 1e-6 and min(_p0, 360 - _p0) < 1e-6)
_short = [(x, -cmath.exp(-1j * math.radians(22.0 * x))) for x in _f]
check("phase_fit reads a short line: 180 degrees at f = 0", abs(port_controls.phase_fit(_short)[1] - 180) < 1e-6)
