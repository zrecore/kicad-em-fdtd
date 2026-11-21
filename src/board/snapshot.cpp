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
    for (const Json& l : require(j, "stackup")) {
      Layer layer;
      layer.name = require(l, "name").get<std::string>();
      layer.copper = require(l, "kind").get<std::string>() == "copper";
      if (!layer.copper) {
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
                        text(v, "owner")});
    for (const Json& c : require(j, "components")) {
      Component comp{require(c, "ref").get<std::string>(), text(c, "value"), text(c, "layer"),
                     c.value("x", 0.0), c.value("y", 0.0)};
      if (c.contains("body")) {
        comp.body = ring(require(c.at("body"), "polygon"), "component body");
        comp.bodyHeight = c.at("body").value("height", 0.0); // 0: the run's default height applies
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
