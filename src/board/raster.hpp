// A grid of cells painted from copper polygons: copper is a union of overlapping shapes (zones, pads,
// tracks), so areas and gaps come from painting every shape into cells (even-odd, so holes stay empty) and
// reading the cells. Each painted cell holds the label of the shape that painted it last (1 by default; 0 is
// empty). Pure logic, shared by the antenna checks and the refinement suggestions.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "board/snapshot.hpp"

namespace board {

struct Raster {
  double u0, v0, cell;
  int nu, nv;
  std::vector<std::uint16_t> on; // 0 empty, else the label painted
  Raster(double u0_, double v0_, double u1, double v1, double cell_)
      : u0(u0_), v0(v0_), cell(cell_), nu(std::max(1, static_cast<int>(std::ceil((u1 - u0_) / cell_)))),
        nv(std::max(1, static_cast<int>(std::ceil((v1 - v0_) / cell_)))),
        on(static_cast<std::size_t>(nu) * static_cast<std::size_t>(nv), 0) {}
  bool at(int i, int j) const { return i >= 0 && j >= 0 && i < nu && j < nv && on[index(i, j)] != 0; }
  std::uint16_t labelAt(int i, int j) const {
    return i >= 0 && j >= 0 && i < nu && j < nv ? on[index(i, j)] : std::uint16_t{0};
  }
  std::size_t index(int i, int j) const {
    return static_cast<std::size_t>(j) * static_cast<std::size_t>(nu) + static_cast<std::size_t>(i);
  }
  // Paints the shape (rings in this raster's frame) with `label`; returns the cells it covers.
  std::size_t paint(const std::vector<const Ring*>& rings, std::uint16_t label = 1) {
    std::vector<std::vector<double>> xs(static_cast<std::size_t>(nv));
    for (const Ring* r : rings)
      for (std::size_t i = 0, n = r->size(); i < n; ++i) {
        const Point a = (*r)[i], b = (*r)[(i + 1) % n];
        if (a.y == b.y)
          continue;
        const double lo = std::min(a.y, b.y), hi = std::max(a.y, b.y);
        const int j0 = std::max(0, static_cast<int>(std::ceil((lo - v0) / cell - 0.5)));
        const int j1 = std::min(nv - 1, static_cast<int>(std::ceil((hi - v0) / cell - 0.5)) - 1);
        for (int j = j0; j <= j1; ++j) {
          const double yc = v0 + (j + 0.5) * cell;
          if (yc >= lo && yc < hi)
            xs[static_cast<std::size_t>(j)].push_back(a.x + (yc - a.y) / (b.y - a.y) * (b.x - a.x));
        }
      }
    std::size_t covered = 0;
    for (int j = 0; j < nv; ++j) {
      auto& row = xs[static_cast<std::size_t>(j)];
      std::sort(row.begin(), row.end());
      for (std::size_t k = 0; k + 1 < row.size(); k += 2) {
        const int i0 = std::max(0, static_cast<int>(std::ceil((row[k] - u0) / cell - 0.5)));
        const int i1 = std::min(nu - 1, static_cast<int>(std::ceil((row[k + 1] - u0) / cell - 0.5)) - 1);
        for (int i = i0; i <= i1; ++i) {
          on[index(i, j)] = label;
          ++covered;
        }
      }
    }
    return covered;
  }
  double areaM2() const {
    return static_cast<double>(std::count_if(on.begin(), on.end(), [](std::uint16_t v) { return v != 0; })) *
           cell * cell;
  }
};

} // namespace board
