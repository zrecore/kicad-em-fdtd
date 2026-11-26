// Radio profiles and the settings registry behind the solver's first screen.
//   data/radio_profiles.json  ->  Catalog (bands, channels, meshing rules; every number sourced)
//   Catalog + user changes    ->  Settings (one registry: label, plain-words help, group, tier, default,
//   source) Settings + board facts    ->  RunPlan (sweep, excitation band, cells): nothing here is a constant
//   in code.
// The same registry drives the preview screen, the search box, the saved files and the run summary.
#pragma once
#include <array>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace app {

struct SettingsError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct Channel {
  std::string label;
  double centerHz, widthHz;
};

struct Variant {
  std::string name;
  std::array<double, 2> bandHz;
  std::vector<Channel> channels;
  std::string source, status; // status: verified, unverified or assumed
};

struct MeshPreset {
  std::string label;
  double cellsPerWavelength;
};

struct Meshing {
  double sweepMarginFraction, excitationMarginFraction, portImpedanceOhm, boundaryDistanceWavelengths;
  std::size_t sweepPoints;
  std::string defaultPreset;
  std::vector<std::pair<std::string, MeshPreset>> presets; // in file order
};

// One fabricator stackup from data/fab_profiles.json, for a given number of copper layers. Dielectrics are
// listed top to bottom; a field named in `assumed` is not published by the fab (`source` says what was used).
struct FabDielectric {
  std::string name, material, source;
  double thickness, epsilonR, lossTangent; // metres
  std::vector<std::string> assumed;
};
struct FabProfile {
  std::string id, name, source, finish;
  std::size_t layers = 0;
  double copperOuterUm = 0, copperInnerUm = 0; // 0 = none
  std::vector<FabDielectric> dielectrics;
};
inline constexpr std::size_t kMaxDielectrics = 7; // settings slots: up to an 8-layer board

class Catalog {
public:
  static Catalog load(const std::string& path);
  static Catalog fromJson(const std::string& text); // throws SettingsError naming the missing field

  const std::string& defaultProfile() const { return defaultProfile_; }
  std::vector<std::string> profileIds() const;
  std::string profileName(const std::string& id) const;
  std::vector<std::string> variantIds(const std::string& profile) const;
  std::string defaultVariant(const std::string& profile) const;
  const Variant& variant(const std::string& profile, const std::string& variantId) const; // "" = default
  const Meshing& meshing() const { return meshing_; }
  const std::vector<FabProfile>& fabProfiles() const { return fab_; }
  const FabProfile& fabProfile(const std::string& id) const; // throws SettingsError if unknown
  // Default field colours ("#RRGGBB") from data/ui_defaults.json; key is "dark_negative", "dark_positive",
  // ...
  const std::map<std::string, std::string>& uiColours() const { return uiColours_; }
  // Sequential colour maps from data/colormaps.json (id -> 256 entries, dark to light, and the source's
  // note).
  struct ColourMapDef {
    std::string note;
    std::vector<std::array<std::uint8_t, 3>> colours;
  };
  const std::map<std::string, ColourMapDef>& colourMaps() const { return colourMaps_; }

private:
  struct Profile {
    std::string id, name, defaultVariant;
    std::vector<std::pair<std::string, Variant>> variants;
  };
  const Profile& profile(const std::string& id) const;
  std::string defaultProfile_;
  Meshing meshing_{};
  std::map<std::string, std::string> uiColours_{{"dark_negative", "#00BEEB"},
                                                {"dark_positive", "#D2149F"},
                                                {"light_negative", "#2150AA"},
                                                {"light_positive", "#B2182B"}};
  std::map<std::string, ColourMapDef> colourMaps_; // empty if data/colormaps.json is absent
  std::vector<Profile> profiles_;
  std::vector<FabProfile> fab_; // from fab_profiles.json next to the radio profiles; empty if absent
};

enum class Tier { Essential, Common, Advanced, Expert };
enum class Kind { Number, Choice, Colour }; // Colour: text "#RRGGBB"
enum class Source { Default, Profile, User };
using Value = std::variant<double, std::string>;

struct SettingDef {
  std::string key, label, help, group, unit;
  Tier tier;
  Kind kind;
  double min = 0, max = 0;           // Number
  std::vector<std::string> keywords; // extra search words
};

class Settings {
public:
  explicit Settings(const Catalog& catalog);

  const std::vector<SettingDef>& defs() const { return defs_; }
  const SettingDef& def(const std::string& key) const;
  const Catalog& catalog() const { return catalog_; }
  double number(const std::string& key) const;
  std::string text(const std::string& key) const;
  Source source(const std::string& key) const;
  std::vector<std::string> choices(const std::string& key) const; // for Kind::Choice, current valid values

  void set(const std::string& key, const Value& v); // validates; throws SettingsError naming the key
  void reset(const std::string& key);
  void resetAll();
  void selectProfile(const std::string& profile, const std::string& variant = "");
  std::vector<std::string> modifiedKeys() const;
  const Variant& currentVariant() const;

  std::string toJson() const; // the user's deviations only
  void fromJson(const std::string& text, std::vector<std::string>& problems);

  // Keys to show: the essential ones, plus every non-essential setting of the expanded groups; with a
  // non-empty query, every setting that matches it (any tier), in registry order.
  std::vector<std::string> visible(const std::set<std::string>& expandedGroups,
                                   const std::string& query) const;
  std::string groupOf(const std::string& key) const { return def(key).group; }
  std::vector<std::string> groups() const; // in registry order

private:
  std::string pcbProfileId() const; // "board" or a fab profile id
  Value defaultValue(const std::string& key) const;
  Value effective(const std::string& key) const;
  void build();
  const Catalog& catalog_;
  std::vector<SettingDef> defs_;
  std::map<std::string, Value> user_;
};

// "#RRGGBB" to bytes; throws SettingsError if it is not that form.
std::array<std::uint8_t, 3> parseColour(const std::string& text);

// Everything the run needs, derived from the settings and two facts about the board.
struct RunPlan {
  std::array<double, 2> bandHz, sweepHz, excitationHz;
  std::size_t sweepPoints;
  double portImpedanceOhm;
  double coarseCell, fineCell, dielectricCell, airMargin; // metres
  double airNearCell, airNearHeight;                      // metres, 0: off
  double airGrowth;                                       // air cell growth ratio next to the board, 1: even
  double lateralGrowth;    // lateral cell growth ratio away from refinement boxes
  bool copperSlab = false; // copper layers with a thickness fill cells of it
  std::size_t pmlCells, maxSteps, sampleEvery;
  double courant, decay;
  std::string backend;        // auto, gpu or cpu
  std::string fieldComponent; // ez, ex or ey
  std::size_t animationFrequencies, volumeResolution;
};

// maxEpsilonR sets the shortest wavelength; minDielectricThickness (0 = unknown) bounds the vertical cell.
RunPlan makePlan(const Settings& s, double maxEpsilonR, double minDielectricThickness = 0);

// Releases the search text once the user has stopped typing for `delay` seconds.
class Debouncer {
public:
  explicit Debouncer(double delaySeconds) : delay_(delaySeconds) {}
  void update(const std::string& text, double nowSeconds) {
    text_ = text;
    changed_ = nowSeconds;
    pending_ = true;
  }
  std::optional<std::string> poll(double nowSeconds) {
    if (pending_ && nowSeconds - changed_ >= delay_) {
      pending_ = false;
      return text_;
    }
    return std::nullopt;
  }

private:
  double delay_, changed_ = 0;
  std::string text_;
  bool pending_ = false;
};

} // namespace app
