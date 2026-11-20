#include "board/mesh.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace board {

std::vector<double> gradedEdges(double lo, double hi, double hMax, const std::vector<Refinement>& fine,
                                double ratio, const std::vector<double>& snap) {
  if (!(hi > lo) || !(hMax > 0) || !(ratio > 1))
    throw std::invalid_argument("gradedEdges: need hi > lo, hMax > 0, ratio > 1");
  double hMin = hMax;
  for (const Refinement& r : fine)
    hMin = std::min(hMin, r.cell);
  // Size field on a sampling finer than the finest cell: the requested size, then limited so that it can grow
  // by at most (ratio - 1) per unit length from any finer place (two sweeps of a distance transform).
  const double ds = hMin / 4;
  const std::size_t n = static_cast<std::size_t>(std::ceil((hi - lo) / ds)) + 1;
  std::vector<double> size(n, hMax);
  for (const Refinement& r : fine)
    for (std::size_t i = 0; i < n; ++i) {
      const double x = lo + static_cast<double>(i) * ds;
      if (x >= r.lo && x <= r.hi)
        size[i] = std::min(size[i], r.cell);
    }
  const double growth =
      0.6 * (ratio - 1) * ds; // margin: a cell spans h, not a point, so the realised ratio is higher
  for (std::size_t i = 1; i < n; ++i)
    size[i] = std::min(size[i], size[i - 1] + growth);
  for (std::size_t i = n - 1; i-- > 0;)
    size[i] = std::min(size[i], size[i + 1] + growth);

  auto sizeAt = [&](double x) {
    return size[std::min(n - 1, static_cast<std::size_t>(std::max(0.0, (x - lo) / ds)))];
  };
  std::vector<double> edges{lo};
  while (edges.back() < hi) {
    const double x = edges.back();
    double h = sizeAt(x);
    h = std::min(h, sizeAt(x + h)); // look ahead so a step never jumps over a refinement
    edges.push_back(x + h);
  }
  // Land exactly on hi: drop a sliver, then stretch the final cells.
  if (edges.size() >= 3 && hi - edges[edges.size() - 2] < 0.5 * (edges.back() - edges[edges.size() - 2]))
    edges.pop_back();
  edges.back() = hi;
  if (edges.size() >= 2 && edges.back() <= edges[edges.size() - 2]) {
    edges.pop_back();
    edges.back() = hi;
  }
  // Snap: move the nearest edge onto each requested position, tapering the shift over neighbouring cells.
  const std::vector<double> original = edges;
  std::vector<bool> fixed(edges.size(), false);
  std::vector<double> targets = snap;
  std::sort(targets.begin(), targets.end());
  for (double target : targets) {
    if (!(target > lo && target < hi) || edges.size() < 5)
      continue;
    std::size_t i = 1;
    for (std::size_t n = 1; n + 1 < edges.size(); ++n)
      if (std::abs(edges[n] - target) < std::abs(edges[i] - target))
        i = n;
    if (fixed[i] || std::abs(edges[i] - target) < 1e-15)
      continue;
    const double shift = target - edges[i];
    constexpr std::size_t kTaper = 3;
    std::vector<double> trial = edges;
    for (int dir : {-1, 1}) {
      std::size_t reach =
          0; // how many neighbouring edges on this side can move before a fixed edge or the end
      while (reach < kTaper) {
        const long next = static_cast<long>(i) + dir * static_cast<long>(reach + 1);
        if (next <= 0 || next >= static_cast<long>(edges.size()) - 1 || fixed[static_cast<std::size_t>(next)])
          break;
        ++reach;
      }
      for (std::size_t d = 1; d <= reach; ++d)
        trial[static_cast<std::size_t>(static_cast<long>(i) + dir * static_cast<long>(d))] +=
            shift * (1.0 - static_cast<double>(d) / static_cast<double>(reach + 1));
    }
    trial[i] = target;
    bool ok = true;
    for (std::size_t n = 1; n < trial.size() && ok; ++n)
      ok = trial[n] - trial[n - 1] >= 0.4 * (original[n] - original[n - 1]);
    if (!ok)
      continue; // a neighbouring snap or the end is too close; leave this one unsnapped
    edges = std::move(trial);
    fixed[i] = true;
  }
  return edges;
}

} // namespace board
