// The saved field data of a run, so a finished run can be opened again without solving (docs/PLAN.md,
// Next steps 16). A phasor file: magic "EMFP", version, frequency and voxel counts, the frequencies, then per
// frequency a float scale and the real and imaginary parts as IEEE half floats of value / scale (solver-unit
// values are far below half precision's range, so each frequency is scaled to its own peak). Little-endian.
#pragma once
#include <filesystem>
#include <memory>

#include "viz/phasor.hpp"

namespace app {

void savePhasors(const std::filesystem::path& file, const viz::PhasorAccumulator& acc);

// Throws std::runtime_error for a missing, foreign, truncated or too-new file.
std::unique_ptr<viz::PhasorAccumulator> loadPhasors(const std::filesystem::path& file);

} // namespace app
