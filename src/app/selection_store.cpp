#include "app/selection_store.hpp"

#include <fstream>
#include <nlohmann/json.hpp>

#include "app/settings.hpp"

namespace app {
namespace {
using Json = nlohmann::ordered_json;
const char* stateName(board::PartTreatment::State s) {
  switch (s) {
  case board::PartTreatment::State::Open:
    return "open";
  case board::PartTreatment::State::Short:
    return "short";
  case board::PartTreatment::State::Values:
    return "values";
  default:
    return "auto";
  }
}
board::PartTreatment::State stateFrom(const std::string& s) {
  return s == "open"     ? board::PartTreatment::State::Open
         : s == "short"  ? board::PartTreatment::State::Short
         : s == "values" ? board::PartTreatment::State::Values
                         : board::PartTreatment::State::Auto;
}
const char* bodyName(board::BodyKind k) {
  return k == board::BodyKind::Dielectric ? "dielectric" : k == board::BodyKind::Metal ? "metal" : "ignore";
}
board::BodyKind bodyFrom(const std::string& s) {
  return s == "dielectric" ? board::BodyKind::Dielectric
         : s == "metal"    ? board::BodyKind::Metal
                           : board::BodyKind::Ignore;
}
} // namespace

void saveSelection(const std::filesystem::path& file, const StoredSelection& s) {
  Json j;
  j["format"] = "kicad-em-fdtd.selection";
  j["version"] = 1;
  j["antenna_ref"] = s.antennaRef;
  j["picked_nets"] = Json::array();
  for (const auto& n : s.pickedNets)
    j["picked_nets"].push_back(n);
  j["extra_ground_nets"] = Json::array();
  for (const auto& n : s.extraGroundNets)
    j["extra_ground_nets"].push_back(n);
  j["parts"] = Json::object();
  for (const auto& [ref, t] : s.parts) {
    Json p;
    p["state"] = stateName(t.state);
    p["fit_dnp"] = t.fitDnp;
    if (t.r)
      p["r_ohm"] = *t.r;
    if (t.l)
      p["l_henry"] = *t.l;
    if (t.c)
      p["c_farad"] = *t.c;
    j["parts"][ref] = p;
  }
  j["bodies"] = Json::object();
  for (const auto& [ref, b] : s.bodies)
    j["bodies"][ref] = {{"kind", bodyName(b.kind)},
                        {"epsilon_r", b.epsilonR},
                        {"loss_tangent", b.lossTangent},
                        {"label", b.label}};
  j["default_body"] = {{"kind", bodyName(s.defaultBody.kind)}, {"epsilon_r", s.defaultBody.epsilonR}};
  j["refinement"] = Json::object();
  for (const auto& [id, c] : s.refinement)
    j["refinement"][id] = {{"enabled", c.enabled}, {"cell_mm", c.cellMm}};
  std::filesystem::create_directories(file.parent_path());
  const auto tmp = std::filesystem::path(file).concat(".tmp"); // write whole or not at all
  {
    std::ofstream out(tmp);
    if (!out)
      throw SettingsError("cannot write " + tmp.string());
    out << j.dump(2) << "\n";
  }
  std::filesystem::rename(tmp, file);
}

StoredSelection loadSelection(const std::filesystem::path& file) {
  StoredSelection s;
  if (!std::filesystem::exists(file))
    return s;
  std::ifstream in(file);
  Json j;
  try {
    in >> j;
    if (j.value("format", "") != "kicad-em-fdtd.selection")
      throw SettingsError("not a selection file: " + file.string());
    s.antennaRef = j.value("antenna_ref", "");
    for (const auto& n : j.value("picked_nets", Json::array()))
      s.pickedNets.insert(n.get<std::string>());
    for (const auto& n : j.value("extra_ground_nets", Json::array()))
      s.extraGroundNets.insert(n.get<std::string>());
    const Json parts = j.value("parts", Json::object());
    for (const auto& [ref, p] : parts.items()) {
      board::PartTreatment t;
      t.state = stateFrom(p.value("state", "auto"));
      t.fitDnp = p.value("fit_dnp", false);
      if (p.contains("r_ohm"))
        t.r = p.at("r_ohm").get<double>();
      if (p.contains("l_henry"))
        t.l = p.at("l_henry").get<double>();
      if (p.contains("c_farad"))
        t.c = p.at("c_farad").get<double>();
      s.parts[ref] = t;
    }
    const Json refinement = j.value("refinement", Json::object()); // named: items() of a temporary dangles
    for (const auto& [id, c] : refinement.items())
      s.refinement[id] = {c.value("enabled", false), c.value("cell_mm", 0.0)};
    const Json bodies = j.value("bodies", Json::object());
    for (const auto& [ref, b] : bodies.items())
      s.bodies[ref] = {bodyFrom(b.value("kind", "ignore")), b.value("epsilon_r", 4.0),
                       b.value("loss_tangent", 0.0), b.value("label", std::string())};
    if (j.contains("default_body"))
      s.defaultBody = {bodyFrom(j["default_body"].value("kind", "ignore")),
                       j["default_body"].value("epsilon_r", 4.0)};
  } catch (const Json::exception& e) {
    throw SettingsError("selection file " + file.string() + ": " + e.what());
  }
  return s;
}

} // namespace app
