#!/usr/bin/env python3
"""Known-correct values for the numerical tests, computed with 50 digits (mpmath, BSD-3-Clause: a generator
dependency only, never needed to build or run anything). Prints C++ constants to paste into the tests.

    python3 -m venv out/venv-golden && out/venv-golden/bin/pip install mpmath
    out/venv-golden/bin/python scripts/make_golden.py

Each block names the test that holds its values. The formulas are written out in the test next to them."""
from mpmath import mp, mpf, exp, pi, sqrt, cos, sin, mpc

mp.dps = 50


def gaussian_band_pulse_transform():
    """tests/cpu/dft_known_test.cpp: the Fourier transform of g(t) = exp(-u^2) cos(w0 s), u = s / tau,
    s = t - t0, for the band 2.4 to 2.5 GHz (tau = 1.1 / bandwidth, t0 = 4 tau)."""
    f_min, f_max = mpf("2.4e9"), mpf("2.5e9")
    f0, tau = (f_min + f_max) / 2, mpf("1.1") / (f_max - f_min)
    t0 = 4 * tau
    print("// G(f), f_min 2.4 GHz, f_max 2.5 GHz; 50-digit values rounded to 17 significant digits")
    print("constexpr double kGolden[][3] = {  // f in Hz, real, imaginary")
    for f in ["2.0e9", "2.3e9", "2.4e9", "2.45e9", "2.5e9", "2.6e9", "3.0e9"]:
        f = mpf(f)
        amp = tau * sqrt(pi) / 2 * (exp(-((pi * tau * (f - f0)) ** 2)) + exp(-((pi * tau * (f + f0)) ** 2)))
        g = amp * mpc(cos(2 * pi * f * t0), -sin(2 * pi * f * t0))
        print("    {%s, %s, %s}," % (mp.nstr(f, 17), mp.nstr(g.real, 17), mp.nstr(g.imag, 17)))
    print("};")


if __name__ == "__main__":
    gaussian_band_pulse_transform()
