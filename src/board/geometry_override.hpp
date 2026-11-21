// Solver-side geometry overrides for parametric studies: the KiCAD board is never edited; a copy of the
// snapshot is modified before the model is built, and every override is printed in the run summary.
//   stretch: all copper and vias on one net are scaled along x or y about the net's low edge, so a patch
//   becomes longer or shorter by `deltaMm` (a feed line on the same net scales with it).
#pragma once
#include <string>
#include <vector>

#include "board/snapshot.hpp"

namespace board {

struct GeometryOverride {
  std::string net;
  char axis = 'x';              // 'x' or 'y'
  double deltaMm = 0;           // change of the net's extent along `axis`, millimetres
  std::string describe() const; // "net RF stretched along x by +0.5 mm"
};

// "NET.x=0.5" or "NET.y=-0.25" (the part after the last '.' before '=' is the axis; net names may contain
// dots). Throws SnapshotError on a malformed text.
GeometryOverride parseGeometryOverride(const std::string& text);

// Applies the overrides in order to `s`. Throws SnapshotError if a net has no copper, or the stretch would
// make the net's extent zero or negative. Returns the descriptions, for the run summary.
std::vector<std::string> applyGeometryOverrides(Snapshot& s, const std::vector<GeometryOverride>& overrides);

} // namespace board
