#include "app/settings.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

#include "core/units.hpp"
#include "sim/simulation.hpp"

namespace app {
namespace {

using Json = nlohmann::ordered_json;

const Json& need(const Json& j, const char* key, const std::string& where) {
  if (!j.is_object() || !j.contains(key))
    throw SettingsError("radio profiles: missing field '" + std::string(key) + "'" +
                        (where.empty() ? "" : " in " + where));
  return j.at(key);
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

// diel1_thickness_mm ... diel7_loss_tangent, copper_outer_um, copper_inner_um (not dielectric_cells_min).
bool isPcbNumber(const std::string& key) {
  return key.rfind("copper_", 0) == 0 ||
         (key.size() > 6 && key.rfind("diel", 0) == 0 && key[4] >= '1' && key[4] <= '9' && key[5] == '_');
}

} // namespace

std::array<std::uint8_t, 3> parseColour(const std::string& text) {
  if (text.size() != 7 || text[0] != '#')
    throw SettingsError("colour must look like #RRGGBB, got '" + text + "'");
  std::array<std::uint8_t, 3> rgb{};
  for (int i = 0; i < 3; ++i) {
    char* end = nullptr;
    const std::string part = text.substr(1 + 2 * static_cast<std::size_t>(i), 2);
    const long v = std::strtol(part.c_str(), &end, 16);
    if (*end != 0)
      throw SettingsError("colour must look like #RRGGBB, got '" + text + "'");
    rgb[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v);
  }
  return rgb;
}

// ---- Catalog ----------------------------------------------------------------------------------------------

Catalog Catalog::load(const std::string& path) {
  std::ifstream f(path);
  if (!f)
    throw SettingsError("cannot open " + path);
  std::ostringstream ss;
  ss << f.rdbuf();
  Catalog c = fromJson(ss.str());
  const std::string ui = path.substr(0, path.find_last_of("/\\") + 1) + "ui_defaults.json";
  if (std::ifstream uf(ui); uf) { // optional: field colours per theme
    std::ostringstream us;
    us << uf.rdbuf();
    try {
      const Json u = Json::parse(us.str());
      for (const std::string theme : {"dark", "light"})
        for (const std::string side : {"negative", "positive"})
          if (u.contains("colours") && u.at("colours").contains(theme) &&
              u.at("colours").at(theme).contains(side)) {
            const std::string value = u.at("colours").at(theme).at(side).get<std::string>();
            parseColour(value); // validate
            c.uiColours_[theme + "_" + side] = value;
          }
    } catch (const std::exception& e) {
      throw SettingsError("ui_defaults.json: " + std::string(e.what()));
    }
  }
  const std::string cmaps = path.substr(0, path.find_last_of("/\\") + 1) + "colormaps.json";
  if (std::ifstream cf(cmaps); cf) { // optional: sequential colour maps for the 3D magnitude view
    std::ostringstream cs;
    cs << cf.rdbuf();
    try {
      const Json u = Json::parse(cs.str());
      for (const auto& [id, def] : u.at("maps").items()) {
        ColourMapDef m;
        m.note = def.value("note", "");
        for (const Json& hex : def.at("colours")) {
          const auto rgb = parseColour(hex.get<std::string>());
          m.colours.push_back({rgb[0], rgb[1], rgb[2]});
        }
        if (m.colours.size() < 2)
          throw SettingsError("colour map '" + id + "' needs at least two entries");
        c.colourMaps_[id] = std::move(m);
      }
    } catch (const std::exception& e) {
      throw SettingsError("colormaps.json: " + std::string(e.what()));
    }
  }
  const std::string fab = path.substr(0, path.find_last_of("/\\") + 1) + "fab_profiles.json";
  if (std::ifstream ff(fab); ff) { // optional: PCB stackups per fabricator
    std::ostringstream fs;
    fs << ff.rdbuf();
    try {
      const Json j = Json::parse(fs.str());
      for (const auto& [id, p] : j.at("profiles").items()) {
        FabProfile fp;
        fp.id = id;
        fp.name = p.value("name", id);
        fp.source = p.value("source", "");
        fp.finish = p.value("finish", "");
        fp.layers = p.at("layers").get<std::size_t>();
        fp.copperOuterUm = p.value("copper_outer_um", 0.0);
        fp.copperInnerUm = p.contains("copper_inner_um") && !p.at("copper_inner_um").is_null()
                               ? p.at("copper_inner_um").get<double>()
                               : 0.0;
        if (p.contains("dielectrics"))
          for (const Json& d : p.at("dielectrics")) {
            FabDielectric fd;
            fd.name = d.at("name").get<std::string>();
            fd.material = d.value("material", "");
            fd.source = d.value("source", "");
            fd.thickness = d.at("thickness_mm").get<double>() * 1e-3;
            fd.epsilonR = d.at("epsilon_r").get<double>();
            fd.lossTangent = d.at("loss_tangent").get<double>();
            fd.assumed = d.value("assumed", std::vector<std::string>{});
            if (!(fd.thickness > 0) || !(fd.epsilonR >= 1) || fd.lossTangent < 0)
              throw SettingsError("profile " + id + ", dielectric " + fd.name + ": bad value");
            fp.dielectrics.push_back(std::move(fd));
          }
        if (fp.dielectrics.size() > kMaxDielectrics)
          throw SettingsError("profile " + id + ": more than " + std::to_string(kMaxDielectrics) +
                              " dielectrics");
        c.fab_.push_back(std::move(fp));
      }
    } catch (const std::exception& e) {
      throw SettingsError("fab_profiles.json: " + std::string(e.what()));
    }
  }
  return c;
}

const FabProfile& Catalog::fabProfile(const std::string& id) const {
  for (const FabProfile& f : fab_)
    if (f.id == id)
      return f;
  throw SettingsError("unknown PCB profile '" + id + "'");
}

Catalog Catalog::fromJson(const std::string& text) {
  Json j;
  try {
    j = Json::parse(text);
  } catch (const Json::exception& e) {
    throw SettingsError(std::string("radio profiles: not valid JSON: ") + e.what());
  }
  try {
    Catalog c;
    c.defaultProfile_ = need(j, "default_profile", "").get<std::string>();
    const Json& m = need(j, "meshing", "");
    c.meshing_.sweepMarginFraction = need(m, "sweep_margin_fraction", "meshing").get<double>();
    c.meshing_.excitationMarginFraction = need(m, "excitation_margin_fraction", "meshing").get<double>();
    c.meshing_.sweepPoints = need(m, "sweep_points", "meshing").get<std::size_t>();
    c.meshing_.portImpedanceOhm = need(m, "port_impedance_ohm", "meshing").get<double>();
    c.meshing_.boundaryDistanceWavelengths =
        need(m, "boundary_distance_wavelengths", "meshing").get<double>();
    c.meshing_.defaultPreset = need(m, "default_preset", "meshing").get<std::string>();
    for (const auto& [id, p] : need(m, "presets", "meshing").items())
      c.meshing_.presets.push_back(
          {id, {need(p, "label", id).get<std::string>(), need(p, "cells_per_wavelength", id).get<double>()}});
    for (const auto& [id, p] : need(j, "profiles", "").items()) {
      Profile profile{
          id, need(p, "name", id).get<std::string>(), need(p, "default_variant", id).get<std::string>(), {}};
      for (const auto& [vid, v] : need(p, "variants", id).items()) {
        Variant var;
        var.name = need(v, "name", vid).get<std::string>();
        const Json& band = need(v, "band_hz", vid);
        var.bandHz = {band.at(0).get<double>(), band.at(1).get<double>()};
        for (const Json& ch : need(v, "channels", vid))
          var.channels.push_back({ch.at("label").get<std::string>(), ch.at("center_hz").get<double>(),
                                  ch.at("width_hz").get<double>()});
        var.source = need(v, "source", vid).get<std::string>();
        var.status = need(v, "status", vid).get<std::string>();
        profile.variants.push_back({vid, std::move(var)});
      }
      c.profiles_.push_back(std::move(profile));
    }
    if (c.profiles_.empty())
      throw SettingsError("radio profiles: no profiles");
    c.profile(c.defaultProfile_); // the default must exist
    return c;
  } catch (const Json::exception& e) {
    throw SettingsError(std::string("radio profiles: wrong field type: ") + e.what());
  }
}

const Catalog::Profile& Catalog::profile(const std::string& id) const {
  for (const Profile& p : profiles_)
    if (p.id == id)
      return p;
  throw SettingsError("unknown profile '" + id + "'");
}

std::vector<std::string> Catalog::profileIds() const {
  std::vector<std::string> ids;
  for (const Profile& p : profiles_)
    ids.push_back(p.id);
  return ids;
}
std::string Catalog::profileName(const std::string& id) const {
  return profile(id).name;
}
std::vector<std::string> Catalog::variantIds(const std::string& id) const {
  std::vector<std::string> ids;
  for (const auto& v : profile(id).variants)
    ids.push_back(v.first);
  return ids;
}
std::string Catalog::defaultVariant(const std::string& id) const {
  return profile(id).defaultVariant;
}
const Variant& Catalog::variant(const std::string& id, const std::string& variantId) const {
  const Profile& p = profile(id);
  const std::string& want = variantId.empty() ? p.defaultVariant : variantId;
  for (const auto& v : p.variants)
    if (v.first == want)
      return v.second;
  throw SettingsError("unknown variant '" + want + "' of profile '" + id + "'");
}

// ---- Settings ---------------------------------------------------------------------------------------------

Settings::Settings(const Catalog& catalog) : catalog_(catalog) {
  build();
}

void Settings::build() {
  auto num = [&](const char* key, const char* label, const char* help, const char* group, const char* unit,
                 Tier tier, double lo, double hi, std::vector<std::string> kw = {}) {
    defs_.push_back({key, label, help, group, unit, tier, Kind::Number, lo, hi, std::move(kw)});
  };
  auto choice = [&](const char* key, const char* label, const char* help, const char* group, Tier tier,
                    std::vector<std::string> kw = {}) {
    defs_.push_back({key, label, help, group, "", tier, Kind::Choice, 0, 0, std::move(kw)});
  };
  choice("profile", "Radio profile",
         "The kind of radio you are designing for; it sets the frequency band and channel markers.", "Band",
         Tier::Essential, {"wifi", "bluetooth", "zigbee", "lwmesh", "uwb", "standard", "protocol"});
  choice("variant", "Profile variant", "Which band of the profile (for example 2.4 GHz or sub-GHz).", "Band",
         Tier::Essential, {"2.4", "5 ghz", "sub-ghz", "868", "915"});
  num("band_min_ghz", "Band start", "Lowest frequency you care about. S11 is plotted around this band.",
      "Band", "GHz", Tier::Essential, 0.01, 100, {"frequency", "sweep", "lower"});
  num("band_max_ghz", "Band end", "Highest frequency you care about.", "Band", "GHz", Tier::Essential, 0.01,
      100, {"frequency", "sweep", "upper"});
  choice("mesh_preset", "Mesh quality",
         "Coarse is fast and rough, fine is slow and accurate. The preview shows the cell count and time.",
         "Mesh", Tier::Essential, {"resolution", "cells", "accuracy", "speed", "grid"});
  num("sweep_points", "Frequency points", "How many frequencies S11 is reported at.", "Band", "",
      Tier::Common, 11, 2001, {"resolution", "sweep", "samples"});
  num("sweep_margin_fraction", "Sweep margin",
      "Extra range reported on each side of the band, as a fraction of the frequency, so a resonance that "
      "landed "
      "off-target is still visible.",
      "Band", "", Tier::Common, 0, 1, {"sweep", "range", "wider"});
  num("port_impedance_ohm", "Port impedance", "Reference impedance of the port; 50 ohm for most RF designs.",
      "Port", "ohm", Tier::Common, 1, 1000, {"reference", "50", "match", "source"});
  num("boundary_distance_wavelengths", "Free space around the board",
      "Distance from the board to the absorbing boundary, in wavelengths. Larger is more accurate and "
      "slower.",
      "Mesh", "wavelengths", Tier::Common, 0.05, 2, {"margin", "domain", "air", "boundary"});
  num("cells_per_wavelength", "Cells per wavelength (override)",
      "0 uses the mesh quality preset. Otherwise the coarse cell is the shortest wavelength in the board "
      "divided by this.",
      "Mesh", "", Tier::Advanced, 0, 100, {"resolution", "cells", "grid", "mesh"});
  num("fine_cell_mm", "Fine cell near ports",
      "Cell size near ports and small features. 0 picks it automatically.", "Mesh", "mm", Tier::Advanced, 0,
      10, {"refine", "gap", "feature", "local"});
  num("refine_copper_edges_mm", "Fine cell at open copper ends",
      "Cell size within 3 mm of every open end of the copper a port feeds (a patch's edges, an antenna's "
      "tips), where the field piles up. With a coarse cell there the resonance reads low (a 60 mm patch: 3 "
      "percent). Refining costs many more cells (about 18 times on that patch): the preview shows them. 0 "
      "turns it off, and a run then warns that open ends use the coarse cell.",
      "Mesh", "mm", Tier::Advanced, 0, 10, {"refine", "edge", "end", "tip", "patch", "open"});
  num("air_near_cell_mm", "Fine air cell next to the board",
      "Vertical cell size in the air layer next to the board, so a part body thinner than the coarse cells "
      "is modelled (a body smaller than the grid is left out and reported). 0 keeps the coarse cell.",
      "Mesh", "mm", Tier::Advanced, 0, 10, {"body", "height", "vertical", "z", "air"});
  num("air_near_height_mm", "Fine air layer height",
      "Thickness of the layer that uses the fine air cell; the cells then grow to the coarse size. Set it to "
      "the tallest body.",
      "Mesh", "mm", Tier::Advanced, 0, 50, {"body", "height", "vertical", "z", "air"});
  num("lateral_growth_ratio", "Mesh growth away from refined areas",
      "How fast the cells along the board (both horizontal axes, x and y) grow from a refined box (the "
      "antenna, a port) out to the coarse size: each cell is at most this much larger than the one before. "
      "Smaller is smoother and slower. "
      "Measured on the TI antenna board: 1.1 gave 0.2 percent scatter in the resonance between 0.2 and 0.1 "
      "mm "
      "cells, 1.3 about 1.3 percent.",
      "Mesh", "", Tier::Advanced, 1, 2, {"grading", "smooth", "lateral", "ratio", "refine"});
  num("mesh_growth_ratio", "Mesh growth next to the board",
      "How fast the air cells above and below the board grow from the board's own cell size up to the coarse "
      "size: each cell is at most this much larger than the one before. 1.3 keeps neighbouring cells smooth "
      "(a hard jump reads a planar line's impedance about 11 percent low); 1 splits the air evenly, as "
      "earlier versions did.",
      "Mesh", "", Tier::Advanced, 1, 2, {"grading", "smooth", "air", "vertical", "z", "ratio"});
  num("dielectric_cells_min", "Cells across each dielectric layer",
      "Minimum vertical cells through each board layer.", "Mesh", "", Tier::Advanced, 1, 20,
      {"layer", "thickness", "vertical", "z"});
  num("excitation_margin_fraction", "Excitation margin",
      "How far beyond the reported sweep the source pulse extends, as a fraction of the sweep width. Keeps "
      "the edges of S11 clean.",
      "Band", "", Tier::Advanced, 0, 2, {"pulse", "source", "spectrum"});
  num("pml_cells", "Absorbing boundary layers", "Thickness of the absorbing boundary (CPML) in cells.",
      "Numerics", "", Tier::Advanced, 2, 32, {"absorbing", "boundary", "cpml", "pml", "reflection"});
  num("courant", "Time step safety", "Fraction of the largest stable time step to use; keep below 1.",
      "Numerics", "", Tier::Expert, 0.1, 0.999, {"courant", "stability", "dt", "cfl"});
  num("max_steps", "Maximum steps", "Safety limit on the number of time steps.", "Numerics", "",
      Tier::Advanced, 100, 1e8, {"limit", "duration", "iterations"});
  num("decay_threshold", "Stop when signal decays to",
      "The run ends when the port voltage falls below this fraction of its peak.", "Numerics", "",
      Tier::Advanced, 1e-9, 0.5, {"stop", "converge", "ringdown", "end"});
  num("sample_every", "Steps per field sample",
      "Steps between field samples for the animation and the S11 record.", "Numerics", "", Tier::Expert, 1,
      256, {"cadence", "snapshot", "animation", "submit"});
  choice("ui_theme", "Appearance",
         "Light, dark, or follow the system (auto). The field slice and the 3D view follow it unless "
         "you choose their panels below.",
         "View", Tier::Essential, {"theme", "dark mode", "light mode", "appearance", "colour scheme"});
  choice("reduce_motion", "Reduce motion",
         "Follow the system's Reduce Motion setting, or force it on or off. When on, the animation starts "
         "paused and the camera jumps instead of easing.",
         "View", Tier::Essential, {"accessibility", "a11y", "animation", "motion", "pause"});
  choice(
      "high_contrast", "High contrast",
      "Follow the system's Increase Contrast setting, or force it on or off. When on, text and borders use "
      "maximum contrast and panels are opaque.",
      "View", Tier::Essential, {"accessibility", "a11y", "contrast", "colour", "transparency"});
  choice("text_scale", "Text size",
         "Follow the system's text-size setting, or choose a size. Everything in the window scales with it.",
         "View", Tier::Essential, {"accessibility", "a11y", "font", "zoom", "large text", "size"});
  choice("ui_font", "Font",
         "System uses the font of your operating system's interface. Built-in is the program's own small "
         "monospace font, which some people find easier to read and which looks the same on every computer.",
         "View", Tier::Essential, {"accessibility", "a11y", "font", "typeface", "monospace", "text"});
  choice("slice_panel", "Field slice panel",
         "Background of the field slice: follow the appearance above, or always light or always dark.",
         "View", Tier::Essential, {"light panel", "dark panel", "slice", "background", "theme"});
  choice("volume_panel", "3D view panel",
         "Background of the 3D view: follow the appearance above, or always light or always dark.", "View",
         Tier::Essential, {"light panel", "dark panel", "3d", "volume", "background", "theme"});
  auto colour = [&](const char* key, const char* label, const char* help) {
    defs_.push_back({key,
                     label,
                     help,
                     "View",
                     "",
                     Tier::Common,
                     Kind::Colour,
                     0,
                     0,
                     {"colour", "color", "theme", "palette", "negative", "positive", "wave"}});
  };
  colour("colour_dark_negative", "Dark theme: negative field",
         "Colour of negative field values on dark panels (default Okabe-Ito sky blue, colour-blind safe).");
  colour("colour_dark_positive", "Dark theme: positive field",
         "Colour of positive field values on dark panels and in the 3D view on a dark theme (default "
         "Okabe-Ito yellow).");
  colour(
      "colour_light_negative", "Light theme: negative field",
      "Colour of negative field values on light panels and in the 3D view on a light theme (default blue).");
  colour("colour_light_positive", "Light theme: positive field",
         "Colour of positive field values on light panels (default red).");
  choice("field_component", "Field component shown",
         "Which electric field component the slice and the 3D volume display. Ez is the one between the "
         "board's copper layers.",
         "View", Tier::Common, {"ez", "ex", "ey", "polarization", "direction", "e-field"});
  num("animation_frequencies", "Frequencies kept for the animation",
      "How many frequencies across the sweep the field animation can show. More costs memory and time.",
      "View", "", Tier::Advanced, 3, 41, {"phasor", "slice", "volume", "animation"});
  num("volume_resolution", "3D volume resolution",
      "Voxels along the longest side of the volume view. Higher is sharper and uses more memory.", "View",
      "voxels", Tier::Advanced, 16, 160, {"3d", "volume", "detail", "voxel"});
  choice(
      "volume_colour_map", "3D volume colours",
      "How the 3D view colours the field. signed: two colours by sign, animated through the phase of the "
      "wave (default). The others colour the size of the field with a sequential colour map (dark to "
      "light, no sign); they animate too (the colour pulses at twice the wave's frequency), and when paused "
      "they show the peak magnitude at the chosen frequency, which is the one to use for finding hot spots "
      "and the pattern of an antenna. viridis, magma, inferno and plasma are CC0 maps by Smith, van der Walt "
      "and Firing; "
      "viridis was designed to stay readable with common colour-vision deficiencies.",
      "View", Tier::Common, {"colour map", "viridis", "magma", "inferno", "plasma", "magnitude", "heat"});
  choice(
      "board_outline", "Board outline",
      "proper (default): when the board is drawn inside a panel, only the board itself is modelled and the "
      "frame, its copper and the parts on it are left out (the export found the board between the slots "
      "and tabs). whole: the whole Edge.Cuts as drawn, frame included. Has no effect when the export found "
      "no panel.",
      "PCB", Tier::Essential, {"panel", "frame", "breakaway", "tabs", "edge.cuts", "outline", "slots"});
  choice("pcb_profile", "PCB stackup",
         "Where the board's layers come from: the open board's own stackup (default), or a fabricator's "
         "published stackup for the same number of copper layers. The values below start from it and you can "
         "change any of them; a value left at 0 keeps the board's own.",
         "PCB", Tier::Essential,
         {"fab", "oshpark", "osh park", "pcbway", "stackup", "fr4", "enig", "layers"});
  for (std::size_t i = 1; i <= kMaxDielectrics; ++i) {
    const std::string n = std::to_string(i),
                      where = " Dielectric layers are counted from the top of the board.";
    auto slot = [&](const std::string& key, const std::string& label, const std::string& help,
                    const char* unit, double hi, std::vector<std::string> kw) {
      kw.insert(kw.end(), {"pcb", "stackup", "dielectric", "layer", "material", "substrate"});
      defs_.push_back({key, label,
                       help + where +
                           " 0 keeps the board's value; unused if the board has "
                           "fewer dielectric layers.",
                       "PCB", unit, Tier::Advanced, Kind::Number, 0, hi, std::move(kw)});
    };
    slot("diel" + n + "_thickness_mm", "Dielectric " + n + " thickness", "Thickness of this layer.", "mm", 10,
         {"height", "core", "prepreg"});
    slot("diel" + n + "_epsilon_r", "Dielectric " + n + " permittivity", "Relative permittivity (Dk).", "",
         100, {"er", "dk", "epsilon"});
    slot("diel" + n + "_loss_tangent", "Dielectric " + n + " loss tangent", "Loss tangent (Df, tan delta).",
         "", 1, {"df", "tan", "loss"});
  }
  choice(
      "component_bodies", "Component bodies",
      "What to do with the bodies of the parts on the board (their footprint outline, extruded by the part's "
      "height). ignore: only a part's copper is modelled (default). dielectric: a block of the "
      "permittivity below, for plastic or ceramic packages. metal: a perfect conductor, for shield cans and "
      "connector shells. Applies to every part in the model; a part the board file gives no outline or "
      "height for is left out and named in the run summary.",
      "PCB", Tier::Essential,
      {"component", "package", "body", "3d", "shield", "ic", "footprint", "courtyard"});
  num("component_body_eps", "Component body permittivity",
      "Relative permittivity of a dielectric component body. Assumed: package materials vary (about 3.5 to "
      "4.5 "
      "for moulded plastic, 6 to 10 for ceramic); set it from the part's datasheet.",
      "PCB", "", Tier::Essential, 1, 100, {"component", "package", "dk", "epsilon", "ceramic"});
  num("component_body_height_mm", "Component body height",
      "Height used for a part's body when the board file gives none (KiCAD's API does not report footprint "
      "heights). Assumed: set it to the tallest relevant part, such as a shield can or connector.",
      "PCB", "mm", Tier::Essential, 0.05, 30, {"component", "package", "height", "tall", "shield"});
  num("copper_outer_um", "Outer copper thickness",
      "Finished copper on the outer layers. Used by the feed-line impedance estimate in the Antenna tab "
      "(copper's thickness lowers a coplanar line's impedance by 6 to 9 percent); the solver models copper "
      "as "
      "a zero-thickness sheet. 0 keeps the board's value.",
      "PCB", "um", Tier::Advanced, 0, 500, {"pcb", "oz", "weight", "copper", "foil"});
  num("copper_inner_um", "Inner copper thickness",
      "Copper on the inner layers (boards with 4 or more layers). Used by the feed-line impedance estimate "
      "only; the solver models copper as a sheet. 0 keeps the board's value.",
      "PCB", "um", Tier::Advanced, 0, 500, {"pcb", "oz", "weight", "copper", "foil"});
  choice(
      "copper_model", "Copper model",
      "sheet (default): copper is a zero-thickness conducting sheet on its dielectric interface, as in every "
      "earlier version. slab: a copper layer that has a thickness (from the board, the fab profile or the "
      "copper thickness settings above) fills grid cells of that thickness, which lowers a trace's "
      "effective permittivity by about 1 percent for 35 um copper and raises its resonance by about the same "
      "(docs/PLAN.md). The cells are as thin as the copper, so the time step falls and a run takes longer; "
      "a layer without a thickness stays a sheet.",
      "PCB", Tier::Advanced, {"copper", "thickness", "slab", "sheet", "foil", "oz", "weight"});
  choice(
      "save_fields", "Keep field data of each run",
      "on (default): a finished run keeps its field data (the 3D and slice views, the surface current, power "
      "flow and far field) so it can be opened again without solving; this takes tens to hundreds of "
      "megabytes per run. off: only the S11 and the summary are kept. Runs can be pruned later (Open run, "
      "or the command line).",
      "Run", Tier::Advanced, {"save", "disk", "reload", "open run", "fields", "space", "prune"});
  num("antenna_keepout_along_mm", "Antenna keepout, width",
      "Width of the antenna's copper-free keepout along the board edge, from the part's datasheet. 0 uses "
      "the value of the antenna rule that matches the part, if any. Used by the Antenna tab.",
      "Antenna", "mm", Tier::Essential, 0, 100, {"antenna", "keepout", "clearance", "keep out"});
  num("antenna_keepout_depth_mm", "Antenna keepout, depth",
      "How far the keepout reaches into the board from its edge, from the part's datasheet. 0 uses the "
      "antenna rule's value. Used by the Antenna tab.",
      "Antenna", "mm", Tier::Essential, 0, 100, {"antenna", "keepout", "clearance", "keep out"});
  choice(
      "antenna_keepout_placement", "Antenna keepout placement",
      "Where the keepout box sits. rule (default): as the antenna rule says, else edge. edge: centred on "
      "the part, starting at the nearest board edge and reaching inward. corner: starting at the corner of "
      "the outline nearest the part, along the nearer edge towards the part and inward; for parts the "
      "datasheet says to mount in a corner. Used by the Antenna tab.",
      "Antenna", Tier::Essential, {"antenna", "keepout", "corner", "edge", "placement", "mounting"});
  num("antenna_testboard_length_mm", "Vendor test board, length",
      "Length of the vendor's evaluation board, from the datasheet. The Antenna tab compares the board's "
      "ground with it. 0 uses the antenna rule's value, if any.",
      "Antenna", "mm", Tier::Advanced, 0, 1000, {"antenna", "test board", "evaluation board", "ground"});
  num("antenna_testboard_width_mm", "Vendor test board, width",
      "Width of the vendor's evaluation board, from the datasheet. 0 uses the antenna rule's value, if any.",
      "Antenna", "mm", Tier::Advanced, 0, 1000, {"antenna", "test board", "evaluation board", "ground"});
  choice("backend", "Compute backend", "auto uses the GPU when one is available.", "Run", Tier::Advanced,
         {"gpu", "cpu", "vulkan", "device"});
}

const SettingDef& Settings::def(const std::string& key) const {
  for (const SettingDef& d : defs_)
    if (d.key == key)
      return d;
  throw SettingsError("unknown setting '" + key + "'");
}

Value Settings::defaultValue(const std::string& key) const {
  const Meshing& m = catalog_.meshing();
  const sim::RunOptions run;
  if (key == "profile")
    return catalog_.defaultProfile();
  if (key == "variant")
    return catalog_.defaultVariant(text("profile"));
  if (key == "band_min_ghz")
    return currentVariant().bandHz[0] * 1e-9;
  if (key == "band_max_ghz")
    return currentVariant().bandHz[1] * 1e-9;
  if (key == "mesh_preset")
    return m.defaultPreset;
  if (key == "sweep_points")
    return static_cast<double>(m.sweepPoints);
  if (key == "sweep_margin_fraction")
    return m.sweepMarginFraction;
  if (key == "excitation_margin_fraction")
    return m.excitationMarginFraction;
  if (key == "port_impedance_ohm")
    return m.portImpedanceOhm;
  if (key == "boundary_distance_wavelengths")
    return m.boundaryDistanceWavelengths;
  if (key == "cells_per_wavelength" || key == "fine_cell_mm" || key == "air_near_cell_mm" ||
      key == "air_near_height_mm")
    return 0.0;
  if (key == "refine_copper_edges_mm")
    return 0.12;
  if (key == "dielectric_cells_min")
    return 3.0;
  if (key == "mesh_growth_ratio" || key == "lateral_growth_ratio")
    return 1.3;
  if (key == "pml_cells")
    return static_cast<double>(run.cpmlCells);
  if (key == "courant")
    return run.courant;
  if (key == "max_steps")
    return static_cast<double>(run.maxSteps);
  if (key == "decay_threshold")
    return run.decay;
  if (key == "sample_every")
    return static_cast<double>(run.sampleEvery);
  if (key == "ui_theme")
    return std::string("auto");
  if (key == "ui_font")
    return std::string("system");
  if (key == "slice_panel" || key == "volume_panel" || key == "reduce_motion" || key == "high_contrast" ||
      key == "text_scale")
    return std::string("follow");
  if (key.rfind("colour_", 0) == 0)
    return catalog_.uiColours().at(key.substr(7));
  if (key == "field_component")
    return std::string("ez");
  if (key == "copper_model")
    return std::string("sheet");
  if (key == "animation_frequencies")
    return 13.0;
  if (key == "volume_resolution")
    return 64.0;
  if (key == "backend")
    return std::string("auto");
  if (key == "volume_colour_map")
    return std::string("signed");
  if (key == "component_bodies")
    return std::string("ignore");
  if (key == "component_body_eps")
    return 4.0;
  if (key == "component_body_height_mm")
    return 1.0;
  if (key == "pcb_profile")
    return std::string("board");
  if (key == "board_outline")
    return std::string("proper");
  if (key == "save_fields")
    return std::string("on");
  if (key == "antenna_keepout_along_mm" || key == "antenna_keepout_depth_mm" ||
      key == "antenna_testboard_length_mm" || key == "antenna_testboard_width_mm")
    return 0.0; // 0: the matching antenna rule's value
  if (key == "antenna_keepout_placement")
    return std::string("rule");
  if (const std::string pid = pcbProfileId(); pid != "board") {
    const FabProfile& f = catalog_.fabProfile(pid);
    if (key == "copper_outer_um")
      return f.copperOuterUm;
    if (key == "copper_inner_um")
      return f.copperInnerUm;
    if (isPcbNumber(key) && key[0] == 'd') {
      const std::size_t i = static_cast<std::size_t>(key[4] - '0') - 1;
      if (i < f.dielectrics.size()) {
        const FabDielectric& d = f.dielectrics[i];
        return key.find("thickness") != std::string::npos ? d.thickness * 1e3
               : key.find("epsilon") != std::string::npos ? d.epsilonR
                                                          : d.lossTangent;
      }
    }
  }
  if (isPcbNumber(key))
    return 0.0; // not set: the board's own value
  throw SettingsError("unknown setting '" + key + "'");
}

std::string Settings::pcbProfileId() const {
  const auto it = user_.find("pcb_profile");
  return it != user_.end() ? std::get<std::string>(it->second) : "board";
}

Value Settings::effective(const std::string& key) const {
  const auto it = user_.find(key);
  return it != user_.end() ? it->second : defaultValue(key);
}

const Variant& Settings::currentVariant() const {
  const auto p = user_.find("profile"), v = user_.find("variant");
  const std::string profile = p != user_.end() ? std::get<std::string>(p->second) : catalog_.defaultProfile();
  return catalog_.variant(profile, v != user_.end() ? std::get<std::string>(v->second) : "");
}

double Settings::number(const std::string& key) const {
  const Value v = effective(key);
  if (!std::holds_alternative<double>(v))
    throw SettingsError("setting '" + key + "' is not a number");
  return std::get<double>(v);
}

std::string Settings::text(const std::string& key) const {
  const Value v = effective(key);
  if (!std::holds_alternative<std::string>(v))
    throw SettingsError("setting '" + key + "' is not a choice");
  return std::get<std::string>(v);
}

Source Settings::source(const std::string& key) const {
  if (user_.count(key))
    return Source::User;
  if (isPcbNumber(key))
    return pcbProfileId() != "board" && std::get<double>(defaultValue(key)) != 0 ? Source::Profile
                                                                                 : Source::Default;
  return key == "band_min_ghz" || key == "band_max_ghz" ? Source::Profile : Source::Default;
}

std::vector<std::string> Settings::choices(const std::string& key) const {
  if (key == "profile")
    return catalog_.profileIds();
  if (key == "variant")
    return catalog_.variantIds(text("profile"));
  if (key == "mesh_preset") {
    std::vector<std::string> ids;
    for (const auto& p : catalog_.meshing().presets)
      ids.push_back(p.first);
    return ids;
  }
  if (key == "backend")
    return {"auto", "gpu", "cpu"};
  if (key == "volume_colour_map") {
    std::vector<std::string> ids{"signed"};
    for (const auto& [id, def] : catalog_.colourMaps())
      ids.push_back(id);
    return ids;
  }
  if (key == "component_bodies")
    return {"ignore", "dielectric", "metal"};
  if (key == "board_outline")
    return {"proper", "whole"};
  if (key == "save_fields")
    return {"on", "off"};
  if (key == "antenna_keepout_placement")
    return {"rule", "edge", "corner"};
  if (key == "pcb_profile") {
    std::vector<std::string> ids{"board"};
    for (const FabProfile& f : catalog_.fabProfiles())
      ids.push_back(f.id);
    return ids;
  }
  if (key == "ui_theme")
    return {"auto", "light", "dark"};
  if (key == "slice_panel" || key == "volume_panel")
    return {"follow", "light", "dark"};
  if (key == "reduce_motion" || key == "high_contrast")
    return {"follow", "on", "off"};
  if (key == "text_scale")
    return {"follow", "100%", "125%", "150%", "200%", "250%"};
  if (key == "ui_font")
    return {"system", "builtin"};
  if (key == "field_component")
    return {"ez", "ex", "ey"};
  if (key == "copper_model")
    return {"sheet", "slab"};
  return {};
}

void Settings::set(const std::string& key, const Value& v) {
  const SettingDef& d = def(key);
  if (d.kind == Kind::Number) {
    if (!std::holds_alternative<double>(v))
      throw SettingsError("setting '" + key + "' needs a number");
    const double x = std::get<double>(v);
    if (!std::isfinite(x) || x < d.min || x > d.max)
      throw SettingsError("setting '" + key + "' must be between " + std::to_string(d.min) + " and " +
                          std::to_string(d.max));
  } else if (d.kind == Kind::Colour) {
    if (!std::holds_alternative<std::string>(v))
      throw SettingsError("setting '" + key + "' needs a colour like #RRGGBB");
    try {
      parseColour(std::get<std::string>(v));
    } catch (const SettingsError& e) {
      throw SettingsError("setting '" + key + "': " + e.what());
    }
  } else {
    if (!std::holds_alternative<std::string>(v))
      throw SettingsError("setting '" + key + "' needs one of its choices");
    const auto ch = choices(key);
    if (std::find(ch.begin(), ch.end(), std::get<std::string>(v)) == ch.end())
      throw SettingsError("setting '" + key + "': '" + std::get<std::string>(v) +
                          "' is not one of its choices");
  }
  if (key == "profile") { // changing the profile resets the variant and the band that came from it
    selectProfile(std::get<std::string>(v));
    return;
  }
  if (key == "variant") {
    selectProfile(text("profile"), std::get<std::string>(v));
    return;
  }
  user_[key] = v;
}

void Settings::reset(const std::string& key) {
  def(key);
  user_.erase(key);
  if (key == "profile") {
    user_.erase("variant");
    user_.erase("band_min_ghz");
    user_.erase("band_max_ghz");
  }
}

void Settings::resetAll() {
  user_.clear();
}

void Settings::selectProfile(const std::string& profile, const std::string& variant) {
  const auto ids = catalog_.profileIds();
  if (std::find(ids.begin(), ids.end(), profile) == ids.end())
    throw SettingsError("setting 'profile': unknown profile '" + profile + "'");
  catalog_.variant(profile, variant); // validates the variant
  user_["profile"] = profile;
  if (variant.empty() || variant == catalog_.defaultVariant(profile))
    user_.erase("variant");
  else
    user_["variant"] = variant;
  user_.erase("band_min_ghz"); // a new profile brings its own band
  user_.erase("band_max_ghz");
  if (profile == catalog_.defaultProfile() && !user_.count("variant"))
    user_.erase("profile");
}

std::vector<std::string> Settings::modifiedKeys() const {
  std::vector<std::string> keys;
  for (const SettingDef& d : defs_)
    if (user_.count(d.key))
      keys.push_back(d.key);
  return keys;
}

std::string Settings::toJson() const {
  Json j;
  j["format"] = "kicad-em-fdtd.settings";
  j["version"] = 1;
  j["profile"] = text("profile");
  j["variant"] = text("variant");
  Json values = Json::object();
  for (const auto& [key, v] : user_)
    if (key != "profile" && key != "variant") {
      if (std::holds_alternative<double>(v))
        values[key] = std::get<double>(v);
      else
        values[key] = std::get<std::string>(v);
    }
  j["values"] = values;
  return j.dump(2);
}

void Settings::fromJson(const std::string& textIn, std::vector<std::string>& problems) {
  Json j;
  try {
    j = Json::parse(textIn);
  } catch (const Json::exception& e) {
    problems.push_back(std::string("settings file is not valid JSON: ") + e.what());
    return;
  }
  try {
    if (j.contains("profile"))
      selectProfile(j.at("profile").get<std::string>(), j.value("variant", std::string()));
  } catch (const std::exception& e) {
    problems.push_back(e.what());
  }
  if (!j.contains("values"))
    return;
  for (const auto& [key, v] : j.at("values").items()) {
    try {
      def(key);
    } catch (const SettingsError&) {
      problems.push_back("unknown setting '" + key + "' (saved by a different version?) was ignored");
      continue;
    }
    try {
      set(key, v.is_string() ? Value(v.get<std::string>()) : Value(v.get<double>()));
    } catch (const std::exception& e) {
      problems.push_back(e.what());
    }
  }
}

std::vector<std::string> Settings::groups() const {
  std::vector<std::string> g;
  for (const SettingDef& d : defs_)
    if (std::find(g.begin(), g.end(), d.group) == g.end())
      g.push_back(d.group);
  return g;
}

std::vector<std::string> Settings::visible(const std::set<std::string>& expanded,
                                           const std::string& query) const {
  std::vector<std::string> out;
  std::vector<std::string> tokens;
  std::istringstream qs(lower(query));
  for (std::string t; qs >> t;)
    tokens.push_back(t);
  for (const SettingDef& d : defs_) {
    if (tokens.empty()) {
      if (d.tier == Tier::Essential || expanded.count(d.group))
        out.push_back(d.key);
      continue;
    }
    std::string hay = lower(d.key + " " + d.label + " " + d.help + " " + d.group + " " + d.unit);
    for (const auto& k : d.keywords)
      hay += " " + lower(k);
    if (std::all_of(tokens.begin(), tokens.end(),
                    [&](const std::string& t) { return hay.find(t) != std::string::npos; }))
      out.push_back(d.key);
  }
  return out;
}

// ---- Plan -------------------------------------------------------------------------------------------------

RunPlan makePlan(const Settings& s, double maxEpsilonR, double minDielectricThickness) {
  RunPlan p{};
  p.bandHz = {s.number("band_min_ghz") * 1e9, s.number("band_max_ghz") * 1e9};
  if (!(p.bandHz[0] < p.bandHz[1]))
    throw SettingsError("band: the start must be below the end");
  const double margin = s.number("sweep_margin_fraction");
  p.sweepHz = {p.bandHz[0] * (1 - margin), p.bandHz[1] * (1 + margin)};
  const double width = p.sweepHz[1] - p.sweepHz[0], ex = s.number("excitation_margin_fraction");
  p.excitationHz = {p.sweepHz[0] - ex * width, p.sweepHz[1] + ex * width};
  p.sweepPoints = static_cast<std::size_t>(s.number("sweep_points"));
  p.portImpedanceOhm = s.number("port_impedance_ohm");

  double cpw = s.number("cells_per_wavelength");
  if (cpw <= 0) {
    const std::string preset = s.text("mesh_preset");
    for (const auto& [id, mp] : s.catalog().meshing().presets)
      if (id == preset)
        cpw = mp.cellsPerWavelength;
  }
  if (cpw <= 0)
    throw SettingsError("setting 'mesh_preset': no cells per wavelength known for the chosen preset");
  const double lambdaMin =
      fdtd::units::kSpeedOfLight / (p.sweepHz[1] * std::sqrt(std::max(maxEpsilonR, 1.0)));
  p.coarseCell = lambdaMin / cpw;
  const double fineSetting = s.number("fine_cell_mm") * 1e-3;
  p.fineCell = fineSetting > 0 ? fineSetting
                               : std::max(p.coarseCell / 8, 0.05e-3); // auto: refine 8x, not below 0.05 mm
  p.fineCell = std::min(p.fineCell, p.coarseCell);
  p.dielectricCell = minDielectricThickness > 0
                         ? std::min(p.coarseCell, minDielectricThickness / s.number("dielectric_cells_min"))
                         : p.coarseCell;
  const double centre = 0.5 * (p.bandHz[0] + p.bandHz[1]);
  p.airMargin = s.number("boundary_distance_wavelengths") * fdtd::units::kSpeedOfLight / centre;
  p.airNearCell = s.number("air_near_cell_mm") * 1e-3;
  p.airNearHeight = s.number("air_near_height_mm") * 1e-3;
  p.airGrowth = s.number("mesh_growth_ratio");
  p.lateralGrowth = s.number("lateral_growth_ratio");
  p.copperSlab = s.text("copper_model") == "slab";
  p.pmlCells = static_cast<std::size_t>(s.number("pml_cells"));
  p.maxSteps = static_cast<std::size_t>(s.number("max_steps"));
  p.sampleEvery = static_cast<std::size_t>(s.number("sample_every"));
  p.courant = s.number("courant");
  p.decay = s.number("decay_threshold");
  p.backend = s.text("backend");
  p.fieldComponent = s.text("field_component");
  p.animationFrequencies = static_cast<std::size_t>(s.number("animation_frequencies"));
  p.volumeResolution = static_cast<std::size_t>(s.number("volume_resolution"));
  return p;
}

} // namespace app
