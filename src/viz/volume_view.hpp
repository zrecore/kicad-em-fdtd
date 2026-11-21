// The camera frame shared by the CPU and the GPU volume renderers: the box is centred at the origin with its
// longest side 1, so the camera distance is scale-free.
#pragma once
#include <algorithm>
#include <cmath>

#include "viz/volume.hpp"

namespace viz {

struct Vec {
  double x, y, z;
};
inline Vec operator+(Vec a, Vec b) {
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline Vec operator-(Vec a, Vec b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline Vec operator*(Vec a, double s) {
  return {a.x * s, a.y * s, a.z * s};
}
inline double dot(Vec a, Vec b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline Vec cross(Vec a, Vec b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline Vec norm(Vec a) {
  return a * (1.0 / std::sqrt(dot(a, a)));
}

// The box is drawn centred at the origin with its longest side 1, so the camera distance is scale-free.
struct View {
  Vec size, pos, forward, right, up;
};
inline View makeView(const DisplayVolume& dv, const Camera& c) {
  const double longest = std::max({dv.extent[0], dv.extent[1], dv.extent[2]});
  View v;
  v.size = {dv.extent[0] / longest, dv.extent[1] / longest, dv.extent[2] / longest};
  const Vec target{c.target[0], c.target[1], c.target[2]}; // the point the camera looks at, orbits and pans
  const Vec dir{std::cos(c.elevation) * std::cos(c.azimuth), std::cos(c.elevation) * std::sin(c.azimuth),
                std::sin(c.elevation)};
  v.pos = target + dir * c.distance;
  v.forward = norm(Vec{0, 0, 0} - dir);
  v.right = norm(cross(v.forward, Vec{0, 0, 1}));
  v.up = cross(v.right, v.forward);
  return v;
}

} // namespace viz
