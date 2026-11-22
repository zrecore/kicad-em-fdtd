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
  std::string placement;                              // "edge", "corner" or empty (edge)
  bool fromProject = false;                           // read from the project's own rules file
};

std::vector<AntennaRule> parseAntennaRules(std::string_view json,
                                           bool fromProject = false); // throws SnapshotError
// The project's rules first (they win when a part number matches), then the shipped ones whose id the project
// did not redefine.
std::vector<AntennaRule> mergeRules(const std::vector<AntennaRule>& shipped,
                                    const std::vector<AntennaRule>& project);

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
  bool ok = false;     // false: `why` says what is missing
  bool corner = false; // the keepout was anchored at a corner (else centred on the part at an edge)
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

// Edge: a box centred on the part, starting at the nearest board edge. Corner: a box starting at the corner
// of the outline nearest the part, reaching `along` mm along the nearer edge towards the part and `depth` mm
// in.
enum class Placement { Edge, Corner };

// `along` and `depth` (mm) are the keepout size. `testBoardMm` (length, width) is the vendor's board or {0,
// 0}.
AntennaReport checkAntenna(const Snapshot& s, const Component& part, double alongMm, double depthMm,
                           const std::set<std::string>& groundNets, double testLengthMm = 0,
                           double testWidthMm = 0, Placement placement = Placement::Edge);

} // namespace board
