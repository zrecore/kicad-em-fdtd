#include "fdtd/grid.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "core/units.hpp"

namespace fdtd {

Grid::Grid(std::array<std::vector<double>, 3> edges) : edges_(std::move(edges)) {
  for (const auto& e : edges_) {
    if (e.size() < 2)
      throw std::invalid_argument("each axis needs at least one cell");
    for (std::size_t i = 1; i < e.size(); ++i)
      if (!(e[i] > e[i - 1]))
        throw std::invalid_argument("grid edges must be strictly increasing");
  }
}

Grid Grid::uniform(std::array<std::size_t, 3> cells, std::array<double, 3> size) {
  std::array<std::vector<double>, 3> edges;
  for (int a = 0; a < 3; ++a) {
    edges[a].resize(cells[a] + 1);
    for (std::size_t i = 0; i <= cells[a]; ++i)
      edges[a][i] = size[a] * static_cast<double>(i) / static_cast<double>(cells[a]);
  }
  return Grid(std::move(edges));
}

double Grid::dualSize(Axis a, std::size_t i) const {
  const std::size_t n = cells(a);
  if (i == 0)
    return 0.5 * cellSize(a, 0);
  if (i == n)
    return 0.5 * cellSize(a, n - 1);
  return 0.5 * (cellSize(a, i - 1) + cellSize(a, i));
}

double Grid::minCellSize(Axis a) const {
  double m = cellSize(a, 0);
  for (std::size_t i = 1; i < cells(a); ++i)
    m = std::min(m, cellSize(a, i));
  return m;
}

double maxStableDt(const Grid& g) {
  double sum = 0;
  for (Axis a : {X, Y, Z})
    sum += 1.0 / (g.minCellSize(a) * g.minCellSize(a));
  return 1.0 / (units::kSpeedOfLight * std::sqrt(sum));
}

} // namespace fdtd
