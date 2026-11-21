// Surface current on zero-thickness PEC sheets, read from the jump of tangential H across the sheet:
// J_s = n x (H_above - H_below). A sheet cell is a forced tangential E edge (a PecEdge on x or y); the sheet
// lies in the z plane of that edge, and the two H samples that straddle it sit half a cell above and below
// at the same lateral position, so each edge has exactly one pair (no staircase ambiguity: the current is
// attributed to the edges the solver actually forces). Vertical via barrels (Ez edges) are not included.
// Cross width (for the current through a cut): an edge covers its dual cell across the current, but only the
// half on the side of a forced neighbour edge, so a sheet's border edges do not count the empty space beyond
// them and the widths along a cut add up to the sheet's width.
//   x edge (i+1/2, j, k):  Jx = -(Hy(i,j,k)   - Hy(i,j,k-1))     [Hy (i+1/2, j, k+1/2)]
//   y edge (i, j+1/2, k):  Jy =  (Hx(i,j,k)   - Hx(i,j,k-1))     [Hx (i, j+1/2, k+1/2)]
#pragma once
#include <complex>
#include <set>
#include <vector>

#include "fdtd/phasor_bank.hpp"
#include "fdtd/solver.hpp"

namespace fdtd {

struct SurfaceCurrentCell {
  Axis direction;      // X: current along x (from a Hy jump), Y: along y
  std::size_t i, j, k; // the forced E edge
  double crossWidth; // width of the cell across the current: the current through a cut is sum(J * crossWidth)
  double lengthAlong; // cell size along the current
};

struct SurfaceCurrentList {
  std::vector<SurfaceCurrentCell> cells;
  std::vector<PhasorChannel> channels; // 2 per cell: [2c] = H above the sheet, [2c+1] = H below
};

inline SurfaceCurrentList surfaceCurrentList(const Grid& g, const std::vector<PecEdge>& edges) {
  SurfaceCurrentList out;
  std::set<std::pair<int, std::size_t>> forced;
  for (const PecEdge& e : edges)
    forced.insert({e.axis, e.node});
  auto has = [&](Axis a, std::size_t i, std::size_t j, std::size_t k) {
    return forced.count({a, g.nodeIndex(i, j, k)}) > 0;
  };
  for (const PecEdge& e : edges) {
    if (e.axis == Z)
      continue;
    const std::size_t i = e.node % g.nodes(X), j = (e.node / g.nodes(X)) % g.nodes(Y),
                      k = e.node / (g.nodes(X) * g.nodes(Y));
    if (k == 0 || k >= g.cells(Z) || i >= g.cells(X) || j >= g.cells(Y) + (e.axis == X ? 1 : 0))
      continue; // a wall, or an edge with no H pair around it
    SurfaceCurrentCell c{e.axis, i, j, k, 0, 0};
    const Axis h = e.axis == X ? Y : X; // the tangential H that carries the jump
    if (e.axis == X) {
      const bool lo = j > 0 && has(X, i, j - 1, k), hi = j < g.cells(Y) && has(X, i, j + 1, k);
      c.crossWidth = (lo ? 0.5 * g.cellSize(Y, j - 1) : 0.0) + (hi ? 0.5 * g.cellSize(Y, j) : 0.0);
      c.lengthAlong = g.cellSize(X, i);
    } else {
      if (j >= g.cells(Y))
        continue;
      const bool lo = i > 0 && has(Y, i - 1, j, k), hi = i < g.cells(X) && has(Y, i + 1, j, k);
      c.crossWidth = (lo ? 0.5 * g.cellSize(X, i - 1) : 0.0) + (hi ? 0.5 * g.cellSize(X, i) : 0.0);
      c.lengthAlong = g.cellSize(Y, j);
    }
    if (c.crossWidth == 0) // an isolated edge: one dual cell
      c.crossWidth = e.axis == X ? g.dualSize(Y, j) : g.dualSize(X, i);
    out.cells.push_back(c);
    out.channels.push_back({Kind::H, h, static_cast<std::uint32_t>(g.nodeIndex(i, j, k))});
    out.channels.push_back({Kind::H, h, static_cast<std::uint32_t>(g.nodeIndex(i, j, k - 1))});
  }
  return out;
}

// J_s of cell c from the two accumulated H phasors (above, below), in A/m.
inline std::complex<double> surfaceCurrent(const SurfaceCurrentCell& c, std::complex<double> hAbove,
                                           std::complex<double> hBelow) {
  const std::complex<double> jump = hAbove - hBelow;
  return c.direction == X ? -jump : jump;
}

} // namespace fdtd
