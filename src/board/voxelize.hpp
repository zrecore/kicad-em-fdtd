// Voxelizer: board snapshot + user's component selection -> FDTD grid, materials and PEC sheets.
// Copper is a zero-thickness PEC sheet on the dielectric interface (staircased to the grid); a copper layer's
// own thickness is not modelled and the stackup's dielectric thickness is used as given. Vias are PEC
// columns.
#pragma once
#include <set>
#include <string>
#include <vector>

#include "board/snapshot.hpp"
#include "fdtd/solver.hpp"

namespace board {

// Which footprints take part in the model. Board-level items (owner "") are always included.
struct Selection {
  bool all = true;
  std::set<std::string> owners; // used when all == false
  bool includes(const std::string& owner) const { return owner.empty() || all || owners.count(owner) > 0; }
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
