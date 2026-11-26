#include "board/snapshot.hpp"

#include <nlohmann/json.hpp>

namespace board {
namespace {

using Json = nlohmann::json;

const Json& require(const Json& j, const char* key) {
  if (!j.is_object() || !j.contains(key))
    throw SnapshotError(std::string("missing field '") + key + "'");
  return j.at(key);
}

Ring ring(const Json& j, const char* what) {
  Ring r;
  for (const Json& p : j) {
    if (!p.is_array() || p.size() != 2)
      throw SnapshotError(std::string(what) + ": each point must be [x, y]");
    r.push_back({p[0].get<double>(), p[1].get<double>()});
  }
  if (r.size() < 3)
    throw SnapshotError(std::string(what) + ": a polygon needs at least 3 points");
  return r;
}

std::string text(const Json& j, const char* key) {
  return j.contains(key) ? j.at(key).get<std::string>() : std::string();
}

} // namespace

Snapshot parseSnapshot(std::string_view textIn) {
  Json j;
  try {
    j = Json::parse(textIn);
  } catch (const Json::exception& e) {
    throw SnapshotError(std::string("not valid JSON: ") + e.what());
  }
  try {
    if (text(j, "format") != "kicad-em-fdtd.snapshot")
      throw SnapshotError("not a kicad-em-fdtd snapshot (field 'format')");
    if (require(j, "version").get<int>() != 1)
      throw SnapshotError("unsupported snapshot version " + require(j, "version").dump() +
                          " (this build reads 1)");
    Snapshot s;
    const Json& board = require(j, "board");
    s.boardName = text(board, "name");
    s.kicadVersion = text(board, "kicad_version");
    s.outline = ring(require(j, "outline"), "outline");
    if (j.contains("cutouts"))
      for (const Json& c : j.at("cutouts"))
        s.cutouts.push_back(ring(c, "cutout"));
    if (j.contains("board_proper")) {
      const Json& bp = j.at("board_proper");
      s.hasBoardProper = true;
      s.properOutline = ring(require(bp, "outline"), "board_proper outline");
      if (bp.contains("cutouts"))
        for (const Json& c : bp.at("cutouts"))
          s.properCutouts.push_back(ring(c, "board_proper cutout"));
    }
    for (const Json& l : require(j, "stackup")) {
      Layer layer;
      layer.name = require(l, "name").get<std::string>();
      layer.copper = require(l, "kind").get<std::string>() == "copper";
      if (layer.copper) {
        layer.thickness = l.value("thickness", 0.0); // 0: unknown
        if (!(layer.thickness >= 0))
          throw SnapshotError("stackup layer '" + layer.name + "': copper thickness must not be negative");
      } else {
        layer.thickness = require(l, "thickness").get<double>();
        layer.epsilonR = require(l, "epsilon_r").get<double>();
        layer.lossTangent = l.value("loss_tangent", 0.0);
        if (!(layer.thickness > 0) || !(layer.epsilonR >= 1))
          throw SnapshotError("stackup layer '" + layer.name + "': need thickness > 0 and epsilon_r >= 1");
      }
      s.stackup.push_back(layer);
    }
    for (const Json& c : require(j, "copper")) {
      CopperShape shape;
      shape.layer = require(c, "layer").get<std::string>();
      shape.owner = text(c, "owner");
      shape.net = text(c, "net");
      shape.outer = ring(require(c, "polygon"), "copper polygon");
      if (c.contains("holes"))
        for (const Json& h : c.at("holes"))
          shape.holes.push_back(ring(h, "copper hole"));
      s.copper.push_back(std::move(shape));
    }
    for (const Json& v : require(j, "vias"))
      s.vias.push_back({require(v, "x").get<double>(), require(v, "y").get<double>(),
                        require(v, "diameter").get<double>(), v.value("drill", 0.0), text(v, "net"),
                        text(v, "owner"), text(v, "from"), text(v, "to")});
    for (const Json& c : require(j, "components")) {
      Component comp{require(c, "ref").get<std::string>(), text(c, "value"), text(c, "layer"),
                     c.value("x", 0.0), c.value("y", 0.0)};
      if (c.contains("body")) {
        comp.body = ring(require(c.at("body"), "polygon"), "component body");
        comp.bodyHeight = c.at("body").value("height", 0.0); // 0: the run's default height applies
        comp.bodyOffset = c.at("body").value("offset", 0.0);
        if (comp.bodyOffset < 0)
          throw SnapshotError("component '" + comp.ref + "': body offset must not be negative");
        if (comp.bodyHeight < 0)
          throw SnapshotError("component '" + comp.ref + "': body height must not be negative");
      }
      if (c.contains("pads"))
        for (const Json& p : c.at("pads"))
          comp.pads.push_back({text(p, "number"), text(p, "net"), require(p, "x").get<double>(),
                               require(p, "y").get<double>()});
      if (c.contains("attributes")) {
        comp.dnp = c.at("attributes").value("dnp", false);
        comp.mounting = text(c.at("attributes"), "mounting");
      }
      s.components.push_back(std::move(comp));
    }
    if (j.contains("ports"))
      for (const Json& p : j.at("ports")) {
        PortDef port;
        port.name = require(p, "name").get<std::string>();
        port.layer = require(p, "layer").get<std::string>();
        port.layerB = text(p, "layer_b");
        port.a = {require(p, "a")[0].get<double>(), require(p, "a")[1].get<double>()};
        port.b = port.layerB.empty()
                     ? Point{require(p, "b")[0].get<double>(), require(p, "b")[1].get<double>()}
                     : port.a;
        port.width = p.value("width", 0.0);
        port.impedance = p.value("impedance", 50.0);
        if (!(port.impedance > 0))
          throw SnapshotError("port '" + port.name + "': impedance must be positive");
        s.ports.push_back(std::move(port));
      }
    return s;
  } catch (const Json::exception& e) {
    throw SnapshotError(std::string("wrong field type: ") + e.what());
  }
}

} // namespace board

namespace board {
namespace {
bool insideRing(const Ring& r, double x, double y) {
  bool hit = false;
  for (std::size_t i = 0, n = r.size(); i < n; ++i) {
    const Point &a = r[i], &b = r[(i + 1) % n];
    if ((a.y > y) != (b.y > y) && x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x)
      hit = !hit;
  }
  return hit;
}
} // namespace

bool restrictToBoardProper(Snapshot& s) {
  if (!s.hasBoardProper)
    return false;
  const Ring& ring = s.properOutline;
  auto onBoard = [&](const Ring& poly) {
    for (const Point& p : poly)
      if (insideRing(ring, p.x, p.y))
        return true;
    return !poly.empty() && !ring.empty() && insideRing(poly, ring[0].x, ring[0].y);
  };
  std::erase_if(s.copper, [&](const CopperShape& c) { return !onBoard(c.outer); });
  std::erase_if(s.vias, [&](const Via& v) { return !insideRing(ring, v.x, v.y); });
  std::erase_if(s.components, [&](const Component& c) { return !insideRing(ring, c.x, c.y); });
  s.outline = s.properOutline;
  s.cutouts = s.properCutouts;
  s.hasBoardProper = false;
  return true;
}
} // namespace board
