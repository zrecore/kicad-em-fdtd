// Field probes: points in the board's coordinates where the electric field is recorded as a spectrum over the
// run (the same running DFT as everything else), for near-field checks and hot-spot hunting. Docs/PLAN.md,
// Next steps 11h.
#pragma once
#include <complex>
#include <cstdint>
#include <string>
#include <vector>

#include "fdtd/grid.hpp"

namespace app {

struct ProbeSpec {
  double xMm = 0, yMm = 0, zMm = 0; // board x and y in millimetres; z above the bottom copper plane
};

// "x,y,z; x,y,z" in millimetres (a semicolon or a newline separates probes). Empty text gives no probes.
// Throws SettingsError for a bad number or a probe with fewer or more than three coordinates, or more than 16
// probes.
std::vector<ProbeSpec> parseProbes(const std::string& text);

// The nearest E node of the grid to the probe, given the board's offset into the grid (model.originX/Y,
// metres) and the grid z of the bottom copper plane (metres).
std::uint32_t probeNode(const fdtd::Grid& grid, const ProbeSpec& probe, double originX, double originY,
                        double bottomZ);

// Column text for a CSV: "f_hz,probe1_re,probe1_im,..." one row per frequency.
std::string probesCsv(const std::vector<double>& frequenciesHz, const std::vector<ProbeSpec>& probes,
                      const std::vector<std::vector<std::complex<double>>>& phasors); // [probe][frequency]

} // namespace app
