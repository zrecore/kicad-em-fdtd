// Board snapshot: the one format the KiCAD plugin writes and the native app reads (docs/snapshot.md).
// All lengths are metres: the plugin converts from KiCAD's nanometres at the board boundary.
#pragma once
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace board {

struct Point {
  double x, y;
};
using Ring = std::vector<Point>;

struct Layer { // stackup entry, top to bottom
  std::string name;
  bool copper = false; // copper layers are zero-thickness PEC sheets on the dielectric interfaces
  double thickness = 0, epsilonR = 1, lossTangent = 0; // dielectric only
};

struct CopperShape {             // one filled polygon of copper (a pad, a track outline, a zone fill)
  std::string layer, owner, net; // owner: footprint reference, or "" for board-level copper
  Ring outer;
  std::vector<Ring> holes;
};

struct Via {
  double x, y, diameter, drill;
  std::string net, owner;
};

struct ComponentPad { // a footprint's pad: what a two-terminal part's two ends are
  std::string number, net;
  double x, y; // board position, metres
};

struct Component {
  std::string ref, value, layer;
  double x, y;
  Ring body; // footprint outline on the board plane (courtyard or bounding box); empty: unknown
  std::vector<ComponentPad> pads; // empty: the exporter gave none
  bool dnp = false;               // do not populate: not fitted, so open between its pads
  std::string mounting;           // "smd", "through_hole" or empty
  double bodyHeight = 0; // metres above (top side) or below (bottom side) the board surface; 0: not given,
                         // the run's default height applies
};

// A lumped port across a gap in one copper layer: from point a (positive) to point b (negative), along x or
// y, over `width` (a strip of parallel edges). Reference impedance in ohm.
struct PortDef {
  std::string name, layer,
      layerB; // layerB non-empty: a vertical port from `layer` (positive) down/up to layerB
  Point a, b;
  double width = 0, impedance = 50;
};

struct Snapshot {
  std::string boardName, kicadVersion;
  Ring outline;
  std::vector<Layer> stackup;
  std::vector<CopperShape> copper;
  std::vector<Via> vias;
  std::vector<Component> components;
  std::vector<PortDef> ports;
};

struct SnapshotError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

Snapshot parseSnapshot(std::string_view json); // throws SnapshotError naming the offending field

} // namespace board
