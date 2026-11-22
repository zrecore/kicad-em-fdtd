// Static checks of a chip antenna's surroundings against its published integration rules (docs/PLAN.md,
// "Reference-guided antenna tuning", Tier 1): copper and vias inside the keepout on every layer, the size of
// the ground against the vendor's test board, and the stackup under the part. No solve. The keepout is placed
// the way chip-antenna datasheets draw it: a box centred on the part, starting at the nearest board edge and
// reaching `depth` into the board. Pure logic.
#pragma once
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "board/snapshot.hpp"

namespace board {

struct AntennaRule {
  std::string id, source, status; // status: "verified" or "unverified"
  std::vector<std::string> parts; // part numbers (matched in the footprint's value or reference)
  double keepoutAlongMm = 0, keepoutDepthMm = 0;
  double testBoardLengthMm = 0, testBoardWidthMm = 0; // 0: not published
};

std::vector<AntennaRule> parseAntennaRules(std::string_view json); // throws SnapshotError

// The rule whose part number appears in the component's value or reference; null when none does.
const AntennaRule* ruleFor(const std::vector<AntennaRule>& rules, const Component& c);

struct KeepoutCopper {
  std::string layer;
  double areaMm2 = 0;
  std::set<std::string> nets;
};
struct GroundLayer {
  std::string layer;
  double areaMm2 = 0;
  double reachMm = 0; // from the board edge to the farthest ground copper, along the keepout's depth axis
  double widthMm = 0; // extent along the edge
};

struct AntennaReport {
  bool ok = false; // false: `why` says what is missing
  std::string why, ref;
  double alongMm = 0, depthMm = 0;
  double edgeDistanceMm = 0;          // part centre to the nearest board edge
  std::vector<KeepoutCopper> keepout; // copper not owned by the part, per layer, with area
  std::size_t viasInKeepout = 0;
  std::vector<GroundLayer> ground; // per copper layer, from the ground nets
  double totalThicknessMm = 0;
  double toNextPlaneMm = 0;          // dielectric between the part's layer and the next copper layer
  std::vector<std::string> findings; // sentences, most important first
};

// `along` and `depth` (mm) are the keepout size. `testBoardMm` (length, width) is the vendor's board or {0,
// 0}.
AntennaReport checkAntenna(const Snapshot& s, const Component& part, double alongMm, double depthMm,
                           const std::set<std::string>& groundNets, double testLengthMm = 0,
                           double testWidthMm = 0);

} // namespace board
