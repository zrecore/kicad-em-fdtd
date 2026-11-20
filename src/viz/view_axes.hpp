// Standard views for the 3D camera and the screen layout of the axis triad (the corner gizmo whose six ends
// snap the camera to the +/-X, +/-Y and +/-Z views). Pure geometry, no drawing, so it is unit-tested.
#pragma once
#include <array>
#include <cmath>
#include <numbers>

#include "viz/volume.hpp"

namespace viz {

// Elevation limit shared by dragging and the top/bottom views (the camera's up vector is +z, so exactly
// +-90 degrees would be degenerate).
inline constexpr double kMaxElevation = std::numbers::pi / 2 - 1e-3;

enum class StdView { PlusX, MinusX, PlusY, MinusY, PlusZ, MinusZ, Isometric };

struct Pose {
  double azimuth, elevation;
};

// The camera sits on the named side of the board and looks at it: "+X" looks from +x towards -x.
inline Pose stdViewPose(StdView v) {
  using std::numbers::pi;
  switch (v) {
  case StdView::PlusX:
    return {0, 0};
  case StdView::MinusX:
    return {pi, 0};
  case StdView::PlusY:
    return {pi / 2, 0};
  case StdView::MinusY:
    return {-pi / 2, 0};
  case StdView::PlusZ:
    return {0, kMaxElevation};
  case StdView::MinusZ:
    return {0, -kMaxElevation};
  case StdView::Isometric:
    return {pi / 4, std::atan(1.0 / std::numbers::sqrt2)};
  }
  return {0, 0};
}

// One step of the glide towards a pose: azimuth takes the short way round, `ease` in (0, 1] is the fraction
// of the remaining distance covered (1 = jump, used under Reduce Motion).
inline Pose easePose(Pose from, Pose to, double ease) {
  using std::numbers::pi;
  double d = std::fmod(to.azimuth - from.azimuth, 2 * pi);
  if (d > pi)
    d -= 2 * pi;
  if (d < -pi)
    d += 2 * pi;
  return {from.azimuth + ease * d, from.elevation + ease * (to.elevation - from.elevation)};
}

// Where an axis end lands on screen for a camera: x right and y down in units of the triad's arm length,
// and depth > 0 when the end points towards the viewer (drawn on top).
struct AxisEnd {
  StdView view;
  char axis; // 'X', 'Y' or 'Z'
  bool plus;
  double x, y, depth;
};

inline std::array<AxisEnd, 6> axisTriad(double azimuth, double elevation) {
  const double ce = std::cos(elevation), se = std::sin(elevation), ca = std::cos(azimuth),
               sa = std::sin(azimuth);
  const std::array<double, 3> toCamera{ce * ca, ce * sa, se};
  const std::array<double, 3> right{-sa, ca, 0};
  const std::array<double, 3> up{-se * ca, -se * sa, ce};
  auto end = [&](StdView v, char axis, bool plus, std::array<double, 3> a) {
    const double s = plus ? 1.0 : -1.0;
    auto dot = [&](const std::array<double, 3>& b) { return s * (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]); };
    return AxisEnd{v, axis, plus, dot(right), -dot(up), dot(toCamera)};
  };
  return {end(StdView::PlusX, 'X', true, {1, 0, 0}), end(StdView::MinusX, 'X', false, {1, 0, 0}),
          end(StdView::PlusY, 'Y', true, {0, 1, 0}), end(StdView::MinusY, 'Y', false, {0, 1, 0}),
          end(StdView::PlusZ, 'Z', true, {0, 0, 1}), end(StdView::MinusZ, 'Z', false, {0, 0, 1})};
}

inline const char* stdViewName(StdView v) {
  switch (v) {
  case StdView::PlusX:
    return "+X";
  case StdView::MinusX:
    return "-X";
  case StdView::PlusY:
    return "+Y";
  case StdView::MinusY:
    return "-Y";
  case StdView::PlusZ:
    return "+Z";
  case StdView::MinusZ:
    return "-Z";
  case StdView::Isometric:
    return "Isometric";
  }
  return "";
}

} // namespace viz
