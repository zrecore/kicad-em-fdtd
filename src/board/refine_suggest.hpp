// Suggested refinement boxes derived from the board's geometry: wherever two nets' copper lies only a small
// gap apart close to a port (a coplanar feed's gap to its ground, a feed to a pour), the field in the gap
// needs several cells across it, and the mesh presets do not give them (docs/PLAN.md: 3 cells across the gap
// read a coplanar line 15 percent low in impedance, 5 cells 8 percent low). Each suggestion names its reason
// and the cell it would use, so the user can switch it on or off and change the cell. The gap is measured on
// the copper itself, not on the grid, so the suggestion does not depend on the current mesh. Pure logic.
#pragma once
#include <string>
#include <vector>

#include "board/snapshot.hpp"
#include "board/voxelize.hpp"

namespace board {

struct RefineSuggestion {
  std::string id;     // stable across runs for the same place: "gap|layer|netA|netB|x|y" (mm, rounded)
  std::string reason; // one sentence for the list
  std::string layer, netA, netB;
  double gapMm = 0;  // the narrowest gap found
  double cellMm = 0; // suggested cell: gap / cellsAcross, not below the floor
  RefineBox box{};   // board coordinates, metres; box.cell is cellMm in metres
};

// Gaps of at most `maxGapMm` between two different nets (both in the model: `sel` is respected) within
// `nearPortMm` of a port, one suggestion per net pair and layer, narrowest first, at most `limit`.
std::vector<RefineSuggestion> suggestRefinement(const Snapshot& s, const Selection& sel,
                                                double cellsAcross = 6.0, double minCellMm = 0.02,
                                                double maxGapMm = 1.0, double nearPortMm = 4.0,
                                                std::size_t limit = 4);

// The open ends of the copper a port feeds: where a conductor stops, the field piles up at the edge (a
// patch's radiating edges, an antenna's tips) and a coarse cell there reads the resonance low (a 60 mm patch:
// 3 percent at the default mesh, docs/trust.md). The copper is everything that touches the copper under a
// port on the port's layer, in the model, except shapes covering `groundShare` of the board's bounding box (a
// pour has no ends); within `reachMm` of the port. For each side (left, right, bottom, top) of that copper
// that ends inside the reach, one box of `depthMm` each side of the end, as wide as the copper that close to
// the end, with cells of `cellMm`. A side where the copper only continues past the reach gets none. Pure
// logic.
std::vector<RefineSuggestion> suggestCopperEnds(const Snapshot& s, const Selection& sel, double cellMm = 0.12,
                                                double depthMm = 3.0, double reachMm = 30.0,
                                                double groundShare = 0.6);

} // namespace board
