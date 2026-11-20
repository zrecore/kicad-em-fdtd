// 3D volume view: a display grid sampled from the solver grid, phasor reconstruction of one frame, and a CPU
// ray marcher (front-to-back compositing, early termination, empty-space skipping). The accumulator is
// viz::PhasorAccumulator over the display voxels, the same type the slice view uses.
#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "fdtd/grid.hpp"
#include "viz/colormap.hpp"
#include "viz/phasor.hpp"

namespace viz {

struct VolumeBox { // region of the solver grid to display, grid coordinates in metres; hi <= lo = whole grid
  std::array<double, 3> lo{0, 0, 0}, hi{0, 0, 0};
};

struct DisplayVolume {
  std::array<std::size_t, 3> dims{};
  std::array<std::vector<std::size_t>, 3> nodeOf; // per axis and display index: nearest solver node
  std::array<double, 3> origin{}, extent{};       // box in grid coordinates (metres)
  std::size_t voxels() const { return dims[0] * dims[1] * dims[2]; }
  std::size_t index(std::size_t i, std::size_t j, std::size_t k) const {
    return i + dims[0] * (j + dims[1] * k);
  }
};

// At most maxDim voxels along the longest side of the box; the other sides keep the aspect ratio.
DisplayVolume makeDisplayVolume(const fdtd::Grid& g, std::size_t maxDim, const VolumeBox& box);

// Samples a node-sized solver field at the display voxels (nearest node).
void sampleField(const fdtd::Grid& g, const DisplayVolume& dv, std::span<const float> field,
                 std::vector<float>& out);

// Re{X_k exp(j phi)} at every voxel.
void reconstruct(const PhasorAccumulator& acc, std::size_t k, double phi, std::vector<float>& out);

struct Camera {
  double azimuth = 0.8, elevation = 0.55; // radians; z is up
  double distance = 2.0;                  // in units of the box's longest side
  double fovY = 0.7;                      // radians
  std::array<double, 3> target{0, 0, 0};  // look-at point, in the box's normalised units (longest side 1)
};

struct RenderSpec {
  std::uint32_t width = 400, height = 300;
  double colourScale = 1; // field value that maps to full colour
  double density = 5;     // opacity per box length at full colour
  double gamma = 0.7;
  double threshold =
      0.04; // |value| / scale (or the dB fraction below) under which a voxel is treated as empty
  // > 0: show this many dB below colourScale instead of a linear scale. Near fields fall off steeply, so a dB
  // scale is what makes the structure visible away from the strongest cells.
  double logRangeDb = 0;
  Rgb negative = kDarkNegative,
      positive = kDarkPositive; // strong-field colours (the viewport is always dark)
  bool lightTheme =
      false;             // light background and absorption-style colours (negative/positive = the light pair)
  double emission = 2.5; // brightness gain of the accumulated colour (the glow); background is not scaled
  bool skipEmpty = true;
  int threads = 0; // 0 = hardware concurrency
};

inline constexpr Rgb kBackground{20, 20, 24};

std::vector<std::uint8_t> renderVolume(const DisplayVolume& dv, const std::vector<float>& values,
                                       const Camera& cam, const RenderSpec& spec);

// Aims the camera at the bounding box of the voxels whose |value| is above 1 percent of colourScale (or, with
// logRangeDb > 0, inside that many dB of it) and sets the distance to frame it. Leaves the camera alone if
// nothing is active. Direction (azimuth, elevation) is kept.
void fitCamera(const DisplayVolume& dv, const std::vector<float>& values, double colourScale,
               double logRangeDb, Camera& cam, double margin = 1.25);

// Pans the camera as if the scene were dragged by (dxPixels, dyPixels) on a view heightPixels tall: moves the
// look-at point in the view plane, so the picture follows the pointer. Rotation and distance are unchanged.
void panCamera(const DisplayVolume& dv, Camera& cam, double dxPixels, double dyPixels, double heightPixels);

// Projects a point (grid coordinates, metres) to pixel coordinates for the same camera; false if behind it.
bool projectPoint(const DisplayVolume& dv, const Camera& cam, std::uint32_t width, std::uint32_t height,
                  const std::array<double, 3>& gridPoint, float& px, float& py);

} // namespace viz
