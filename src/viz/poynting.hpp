// Time-averaged Poynting vector from E and H phasors: S = 1/2 Re(E x conj(H)), in W/m^2 when E is in V/m and
// H in A/m. The phasors need only share one time origin (the accumulator guarantees it); the result does not
// depend on it. On a Yee grid E and H sit half a cell apart, so a vector built from the six components at one
// node index is accurate to first order in the cell size (docs/PLAN.md, Next steps 11c).
#pragma once
#include <array>
#include <complex>

namespace viz {

using Phasor3 = std::array<std::complex<double>, 3>;

inline std::array<double, 3> poynting(const Phasor3& e, const Phasor3& h) {
  auto term = [&](int a, int b) { return (e[a] * std::conj(h[b]) - e[b] * std::conj(h[a])).real() * 0.5; };
  return {term(1, 2), term(2, 0), term(0, 1)};
}

} // namespace viz
