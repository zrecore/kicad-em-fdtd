// Quantities derived from a reflection coefficient: input impedance, VSWR, return loss, mismatch loss.
// Z = z0 (1 + G) / (1 - G); VSWR = (1 + |G|) / (1 - |G|); RL = -20 log10 |G|; ML = -10 log10 (1 - |G|^2).
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>

namespace fdtd {

inline std::complex<double> impedanceFromGamma(std::complex<double> gamma, double z0) {
  const std::complex<double> one(1.0, 0.0);
  const auto den = one - gamma;
  if (std::abs(den) < 1e-12)
    return {std::numeric_limits<double>::infinity(), 0.0}; // an open circuit
  return z0 * (one + gamma) / den;
}

// Infinity for |G| >= 1 (everything is reflected).
inline double vswr(std::complex<double> gamma) {
  const double g = std::abs(gamma);
  return g >= 1.0 ? std::numeric_limits<double>::infinity() : (1 + g) / (1 - g);
}

inline double returnLossDb(std::complex<double> gamma) {
  return -20 * std::log10(std::max(std::abs(gamma), 1e-12));
}

// The power lost to the mismatch, as a positive number of dB (0 for a perfect match).
inline double mismatchLossDb(std::complex<double> gamma) {
  const double g2 = std::min(std::norm(gamma), 1.0 - 1e-12);
  return -10 * std::log10(1 - g2);
}

} // namespace fdtd
