// The accurate estimate of a feed line's impedance: the line the feed check measured (antenna.hpp,
// FeedReport) solved as a 2D cross-section (fdtd/line2d.hpp) with the copper's real thickness, instead of the
// closed form of line_impedance.hpp. Pure logic; the backend (CPU, or the GPU one in fdtd_vk) is passed in.
//
// The section is the measured width over the measured dielectric (one permittivity: the report's series-
// capacitance average) above a ground plane, with ground copper at the measured gap on BOTH sides when the
// line is coplanar (the report has a single gap; a line with ground on one side only is read as symmetric,
// which makes the estimate low). The walls are four line-widths away.
#pragma once
#include <string>

#include "board/antenna.hpp"
#include "fdtd/line2d.hpp"

namespace board {

struct FeedSolve {
  bool ok = false; // false: `why` says what could not be solved
  std::string why;
  double z0Ohm = 0, epsEff = 0;
  double cellMm = 0;   // the coarse cell; the extrapolation also solved at half of it
  double copperMm = 0; // the thickness used (0: a sheet)
  bool converged = false;
  std::vector<std::string> findings; // sentences, most important first
};

fdtd::line2d::CrossSection feedSection(const FeedReport& feed);

// The cell at the conductor edges: 4 cells across the narrowest of the line width, the gap and the
// dielectric, and at least one across the copper. The grid lines sit exactly on every conductor edge and the
// cells grow away from them (feedGrid), so the line's dimensions are exact whatever the cell.
double feedCellMm(const FeedReport& feed);
fdtd::line2d::GridOptions feedGrid();

// `cellMm` 0 picks feedCellMm. Throws nothing: a section that cannot be solved comes back with ok false.
FeedSolve solveFeed(const FeedReport& feed, const fdtd::line2d::RasterSolver& solver = {}, double cellMm = 0);

} // namespace board
