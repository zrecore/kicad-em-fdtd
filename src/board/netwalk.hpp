// Net-driven selection: the user picks nets (not single traces) and the parts that belong to them follow.
// Every part with a pad on a picked net is included; a part that can be modelled as two-terminal
// (resistor, 0 ohm link, capacitor, inductor, or a bead with user values) carries the walk across to the net
// on its other side, so a feed line reaches the antenna through its matching network. The walk stops at parts
// with three or more pads (ICs, connectors, modules) and at parts that are open or not modelled. Ground and
// plane nets never start or continue a walk (nearly every part touches ground) and are always kept as the
// reference. Pure logic, no solver.
#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

#include "board/snapshot.hpp"
#include "board/voxelize.hpp"

namespace board {

struct NetWalk {
  std::set<std::string> picked; // what the user picked
  std::set<std::string> nets;   // the picked nets and the nets reached through two-terminal parts
  std::set<std::string> parts;  // references included, each with a reason in `reasons`
  std::map<std::string, std::string> reasons;
  std::vector<std::string> notes; // what stopped the walk and why, for the run summary
};

// Nets that are ground or a plane by name (GND, AGND, VSS, ...) or because one net's copper covers more than
// `planeFraction` of the board's bounding rectangle.
std::set<std::string> guessGroundNets(const Snapshot& s, double planeFraction = 0.25);

NetWalk walkNets(const Snapshot& s, const std::set<std::string>& picked, const std::set<std::string>& ground,
                 const std::map<std::string, PartTreatment>& treatments = {});

// Make `sel` an explicit selection from the walk: only the walk's parts, only copper on its nets plus ground.
void applyNetSelection(Selection& sel, const NetWalk& walk, const std::set<std::string>& ground);

// Back to "model everything": no net filter, every part (the per-part treatments and bodies stay).
void clearNetSelection(Selection& sel);

} // namespace board
