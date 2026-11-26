#include "board/feed_solve.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <string>

namespace board {
namespace l2 = fdtd::line2d;
namespace {
constexpr double mm = 1e-3;
}

l2::GridOptions feedGrid() {
  l2::GridOptions g; // fine cells at the conductor edges, growing 30 % per cell to 8 times the cell
  g.growth = 1.3;
  g.maxCellFactor = 8;
  return g;
}

l2::CrossSection feedSection(const FeedReport& f) {
  const double t = f.copperMm * mm;
  // Walls 8 line-widths away: at 4 they bias the effective permittivity about 2 percent low (docs/methods/
  // microstrip-closed-forms.md), as large as the copper effect this estimate exists to show.
  constexpr double kMargin = 8;
  if (f.coplanar || f.oneSided) // one-sided: ground on the left only (a mirror image reads the same)
    return l2::groundedCoplanarSection(f.widthMm * mm, f.gapMm * mm, f.heightMm * mm, f.epsR, t, true,
                                       f.coplanar, kMargin);
  return l2::microstripSection(f.widthMm * mm, f.heightMm * mm, f.epsR, t, kMargin);
}

double feedCellMm(const FeedReport& f) {
  double feature = std::min(f.widthMm, f.heightMm);
  if (f.coplanar || f.oneSided)
    feature = std::min(feature, f.gapMm);
  double cell = feature / 4;
  if (f.copperMm > 0)
    cell = std::min(cell, f.copperMm); // at least one cell across the copper: grid lines sit on its faces
  return cell;
}

FeedSolve solveFeed(const FeedReport& f, const l2::RasterSolver& solver, double cellMm) {
  FeedSolve out;
  if (!f.ok || !(f.widthMm > 0) || !(f.heightMm > 0) || !(f.epsR >= 1) ||
      ((f.coplanar || f.oneSided) && !(f.gapMm > 0))) {
    out.why = f.ok ? "the feed line's width, gap or dielectric is missing" : f.why;
    return out;
  }
  out.cellMm = cellMm > 0 ? cellMm : feedCellMm(f);
  const FeedReport& line = f;
  out.copperMm = line.copperMm;
  try {
    l2::SolveOptions options;
    options.tolerance = 1e-8;
    const l2::LineResult r = l2::lineImpedanceExtrapolated(feedSection(line), out.cellMm * mm,
                                                           solver ? solver : l2::cpuSolver(options));
    out.z0Ohm = r.z0Ohm;
    out.epsEff = r.epsEff;
    out.converged = r.converged;
  } catch (const std::exception& e) {
    out.why = e.what();
    return out;
  }
  const std::string copper =
      f.copperMm > 0 ? std::to_string(static_cast<int>(std::lround(f.copperMm * 1000))) + " um"
                     : "zero-thickness (the board gives no copper thickness: set it in Settings, PCB)";
  char buf[512];
  std::snprintf(buf, sizeof buf,
                "Field solve of the same line (%s, %s copper, %.3f mm cells, extrapolated): %.1f ohm, "
                "effective permittivity %.2f%s. %s",
                f.coplanar   ? "ground beside it at the same gap on both sides"
                : f.oneSided ? "ground beside it on one side"
                             : "microstrip",
                copper.c_str(), out.cellMm, out.z0Ohm, out.epsEff,
                out.converged ? "" : " (did not fully converge)",
                f.coplanar ? "The report has one gap, so ground on one side only would read higher." : "");
  out.findings.push_back(buf);
  out.ok = true;
  return out;
}

} // namespace board
