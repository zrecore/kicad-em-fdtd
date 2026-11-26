// Closed-form characteristic impedance of the two feed lines a PCB antenna is usually fed through: a
// microstrip over a ground plane and a coplanar waveguide with ground below it (CPWG). Zero-thickness
// conductors, lossless, quasi-static. Checked against this project's own 2D field solver (src/fdtd/line2d.*,
// docs/methods/microstrip-closed-forms.md): microstrip within 0.3 percent in the effective permittivity and
// +0.1 to +0.5 percent in Z0 over width ratios 0.1 to 8 and permittivities 2.2 to 9.8; coplanar within 0.2
// percent (eeff) while gap <= h / 2 and within 1 percent for gap <= h. The same formulas are in
// scripts/antenna/lines.py. Pure logic.
#pragma once

namespace board {

struct LineImpedance {
  double z0Ohm = 0;
  double epsEff = 0;
};

// Microstrip of width w over a substrate of height h (both metres) and relative permittivity epsR.
// Hammerstad and Jensen, IEEE MTT-S Digest 1980, pp. 407-409, doi:10.1109/MWSYM.1980.1124303 (DOI checked on
// Crossref; the paper is paywalled and was not read: the equations are from two open secondary sources).
LineImpedance microstripImpedance(double w, double h, double epsR);

// Coplanar waveguide with a ground plane below: strip width w, gap to the side grounds `gap`, substrate
// height h. Conformal mapping, as in R. N. Simons, Coplanar Waveguide Circuits, Components, and Systems
// (Wiley 2001) chapter 3, doi:10.1002/0471224758.ch3 (DOI checked on Crossref, book not read; equations
// checked against the Qucs technical documentation). Infinitely wide side grounds.
LineImpedance groundedCoplanarImpedance(double w, double gap, double h, double epsR);

// K(k) / K'(k) for the complete elliptic integral of the first kind, 0 < k < 1.
double ellipticRatio(double k);

} // namespace board
