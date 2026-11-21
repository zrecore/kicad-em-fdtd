#include "board/geometry_override.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace board {

std::string GeometryOverride::describe() const {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%+.4g mm", deltaMm);
  return "net " + net + " stretched along " + axis + " by " + buf;
}

GeometryOverride parseGeometryOverride(const std::string& text) {
  const auto eq = text.rfind('=');
  const auto dot = eq == std::string::npos ? std::string::npos : text.rfind('.', eq);
  if (eq == std::string::npos || dot == std::string::npos || dot == 0 || eq - dot != 2)
    throw SnapshotError("a geometry override is NET.x=MM or NET.y=MM, got '" + text + "'");
  GeometryOverride o;
  o.net = text.substr(0, dot);
  o.axis = text[dot + 1];
  if (o.axis != 'x' && o.axis != 'y')
    throw SnapshotError("a geometry override's axis is x or y, got '" + std::string(1, o.axis) + "'");
  char* end = nullptr;
  const std::string value = text.substr(eq + 1);
  o.deltaMm = std::strtod(value.c_str(), &end);
  if (value.empty() || *end)
    throw SnapshotError("a geometry override's value is a number of millimetres, got '" + value + "'");
  return o;
}

std::vector<std::string> applyGeometryOverrides(Snapshot& s, const std::vector<GeometryOverride>& overrides) {
  std::vector<std::string> notes;
  for (const GeometryOverride& o : overrides) {
    const bool x = o.axis == 'x';
    double lo = 1e300, hi = -1e300;
    auto extend = [&](const Ring& r) {
      for (const Point& p : r) {
        lo = std::min(lo, x ? p.x : p.y);
        hi = std::max(hi, x ? p.x : p.y);
      }
    };
    for (const CopperShape& c : s.copper)
      if (c.net == o.net)
        extend(c.outer);
    for (const Via& v : s.vias)
      if (v.net == o.net) {
        lo = std::min(lo, x ? v.x : v.y);
        hi = std::max(hi, x ? v.x : v.y);
      }
    if (!(hi > lo))
      throw SnapshotError("geometry override: net '" + o.net + "' has no copper with an extent along " +
                          o.axis);
    const double extent = hi - lo, delta = o.deltaMm * 1e-3;
    if (!(extent + delta > 0))
      throw SnapshotError("geometry override: " + o.describe() + " would leave the net with no extent");
    const double k = (extent + delta) / extent;
    auto map = [&](double c) { return lo + (c - lo) * k; };
    auto scale = [&](Ring& r) {
      for (Point& p : r)
        (x ? p.x : p.y) = map(x ? p.x : p.y);
    };
    for (CopperShape& c : s.copper)
      if (c.net == o.net) {
        scale(c.outer);
        for (Ring& h : c.holes)
          scale(h);
      }
    for (Via& v : s.vias)
      if (v.net == o.net)
        (x ? v.x : v.y) = map(x ? v.x : v.y);
    notes.push_back("geometry override: " + o.describe());
  }
  return notes;
}

} // namespace board
