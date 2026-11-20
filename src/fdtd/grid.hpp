// Structured, possibly graded grid: cell edge positions per axis (SI metres).
//
// Storage convention (Yee): the grid has nx*ny*nz cells and (nx+1)*(ny+1)*(nz+1) nodes. Every field array
// has node dimensions and x varies fastest, so the walls at i = 0 and i = nx are real stored samples
// (tangential E there is zero for a PEC wall). Positions:
//   Ex (i+1/2, j, k)   Ey (i, j+1/2, k)   Ez (i, j, k+1/2)
//   Hx (i, j+1/2, k+1/2)   Hy (i+1/2, j, k+1/2)   Hz (i+1/2, j+1/2, k)
// Entries of an array that do not correspond to a sample (for example Ex at i = nx) stay zero.
// On a graded grid each difference is divided by the distance between the samples it straddles: the cell
// size for differences of E in H updates, the dual (centre to centre) size for H in E updates.
#pragma once
#include <array>
#include <cstddef>
#include <vector>

namespace fdtd {

enum Axis { X = 0, Y = 1, Z = 2 };

class Grid {
public:
  // edges[a] holds n_a + 1 strictly increasing node positions along axis a.
  explicit Grid(std::array<std::vector<double>, 3> edges);
  static Grid uniform(std::array<std::size_t, 3> cells, std::array<double, 3> size); // domain starts at 0

  std::size_t cells(Axis a) const { return edges_[a].size() - 1; }
  std::size_t nodes(Axis a) const { return edges_[a].size(); }
  std::size_t cellCount() const { return cells(X) * cells(Y) * cells(Z); }
  std::size_t nodeCount() const { return nodes(X) * nodes(Y) * nodes(Z); }
  double edge(Axis a, std::size_t i) const { return edges_[a][i]; } // node position
  double cellSize(Axis a, std::size_t i) const { return edges_[a][i + 1] - edges_[a][i]; }
  // Distance between the centres of the cells either side of node i (half a cell at the walls).
  double dualSize(Axis a, std::size_t i) const;
  double minCellSize(Axis a) const;
  double length(Axis a) const { return edges_[a].back() - edges_[a].front(); }
  std::size_t nodeIndex(std::size_t i, std::size_t j, std::size_t k) const {
    return i + nodes(X) * (j + nodes(Y) * k);
  }
  std::size_t cellIndex(std::size_t i, std::size_t j, std::size_t k) const {
    return i + cells(X) * (j + cells(Y) * k);
  }

private:
  std::array<std::vector<double>, 3> edges_;
};

// Largest stable time step of the explicit scheme (3D Courant limit with the smallest cell on each axis).
double maxStableDt(const Grid& g);

} // namespace fdtd
