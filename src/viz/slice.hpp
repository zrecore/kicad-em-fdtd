// Slice view of a field on the (graded) solver grid: extract a plane, accumulate phasors on it, render
// frames.
#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "fdtd/grid.hpp"
#include "viz/colormap.hpp"
#include "viz/phasor.hpp"

namespace viz {

struct Plane {
  fdtd::Axis normal; // the plane is at node `index` along this axis
  std::size_t index;
  fdtd::Axis u, v; // in-plane axes (increasing axis order)
  std::size_t nu = 0, nv = 0;
};

Plane makePlane(const fdtd::Grid& g, fdtd::Axis normal, std::size_t index);

// Values of a node-sized field on the plane, u fastest.
std::vector<float> extractPlane(const fdtd::Grid& g, const Plane& p, std::span<const float> field);

struct FrameSpec {
  double pixelsPerMetre = 6000; // 6 px per mm
  double colourScale = 1;       // field value that maps to full colour
  double gamma = 0.5;           // <1 lifts weak fields
  // Light panel: white-centred colour map on a light background, the way published field plots look.
  bool lightTheme = false;
  Rgb negative = kDarkNegative, positive = kDarkPositive; // strong-field colours for the panel's theme
  // If non-zero the frame is exactly this size and the plane is fitted and centred in it (a window).
  std::uint32_t canvasWidth = 0, canvasHeight = 0;
  // Visible part of the plane in grid coordinates (metres); unused when cropU1 <= cropU0.
  double cropU0 = 0, cropU1 = 0, cropV0 = 0, cropV1 = 0;
};

// Where each pixel of a frame sits on the plane: the nearest node (for the copper mask), the node below and
// the fraction towards the next one (for the bilinear sample); -1 marks pixels outside the plane. Cheap
// (width + height searches), so the GPU renderer takes it from here and shades the pixels itself.
struct SliceMap {
  std::uint32_t width = 0, height = 0;
  std::vector<long> colNode, rowNode;
  std::vector<std::size_t> colLo, rowLo;
  std::vector<double> colT, rowT;
};
SliceMap makeSliceMap(const fdtd::Grid& g, const Plane& p, const FrameSpec& spec);

// RGB frame (top row first) of Re{X exp(j phi)} for frequency k over the plane's extent, nearest-node
// sampling on the graded grid. mask (nu*nv, may be empty) marks copper nodes drawn as a grey overlay.
std::vector<std::uint8_t> renderFrame(const fdtd::Grid& g, const Plane& p, const PhasorAccumulator& acc,
                                      std::size_t k, double phi, const FrameSpec& spec,
                                      const std::vector<std::uint8_t>& mask, std::uint32_t& width,
                                      std::uint32_t& height);

} // namespace viz
