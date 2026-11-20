// Ear-clipping triangulation of a simple polygon (the outline of a copper shape), for the translucent fills
// in the 3D view. Robust to what real boards produce: either winding, repeated and collinear points, a
// repeated closing point. Holes are not handled (the caller fills the outer ring). Returns triangles as
// indices into the input; empty when the polygon is degenerate or the clipping cannot finish (the caller then
// draws the outline only). ImGui's AddConcavePolyFilled was used before and drew huge wrong triangles on
// such rings.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace viz {

inline std::vector<std::array<std::size_t, 3>> triangulate(const std::vector<std::array<double, 2>>& pts,
                                                           std::size_t maxPoints = 4000) {
  using P = std::array<double, 2>;
  std::vector<std::size_t> ring;
  for (std::size_t i = 0; i < pts.size(); ++i)
    if (ring.empty() || pts[i] != pts[ring.back()])
      ring.push_back(i);
  while (ring.size() > 1 && pts[ring.front()] == pts[ring.back()])
    ring.pop_back();
  if (ring.size() < 3 || ring.size() > maxPoints)
    return {};
  auto cross = [&](const P& o, const P& a, const P& b) {
    return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
  };
  double area2 = 0, scale = 0;
  for (std::size_t i = 0; i < ring.size(); ++i) {
    const P &a = pts[ring[i]], &b = pts[ring[(i + 1) % ring.size()]];
    area2 += a[0] * b[1] - b[0] * a[1];
    scale = std::max({scale, std::abs(a[0]), std::abs(a[1])});
  }
  if (std::abs(area2) <= 1e-12 * std::max(scale * scale, 1e-300))
    return {};
  if (area2 < 0)
    std::reverse(ring.begin(), ring.end()); // counter-clockwise from here on
  const double eps = 1e-12 * std::max(scale * scale, 1e-300);

  std::vector<std::array<std::size_t, 3>> out;
  while (ring.size() > 3) {
    const std::size_t n = ring.size();
    bool clipped = false;
    for (std::size_t i = 0; i < n && !clipped; ++i) {
      const std::size_t ia = ring[(i + n - 1) % n], ib = ring[i], ic = ring[(i + 1) % n];
      const P &a = pts[ia], &b = pts[ib], &c = pts[ic];
      const double turn = cross(a, b, c);
      if (turn <= eps) {             // collinear or reflex
        if (std::abs(turn) <= eps) { // collinear: dropping it keeps the shape
          ring.erase(ring.begin() + static_cast<std::ptrdiff_t>(i));
          clipped = true;
        }
        continue;
      }
      bool empty = true;
      for (std::size_t j = 0; j < n && empty; ++j) {
        const std::size_t ip = ring[j];
        if (ip == ia || ip == ib || ip == ic)
          continue;
        const P& p = pts[ip];
        if (p == a || p == b || p == c)
          continue;
        empty = !(cross(a, b, p) >= -eps && cross(b, c, p) >= -eps && cross(c, a, p) >= -eps);
      }
      if (empty) {
        out.push_back({ia, ib, ic});
        ring.erase(ring.begin() + static_cast<std::ptrdiff_t>(i));
        clipped = true;
      }
    }
    if (!clipped)
      return {}; // self-intersecting or otherwise not simple: no fill rather than a wrong one
  }
  out.push_back({ring[0], ring[1], ring[2]});
  return out;
}

} // namespace viz
