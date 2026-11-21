// Near-to-far-field transform on a closed box of Yee samples (a Huygens surface): the equivalent surface
// currents J = n x H and M = -n x E are read from accumulated phasors of E and H on the box faces and
// integrated against the free-space Green's function. Written from the textbook formulation (radiation
// integrals N and L, U = k^2 / (32 pi^2 eta) (|L_phi + eta N_theta|^2 + |L_theta - eta N_phi|^2)); see
// docs/PLAN.md, References [28] (Luebbers et al.; not read), and the checks in tests/cpu/ntff_test.cpp.
//
// Conventions: phasors are the running DFT X = sum f(t) exp(-j w t) dt of a real signal, which is the
// engineering phasor (exp(+j w t)) times a constant; every power-like result is therefore in units of that
// constant squared, and only ratios against another quantity made the same way (the port's input power) or
// the directivity (a ratio) are physical.
#pragma once
#include <array>
#include <complex>
#include <cstdint>
#include <functional>
#include <vector>

#include "fdtd/grid.hpp"
#include "fdtd/phasor_bank.hpp"

namespace fdtd {

struct NtffElement {
  char type;                  // 'J' (electric) or 'M' (magnetic) surface current
  int axis;                   // the component it points along
  std::array<double, 3> pos;  // metres, grid coordinates
  double weight;              // area it represents, m^2
  double sign;                // value = sign * mean of the channels
  std::uint32_t chanA, chanB; // chanB == UINT32_MAX: a single channel
};

// 1/2 Re(E conj(H)) x area x sign, for a lateral sample where E and H sit together: the surface flux check.
struct NtffFluxTerm {
  double sign, weight;
  std::uint32_t chanE, chanHa, chanHb; // H is the mean of the two samples half a cell either side
};

struct NtffPlan {
  std::vector<PhasorChannel> channels;
  std::vector<NtffElement> elements;
  std::vector<NtffFluxTerm> flux;
  std::array<double, 3> centre{}; // phase origin
};

// A box with its faces on the node planes lo[a] and hi[a] along every axis a (lo < hi).
NtffPlan makeNtffPlan(const Grid& grid, std::array<std::size_t, 3> lo, std::array<std::size_t, 3> hi);

using PhasorOf = std::function<std::complex<double>(std::uint32_t channel)>;

struct FarField {
  std::size_t nTheta = 0, nPhi = 0;
  std::vector<double> theta, phi; // radians, cell centres of the angular grid
  std::vector<double> u;          // radiation intensity [theta * nPhi + phi], phasor convention
  double power = 0;               // integral of u over the sphere
  double peak = 0;                // largest u
  std::size_t peakTheta = 0, peakPhi = 0;
  double directivity() const { return power > 0 ? 4 * 3.14159265358979323846 * peak / power : 0; }
};

// Radiation intensity on an nTheta x nPhi grid at one frequency, using all hardware threads.
FarField farField(const NtffPlan& plan, const PhasorOf& phasor, double frequencyHz, std::size_t nTheta,
                  std::size_t nPhi);

// The radiated power directly from the surface: 1/2 Re of the Poynting flux through the box, phasor
// convention, for the same phasors. For fields that satisfy Maxwell's equations it equals farField().power.
double surfaceFlux(const NtffPlan& plan, const PhasorOf& phasor);

// What an antenna's far field and its port say together, at one frequency. Inputs: the far field (transform
// phasors), Vs, the source's phasor made the same way (a transform of the same kind) and S11 at the port of
// reference impedance z0. With the source of series resistance z0 the power it can deliver is |Vs|^2 / (4 z0)
// and the power accepted is that times (1 - |S11|^2) (transform convention: twice the phasor values, so the
// far field's phasor-convention power is doubled to match).
struct AntennaFigures {
  double radiatedPower = 0;  // 2 x the integral of U, transform convention
  double availablePower = 0; // |Vs|^2 / (4 z0)
  double acceptedPower = 0;  // available x (1 - |S11|^2)
  double efficiency = 0;     // radiated / accepted: radiation efficiency (losses in the model only)
  double directivity = 0;    // 4 pi U_peak / integral of U
  double gain = 0;           // efficiency x directivity (IEEE gain: referenced to the accepted power)
  double realisedGain = 0;   // gain x (1 - |S11|^2): referenced to the available power, mismatch included
  double peakThetaDeg = 0, peakPhiDeg = 0;
};
// `accepted`, if not negative, is the accepted power measured strip by strip (fdtd::acceptedPower) and
// replaces the S11-based one, which is wrong when the port's strips see different voltages.
AntennaFigures antennaFigures(const FarField& ff, std::complex<double> vs, std::complex<double> s11,
                              double z0, double accepted = -1);

} // namespace fdtd
