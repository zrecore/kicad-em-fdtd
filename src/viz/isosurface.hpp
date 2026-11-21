// A threshold surface of the display volume (the set of points where the field equals a level), made by
// marching tetrahedra: each voxel cell is cut into six tetrahedra and each tetrahedron contributes zero, one
// or two triangles with vertices interpolated linearly along its edges. No lookup tables, so nothing to
// license beyond the idea (Doi and Koide's decomposition is textbook). Docs/PLAN.md, Next steps 11h.
#pragma once
#include <array>
#include <iosfwd>
#include <vector>

#include "viz/volume.hpp"

namespace viz {

struct Triangle {
  std::array<std::array<float, 3>, 3> v; // grid coordinates, metres
};

// The surface where `field` (one value per voxel, DisplayVolume::index order, sampled at voxel centres)
// crosses `level`. A level equal to a sample value is treated as just above it. Empty if the field never
// crosses the level.
std::vector<Triangle> isosurface(const DisplayVolume& dv, const std::vector<float>& field, float level);

double surfaceArea(const std::vector<Triangle>& mesh); // square metres

// Wavefront OBJ (an open text format): vertices in metres plus `shift`, one face per triangle.
void writeObj(std::ostream& out, const std::vector<Triangle>& mesh, std::array<double, 3> shift = {0, 0, 0});

} // namespace viz
