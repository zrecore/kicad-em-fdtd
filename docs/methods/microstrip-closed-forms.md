# Microstrip and conductor-backed coplanar closed forms: what they claim, why they hold, and how well they hold

*In-house methods note, 2026-11-23. **Provenance:** two papers are paywalled and their full text is unread. Their open abstracts (via OpenAlex, paraphrased here, nothing copied) say: Hammerstad and Jensen, IEEE MTT-S Digest 1980, pp. 407-409, DOI 10.1109/MWSYM.1980.1124303, give very accurate closed-form equations for the characteristic impedance, effective dielectric constant and attenuation of single and coupled microstrip, including dispersion, strip thickness and an anisotropic substrate (no error bound in the abstract); R. N. Simons, Coplanar Waveguide Circuits, Components, and Systems, Wiley 2001, chapter 3, DOI 10.1002/0471224758.ch3, gives quasi-static effective dielectric constant and characteristic impedance of conductor-backed coplanar waveguide (CBCPW) without a top cover, then the effect of lateral walls on the dominant and higher-order modes (no numbers in the abstract). The equations used by this project come from two open secondary sources and the Qucs technical documentation (read), not from those papers. Below: what can be shown from first principles, and a numerical test of how good the equations are, against this project's own 2D field solver (`src/fdtd/line2d.*`, written here from finite-difference electrostatics). Not a reproduction of the papers' own error tables, which are unread.*

## 1. Why Z0 and the effective permittivity come from two capacitances (derived)
A quasi-TEM line in non-magnetic dielectrics. Let C be its capacitance per meter with the dielectrics and C0 with every dielectric replaced by air. The magnetic field is unaffected by non-magnetic dielectrics, so the inductance per meter is the air line's: L C0 = mu0 eps0 = 1/c^2. The telegraph equations give the phase velocity v = 1/sqrt(L C) = c sqrt(C0/C) and Z0 = sqrt(L/C) = 1/(c sqrt(C C0)). Defining the effective permittivity by v = c/sqrt(eeff):

    eeff = C / C0,    Z0 = 1 / (c sqrt(C C0)) = Z0,air / sqrt(eeff).                       (1)

`line2d` computes C and C0 from Laplace's equation (energy of the potential between a signal conductor at 1 V and the other conductors at 0 V) and applies (1); the closed forms are fits or conformal-map results for the same two numbers. Quasi-static means the dispersion (a frequency-dependent eeff) is not in (1); at 2.4 GHz on 1.6 mm FR-4 it is a small correction.

## 2. How the 2D solver is made trustworthy (and a bias found on the way)
* Parallel plates (exact C = eps eps0 / d), the first-order convergence of a thin conductor's edge, and a Richardson extrapolation that removes it are scenarios in `tests/cpu/line2d_test.cpp`.
* **A wall bias, measured:** the solver's domain has grounded walls beside and above the line. Moving them away from 4 to 8 to 16 line-widths (or substrate heights): 2.0 mm line on 1.6 mm of er 4.4: eeff 3.155, 3.201, 3.215 and Z0 62.43, 63.13, 63.34 ohm. The closed form says 3.219 and 63.50 ohm. So the earlier statement in this plan that the 2D solve sits 1 to 2 percent below the closed form was the 4-width wall margin, not the formula: with the walls at 16 widths the two agree to 0.1 percent (eeff) and 0.25 percent (Z0). The margin factor of `microstripSection` and `groundedCoplanarSection` is therefore a parameter that must be large when the point is the open-space value (the feed row uses 4 to keep the grid small and accepts its small bias; the tests that compare with closed forms use 8 or 16).

## 3. Microstrip: the closed form against the 2D solve (`feed_solve_test.cpp`, `[.report]`)
Zero-thickness strip, substrate height 1, walls 16 away, graded grid, Richardson-extrapolated; difference = closed form / 2D - 1.

| er | u = w/h | eeff diff | Z0 diff |
|---|---|---|---|
| 2.2 | 0.1, 0.25, 0.5, 1, 2, 4, 8 | -0.02, -0.03, -0.03, +0.03, -0.04, -0.12, -0.06 % | +0.26, +0.35, +0.43, +0.53, +0.28, +0.20, +0.10 % |
| 4.4 | same | +0.02, +0.04, +0.06, +0.14, +0.03, -0.16, -0.11 % | +0.24, +0.32, +0.39, +0.47, +0.25, +0.22, +0.13 % |
| 9.8 | same | +0.10, +0.14, +0.18, +0.28, +0.11, -0.17, -0.15 % | +0.20, +0.27, +0.33, +0.41, +0.21, +0.22, +0.15 % |

Over 21 combinations of width ratio (0.1 to 8) and permittivity (2.2 to 9.8) the closed form is within 0.28 percent of the independent solve in eeff and within +0.10 to +0.53 percent in Z0, always on the high side by a margin that peaks near u = 1. That is "very accurate" confirmed to about half a percent; the solve's own residual error (wall bias at 16 widths, grid) is of that size, so nothing tighter can be claimed from this test. **Not tested:** strip thickness (the closed form here is for zero thickness; the 2D solve shows what 35 um does, -1.4 percent in eeff for a 1.0 mm line on 1.6 mm), dispersion (needs a full-wave measurement, section 5 of `line-extraction-and-bends.md`), attenuation, anisotropy, coupled lines.

## 4. Conductor-backed coplanar waveguide: the closed form against the 2D solve
The form (Qucs documentation; Simons ch. 3 states the same quasi-static result, unread) comes from conformal mapping with partial capacitances: with a = w/2, b = a + gap, k = a/b, k1 = tanh(pi a / 2h) / tanh(pi b / 2h) and r(k) = K(k)/K'(k) the ratio of complete elliptic integrals (`ellipticRatio`), the air region above the strip and the dielectric-filled region below it are mapped to parallel-plate capacitors, giving a filling factor q = r(k1) / (r(k) + r(k1)), eeff = 1 + (er - 1) q, and Z0 = (60 pi / sqrt(eeff)) / (r(k) + r(k1)); `groundedCoplanarImpedance` is exactly this. The mapping treats the interface between the substrate and the air as a magnetic wall, which is exact only in the limit of a thick substrate or small gaps and is the source of the error below. Side grounds are semi-infinite; walls 16 substrate heights above.

| er | w/h | gap/w | eeff diff | Z0 diff |
|---|---|---|---|---|
| 4.4 | 0.5 | 0.25, 0.5, 1.0 | +0.01, -0.01, +0.10 % | +0.29, +0.46, +0.51 % |
| 4.4 | 1.0 | 0.25, 0.5, 1.0 | +0.00, +0.13, +0.59 % | +0.38, +0.47, +1.04 % |
| 9.8 | 0.5 | 0.25, 0.5, 1.0 | +0.01, -0.01, +0.15 % | +0.29, +0.46, +0.49 % |
| 9.8 | 1.0 | 0.25, 0.5, 1.0 | +0.01, +0.20, +0.87 % | +0.38, +0.44, +0.90 % |

The form agrees to 0.0 to 0.2 percent in eeff while the gap is at most half the substrate height (gap/h <= 0.5) and drifts to 0.6 to 0.9 percent when the gap equals the substrate height; Z0 reads 0.3 to 1.0 percent high throughout. The antenna feed lines of this project (gap 0.15 to 0.3 mm on 1.4 to 1.6 mm) are in the first regime. This replaces the "coplanar not confirmed" in `src/board/line_impedance.hpp`.

## 5. What these checks do not say
Both forms are for zero-thickness copper; real copper lowers a coplanar line's impedance 6 to 9 percent (the feed row solves it), and OSH Park's published 15 mil / 7.87 mil line (49.8 ohm, with solder mask) against 54.7 ohm (closed form), 51.0 ohm (2D solve with 45.7 um copper) shows the size. The 3D FDTD solver's own effective permittivity is a different question and not what these tables test (see `line-extraction-and-bends.md`).
