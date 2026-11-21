// Export of the display volume for ParaView and similar tools: the legacy VTK "STRUCTURED_POINTS" text format
// (an open, documented format: dimensions, origin and spacing, then one value per point, x varying fastest).
// Points are the voxel centres of the display volume, in metres; `shift` is added to the origin (use the
// board's offset to export in board coordinates). Docs/PLAN.md, Next steps 11h.
#pragma once
#include <array>
#include <iosfwd>
#include <string>
#include <vector>

#include "viz/volume.hpp"

namespace viz {

struct VtkScalar {
  std::string name;          // letters, digits and underscores (spaces are replaced)
  std::vector<float> values; // one per voxel, DisplayVolume::index order
};

// Throws std::invalid_argument if a scalar's size is not dv.voxels(). `title` must be one line (it is cut at
// the first newline and at 250 characters).
void writeVtkVolume(std::ostream& out, const DisplayVolume& dv, const std::vector<VtkScalar>& scalars,
                    const std::string& title, std::array<double, 3> shift = {0, 0, 0});

} // namespace viz
