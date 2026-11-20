// Units: the core works in SI (metres, seconds, hertz). Millimetres and KiCAD's integer nanometres
// exist only at the board boundary and are converted here, once.
#pragma once
#include <cstdint>

namespace fdtd::units {

constexpr double kSpeedOfLight = 299792458.0;                          // m/s
constexpr double kMu0 = 1.25663706212e-6;                              // H/m (CODATA 2018)
constexpr double kEps0 = 1.0 / (kMu0 * kSpeedOfLight * kSpeedOfLight); // F/m, consistent with c

constexpr double mm_to_m(double mm) {
  return mm * 1e-3;
}
constexpr double nm_to_m(std::int64_t nm) {
  return static_cast<double>(nm) * 1e-9;
}
constexpr double ghz_to_hz(double ghz) {
  return ghz * 1e9;
}

} // namespace fdtd::units
