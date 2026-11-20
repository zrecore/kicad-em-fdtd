// 1D graded mesh: coarse cells of size hMax, fine cells inside the refinement intervals, and a smooth
// transition between them (neighbouring cell sizes differ by at most `ratio`).
#pragma once
#include <vector>

namespace board {

struct Refinement { // metres, along one axis
  double lo, hi, cell;
};

// Node positions from lo to hi (both exact). Cells inside a refinement never exceed its cell, others never
// exceed hMax, except that the last cell may reach 1.5 * hMax (a sliver is merged into its neighbour, because
// the smallest cell sets the time step).
// `snap` lists positions (for example copper edges) that must land exactly on a grid line: the nearest edge
// is moved there and the shift is tapered over a few neighbouring cells, so the cell count and the smoothness
// are kept.
std::vector<double> gradedEdges(double lo, double hi, double hMax, const std::vector<Refinement>& fine,
                                double ratio = 1.3, const std::vector<double>& snap = {});

} // namespace board
