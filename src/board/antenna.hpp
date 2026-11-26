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

// Which footprint is the antenna, when the user has not said. A part looks like an antenna when its reference
// starts with ANT or AE, or its value says antenna (the word, or a PIFA / IFA / MIFA token). A connector, a
// resistor or a chip whose value merely contains those letters inside a longer word does not.
bool looksLikeAntenna(const Component& c);
// The parts in the order the Antenna tab lists them: those that look like antennas first, then every other
// part in natural order (C2 before C10), so the antenna is never buried in the export's own order.
std::vector<const Component*> antennaPickOrder(const std::vector<Component>& parts);
// The part to preselect: the first one a stored rule recognises, else the only one that looks like an
// antenna, else null (the user picks). The user's own pick always wins over this.
const Component* defaultAntennaPart(const std::vector<Component>& parts,
                                    const std::vector<AntennaRule>& rules);

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

// The feed line of an antenna part, read from the board as drawn: the copper that leaves the part's signal
// pad, its width, the gap to ground copper beside it on the same layer, the ground plane below it, and from
// those the characteristic impedance by the closed forms in line_impedance.hpp (microstrip, or grounded
// coplanar waveguide when ground copper runs close beside the line). A first look, not a field solve.
// One stretch of the measured feed line with a steady width (and side gap), by distance from the part's pad.
struct FeedSegment {
  double fromMm = 0, toMm = 0, widthMm = 0, gapMm = 0; // gapMm: 0 when there is no ground beside the line
  double z0Ohm = 0;                                    // closed form for this width and gap (quick estimate)
};

struct FeedReport {
  bool ok = false; // false: `why` says what could not be read
  std::string why, net, layer, referenceLayer;
  bool coplanar = false; // ground copper beside the line on both sides
  bool oneSided = false; // ground copper beside it on one side only (gapMm is that side's; coplanar is false)
  double widthMm = 0, gapMm = 0; // gapMm: to the side ground, 0 when there is none close by
  double heightMm = 0, epsR = 0; // dielectric between the line and its reference plane, and its permittivity
  double copperMm = 0;           // finished copper thickness of the line's layer, 0 when unknown
  double z0Ohm = 0, epsEff = 0;
  double atXMm = 0, atYMm = 0; // where the line was measured (board coordinates)
  // What the single number hides (the row's Z0 is that of the uniform line, as measured between 1 and 6 mm
  // from the pad): the stretches of steady width, the other parts whose pads touch the feed copper there, and
  // where the copper turns or branches (a corner, a T, the start of a stub).
  std::vector<FeedSegment> segments;
  std::vector<std::string> otherParts;
  std::vector<double> turnsAtMm;     // distance from the pad
  std::vector<std::string> findings; // sentences, most important first
};

// `portOhm` is the reference impedance the run will use.
FeedReport checkFeed(const Snapshot& s, const Component& part, const std::set<std::string>& groundNets,
                     double portOhm = 50.0);

} // namespace board
