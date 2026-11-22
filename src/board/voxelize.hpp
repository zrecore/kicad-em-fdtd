// Voxelizer: board snapshot + user's component selection -> FDTD grid, materials and PEC sheets.
// Copper is a zero-thickness PEC sheet on the dielectric interface (staircased to the grid); a copper layer's
// own thickness is not modelled and the stackup's dielectric thickness is used as given. Vias are PEC
// columns.
#pragma once
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "board/parts.hpp"
#include "board/snapshot.hpp"
#include "fdtd/solver.hpp"

namespace board {

// Which footprints take part in the model. Board-level items (owner "") are always included.
// How a component's body (its footprint outline extruded by its height) enters the model. Ignore leaves it
// out (the default: only its copper is modelled); Dielectric fills it with a lossless dielectric of the given
// permittivity; Metal makes it a perfect conductor (a shield can, a connector shell).
enum class BodyKind { Ignore, Dielectric, Metal };
struct BodyTreatment {
  BodyKind kind = BodyKind::Ignore;
  double epsilonR = 4.0;    // Dielectric only; a labelled default, the user's to set
  double lossTangent = 0.0; // Dielectric only: 0 is lossless
  std::string label; // how well the model matches a datasheet, printed in the run notes; empty: assumed
};

struct Selection {
  bool all = true;
  std::set<std::string> owners; // used when all == false
  BodyTreatment defaultBody;    // for every selected component without its own entry
  double defaultBodyHeight = 0; // metres, for a body whose snapshot gives no height
  std::map<std::string, BodyTreatment> bodies;
  std::map<std::string, PartTreatment> parts; // per two-terminal part; absent means Auto
  bool netFilter = false;                     // net-driven selection (board/netwalk.hpp): keep only `nets`
  std::set<std::string> nets;                 // the nets kept: the walk's plus ground
  std::set<std::string> pickedNets;           // what the user picked (the walk starts here)
  std::vector<std::string> netNotes;          // why each part and net is in, for the run summary
  bool netIncluded(const std::string& net) const { return !netFilter || nets.count(net) > 0; }
  bool includes(const std::string& owner) const { return owner.empty() || all || owners.count(owner) > 0; }
  BodyTreatment bodyOf(const std::string& ref) const {
    const auto it = bodies.find(ref);
    return it == bodies.end() ? defaultBody : it->second;
  }
};

struct RefineBox { // board coordinates, metres: cells of size `cell` inside the box, graded outward
  double x0, y0, x1, y1, cell;
};

struct GridPlan {                     // metres
  double lateralCell;                 // x and y cell size (the coarse size when refinement boxes are given)
  double maxDielectricCell;           // cells per dielectric layer = ceil(thickness / this)
  double airMargin;                   // free space around the board on every side
  double airCell;                     // target cell size in that margin (z direction)
  std::vector<RefineBox> refine = {}; // empty: uniform lateral cells; else graded with ratio 1.3
  bool snapToGeometry = false;        // put grid lines on copper, outline and port edges (accuracy)
  double airNearCell = 0;             // z cell size in the air next to the board (0: airCell throughout)
  double airNearHeight = 0;           // that fine layer's thickness, then cells grow by 1.3 up to airCell
};

// A refinement box around every port, `pad` larger on each side, with cells of size `cell`.
std::vector<RefineBox> refineAroundPorts(const Snapshot& s, double cell, double pad);

// One port, expanded into primitive lumped edges: `series` cells in a row, `parallel` such strips side by
// side. Strip resistance is R * parallel so the strips together present R; within a strip each edge takes the
// share dl/L of the resistance and of the source voltage. V = sum over one strip of sign * E * dl; I = sum
// over strips.
struct PortEdge {
  fdtd::LumpedPort edge; // voltage waveform left empty: the runner fills it with sign * share * Vs(t)
  std::size_t strip;
  double share; // dl / L
  double sign;  // +1 if the positive terminal is at the lower coordinate
};
struct PortPlan {
  std::string name;
  double impedance;
  std::size_t series = 0, parallel = 0;
  std::vector<PortEdge> edges;
  bool lumped = false; // a resistor or capacitor part across a gap: a passive load, never the driven port
};

struct Model {
  fdtd::Grid grid;
  std::vector<fdtd::Material> materials; // id 0 is air
  std::vector<std::uint8_t> cellMaterial;
  std::vector<fdtd::PecEdge> pecEdges;
  std::vector<PortPlan> ports;
  std::vector<std::string> notes;           // what was left out or simplified, for the run summary
  std::vector<std::size_t> copperPlaneNode; // z node index of each stackup entry (copper only, else 0)
  double originX = 0, originY = 0;          // board coordinates of grid node (0, 0)
};

// centreFrequencyHz sets the conductivity that represents dielectric loss tangent.
Model voxelize(const Snapshot& s, const Selection& selection, const GridPlan& plan, double centreFrequencyHz);

} // namespace board
