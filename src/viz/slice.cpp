#include "viz/slice.hpp"

#include <algorithm>
#include <cmath>

#include "viz/colormap.hpp"

namespace viz {

Plane makePlane(const fdtd::Grid& g, fdtd::Axis normal, std::size_t index) {
  Plane p;
  p.normal = normal;
  p.index = index;
  p.u = normal == fdtd::X ? fdtd::Y : fdtd::X;
  p.v = normal == fdtd::Z ? fdtd::Y : fdtd::Z;
  p.nu = g.nodes(p.u);
  p.nv = g.nodes(p.v);
  return p;
}

std::vector<float> extractPlane(const fdtd::Grid& g, const Plane& p, std::span<const float> field) {
  std::vector<float> out(p.nu * p.nv);
  for (std::size_t b = 0; b < p.nv; ++b)
    for (std::size_t a = 0; a < p.nu; ++a) {
      std::array<std::size_t, 3> idx{};
      idx[p.normal] = p.index;
      idx[p.u] = a;
      idx[p.v] = b;
      out[a + p.nu * b] = field[g.nodeIndex(idx[0], idx[1], idx[2])];
    }
  return out;
}

namespace {
// Nearest node along an axis to position x.
std::size_t nearestNode(const fdtd::Grid& g, fdtd::Axis a, double x) {
  std::size_t lo = 0, hi = g.nodes(a) - 1;
  while (hi - lo > 1) {
    const std::size_t mid = (lo + hi) / 2;
    (g.edge(a, mid) <= x ? lo : hi) = mid;
  }
  return std::abs(g.edge(a, lo) - x) <= std::abs(g.edge(a, hi) - x) ? lo : hi;
}
} // namespace

SliceMap makeSliceMap(const fdtd::Grid& g, const Plane& p, const FrameSpec& spec) {
  SliceMap m;
  const bool crop = spec.cropU1 > spec.cropU0 && spec.cropV1 > spec.cropV0;
  const double u0 = crop ? spec.cropU0 : 0, v0 = crop ? spec.cropV0 : 0;
  const double lenU = crop ? spec.cropU1 - spec.cropU0 : g.length(p.u),
               lenV = crop ? spec.cropV1 - spec.cropV0 : g.length(p.v);
  double ppm = spec.pixelsPerMetre;
  double offX = 0, offY = 0; // content origin in the canvas, pixels
  if (spec.canvasWidth && spec.canvasHeight) {
    m.width = spec.canvasWidth;
    m.height = spec.canvasHeight;
    ppm = std::min(m.width / lenU, m.height / lenV);
    offX = 0.5 * (m.width - lenU * ppm);
    offY = 0.5 * (m.height - lenV * ppm);
  } else {
    m.width = static_cast<std::uint32_t>(std::ceil(lenU * ppm));
    m.height = static_cast<std::uint32_t>(std::ceil(lenV * ppm));
  }
  // Column and row to node maps; -1 marks pixels outside the plane (letterbox).
  m.colNode.assign(m.width, -1);
  m.rowNode.assign(m.height, -1);
  m.colLo.assign(m.width, 0);
  m.rowLo.assign(m.height, 0);
  m.colT.assign(m.width, 0);
  m.rowT.assign(m.height, 0);
  // The node on each side of x along an axis, and how far x is between them (bilinear sampling between
  // nodes is smooth, unlike nearest-node pixels).
  auto lower = [&](fdtd::Axis a, double x, std::size_t& i0, double& t) {
    std::size_t lo = 0, hi = g.nodes(a) - 1;
    while (hi - lo > 1) {
      const std::size_t mid = (lo + hi) / 2;
      (g.edge(a, mid) <= x ? lo : hi) = mid;
    }
    i0 = lo;
    t = std::clamp((x - g.edge(a, lo)) / (g.edge(a, lo + 1) - g.edge(a, lo)), 0.0, 1.0);
  };
  for (std::uint32_t x = 0; x < m.width; ++x) {
    const double u = (x + 0.5 - offX) / ppm;
    if (u < 0 || u > lenU || u0 + u < 0 || u0 + u > g.length(p.u))
      continue;
    m.colNode[x] = static_cast<long>(nearestNode(g, p.u, u0 + u));
    lower(p.u, u0 + u, m.colLo[x], m.colT[x]);
  }
  for (std::uint32_t y = 0; y < m.height; ++y) { // image row 0 is the top: largest v
    const double v = lenV - (y + 0.5 - offY) / ppm;
    if (v < 0 || v > lenV || v0 + v < 0 || v0 + v > g.length(p.v))
      continue;
    m.rowNode[y] = static_cast<long>(nearestNode(g, p.v, v0 + v));
    lower(p.v, v0 + v, m.rowLo[y], m.rowT[y]);
  }
  return m;
}

std::vector<std::uint8_t> renderFrame(const fdtd::Grid& g, const Plane& p, const PhasorAccumulator& acc,
                                      std::size_t k, double phi, const FrameSpec& spec,
                                      const std::vector<std::uint8_t>& mask, std::uint32_t& width,
                                      std::uint32_t& height) {
  const SliceMap map = makeSliceMap(g, p, spec);
  width = map.width;
  height = map.height;
  const auto& colNode = map.colNode;
  const auto& rowNode = map.rowNode;
  const auto& colLo = map.colLo;
  const auto& rowLo = map.rowLo;
  const auto& colT = map.colT;
  const auto& rowT = map.rowT;
  // One frame of the field on the nodes.
  std::vector<float> field(p.nu * p.nv);
  for (std::size_t n = 0; n < field.size(); ++n)
    field[n] = acc.steady(k, n, phi);
  const Rgb background = spec.lightTheme ? Rgb{238, 238, 241} : Rgb{20, 20, 24};
  std::vector<std::uint8_t> rgb(static_cast<std::size_t>(width) * height * 3);
  for (std::uint32_t y = 0; y < height; ++y)
    for (std::uint32_t x = 0; x < width; ++x) {
      Rgb c = background;
      if (colNode[x] >= 0 && rowNode[y] >= 0) {
        const std::size_t node =
            static_cast<std::size_t>(colNode[x]) + p.nu * static_cast<std::size_t>(rowNode[y]);
        if (!mask.empty() && mask[node]) {
          c = spec.lightTheme ? Rgb{70, 70, 82} : Rgb{110, 110, 110};
        } else {
          const std::size_t i0 = colLo[x], j0 = rowLo[y];
          const std::size_t i1 = std::min(i0 + 1, p.nu - 1), j1 = std::min(j0 + 1, p.nv - 1);
          const double tx = colT[x], ty = rowT[y];
          const double v = (field[i0 + p.nu * j0] * (1 - tx) + field[i1 + p.nu * j0] * tx) * (1 - ty) +
                           (field[i0 + p.nu * j1] * (1 - tx) + field[i1 + p.nu * j1] * tx) * ty;
          const double s = v / spec.colourScale;
          c = spec.lightTheme ? divergingLight(static_cast<float>(s), static_cast<float>(spec.gamma),
                                               spec.negative, spec.positive)
                              : divergingDark(static_cast<float>(s), static_cast<float>(spec.gamma),
                                              spec.negative, spec.positive);
        }
      }
      std::uint8_t* px = &rgb[(static_cast<std::size_t>(y) * width + x) * 3];
      px[0] = c.r;
      px[1] = c.g;
      px[2] = c.b;
    }
  return rgb;
}

} // namespace viz
