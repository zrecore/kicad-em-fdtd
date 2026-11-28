#include "app/cli_json.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "fdtd/sparam.hpp"

namespace app::clijson {

namespace fs = std::filesystem;
using Json = nlohmann::ordered_json;

namespace {

Json header(const std::string& version) {
  Json j;
  j["schema"] = kSchema;
  j["tool"] = "emfdtd";
  j["version"] = version;
  return j;
}

const char* tierName(Tier t) {
  switch (t) {
  case Tier::Essential:
    return "essential";
  case Tier::Common:
    return "common";
  case Tier::Advanced:
    return "advanced";
  default:
    return "expert";
  }
}

Json facts(const RunFacts& f) {
  Json j;
  j["profile"] = f.profile;
  j["variant"] = f.variant;
  j["variant_status"] = f.variantStatus;
  j["band_hz"] = f.bandHz;
  j["sweep_hz"] = f.sweepHz;
  j["excitation_hz"] = f.excitationHz;
  j["grid"] = {{"cells", f.cells}, {"cell_count", f.cellCount}, {"smallest_cell_m", f.smallestCellM}};
  j["time_step_s"] = f.timeStepS;
  j["estimate"] = {{"steps", f.estimatedSteps},
                   {"memory_bytes", f.estimatedMemoryBytes},
                   {"seconds", f.estimatedSeconds},
                   {"exceeds_memory", f.exceedsMemory}};
  if (f.fieldBytes)
    j["estimate"]["field_bytes"] = f.fieldBytes;
  j["backend"] = f.backend;
  j["device"] = f.device;
  j["changed_settings"] = f.changedSettings;
  j["notes"] = f.notes;
  return j;
}

std::string slurp(const fs::path& p) {
  std::ifstream f(p);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// "a b c" -> after the prefix, or empty if the line does not start with it.
bool after(const std::string& line, const std::string& prefix, std::string& rest) {
  if (line.compare(0, prefix.size(), prefix) != 0)
    return false;
  rest = line.substr(prefix.size());
  return true;
}

// The first frequency where the dB curve crosses `level` between points a (inside) and b (outside).
double crossing(const std::vector<double>& f, const std::vector<double>& db, std::size_t a, std::size_t b,
                double level) {
  const double t = (level - db[a]) / (db[b] - db[a]);
  return f[a] + t * (f[b] - f[a]);
}

Json s11Info(const fs::path& file, bool withTable) {
  std::ifstream in(file);
  const fdtd::SParam1 p = fdtd::readTouchstone(in);
  const std::size_t n = p.s11.size();
  Json j;
  j["points"] = n;
  if (n == 0) {
    j["best"] = nullptr;
    j["band_10db"] = nullptr;
    return j;
  }
  std::vector<double> db(n);
  for (std::size_t i = 0; i < n; ++i)
    db[i] = 20.0 * std::log10(std::max(std::abs(p.s11[i]), 1e-30));
  const std::size_t best = static_cast<std::size_t>(std::min_element(db.begin(), db.end()) - db.begin());
  j["f_min_hz"] = p.frequenciesHz.front();
  j["f_max_hz"] = p.frequenciesHz.back();
  j["best"] = {{"freq_hz", p.frequenciesHz[best]}, {"db", db[best]}};
  if (db[best] <= -10.0) { // the stretch around the deepest point that stays at or below -10 dB
    std::size_t lo = best, hi = best;
    while (lo > 0 && db[lo - 1] <= -10.0)
      --lo;
    while (hi + 1 < n && db[hi + 1] <= -10.0)
      ++hi;
    const double low = lo > 0 ? crossing(p.frequenciesHz, db, lo, lo - 1, -10.0) : p.frequenciesHz[lo];
    const double high = hi + 1 < n ? crossing(p.frequenciesHz, db, hi, hi + 1, -10.0) : p.frequenciesHz[hi];
    j["band_10db"] = {{"low_hz", low}, {"high_hz", high}, {"width_hz", high - low}};
  } else {
    j["band_10db"] = nullptr;
  }
  if (withTable) {
    Json table = Json::array();
    for (std::size_t i = 0; i < n; ++i)
      table.push_back({{"f_hz", p.frequenciesHz[i]},
                       {"re", static_cast<double>(p.s11[i].real())},
                       {"im", static_cast<double>(p.s11[i].imag())}});
    j["table"] = table;
  }
  return j;
}

} // namespace

std::string error(const std::string& version, const std::string& message) {
  Json j = header(version);
  j["error"] = message;
  return j.dump(2);
}

std::string settingsList(const std::string& version, const Settings& settings) {
  Json j = header(version);
  Json list = Json::array();
  for (const SettingDef& d : settings.defs()) {
    Json s;
    s["key"] = d.key;
    s["label"] = d.label;
    s["tier"] = tierName(d.tier);
    s["group"] = d.group;
    s["unit"] = d.unit;
    s["help"] = d.help;
    if (d.kind == Kind::Number) {
      s["kind"] = "number";
      s["value"] = settings.number(d.key);
      s["min"] = d.min;
      s["max"] = d.max;
    } else {
      s["kind"] = d.kind == Kind::Choice ? "choice" : "colour";
      s["value"] = settings.text(d.key);
      if (d.kind == Kind::Choice)
        s["choices"] = settings.choices(d.key);
    }
    s["default"] = s["value"]; // a fresh registry: the value is the default of the current profile
    list.push_back(s);
  }
  j["settings"] = list;
  return j.dump(2);
}

std::string devicesList(const std::string& version, const std::vector<DeviceRow>& devices) {
  Json j = header(version);
  Json list = Json::array();
  for (const DeviceRow& d : devices) {
    Json row;
    row["index"] = d.index ? Json(*d.index) : Json(nullptr);
    row["name"] = d.name;
    row["uuid"] = d.uuid;
    row["memory_bytes"] = d.memoryBytes;
    row["vulkan"] = {d.vulkanMajor, d.vulkanMinor};
    row["driver"] = d.driver;
    row["discrete"] = d.discrete;
    row["usable"] = d.usable;
    list.push_back(row);
  }
  j["devices"] = list;
  return j.dump(2);
}

RunFacts runFacts(const Settings& settings, const Prepared& prepared, const Estimate& estimate, bool gpu,
                  const std::string& device, const std::vector<std::string>& extraNotes) {
  RunFacts f;
  const Variant& v = settings.currentVariant();
  f.profile = settings.text("profile");
  f.variant = v.name;
  f.variantStatus = v.status;
  f.bandHz = prepared.plan.bandHz;
  f.sweepHz = prepared.plan.sweepHz;
  f.excitationHz = prepared.plan.excitationHz;
  const fdtd::Grid& g = prepared.model.grid;
  f.cells = {g.cells(fdtd::X), g.cells(fdtd::Y), g.cells(fdtd::Z)};
  f.cellCount = g.cellCount();
  f.smallestCellM = estimate.smallestCell;
  f.timeStepS = estimate.timeStep;
  f.estimatedSteps = estimate.steps;
  f.estimatedMemoryBytes = estimate.memoryBytes;
  f.estimatedSeconds = estimate.seconds;
  f.exceedsMemory = estimate.exceedsMemory;
  f.backend = gpu ? "gpu" : "cpu";
  f.device = gpu ? device : "";
  f.changedSettings = settings.modifiedKeys();
  f.notes = prepared.model.notes;
  f.notes.insert(f.notes.end(), extraNotes.begin(), extraNotes.end());
  return f;
}

std::string dryRun(const std::string& version, const RunFacts& f) {
  Json j = header(version);
  j["mode"] = "dry_run";
  j.update(facts(f));
  return j.dump(2);
}

std::string run(const std::string& version, const RunFacts& f, const RunOutcome& o) {
  Json j = header(version);
  j["mode"] = "run";
  j.update(facts(f));
  j["results_dir"] = o.runDir;
  j["steps"] = o.steps;
  j["time_step_s"] = o.timeStepS;
  j["wall_seconds"] = o.wallSeconds;
  j["mcells_per_second"] = o.mcellsPerSecond;
  if (o.bestHz && o.bestDb)
    j["best_match"] = {{"freq_hz", *o.bestHz}, {"db", *o.bestDb}};
  else
    j["best_match"] = nullptr;
  j["exit_status"] = o.exitStatus;
  if (o.gpuBytesAllocated)
    j["gpu_bytes_allocated"] = o.gpuBytesAllocated;
  else
    j["gpu_bytes_allocated"] = nullptr;
  if (o.resumedFromStep)
    j["resumed_from_step"] = o.resumedFromStep;
  else
    j["resumed_from_step"] = nullptr;
  if (o.fieldsRequested)
    j["fields"] = {{"kept", o.fieldsKept}, {"bytes", o.fieldsBytes}, {"note", o.fieldsNote}};
  else
    j["fields"] = nullptr;
  return j.dump(2);
}

std::string paused(const std::string& version, const std::string& runDir, std::size_t checkpointStep,
                   const std::string& checkpointDir) {
  Json j = header(version);
  j["mode"] = "run";
  j["paused"] = true;
  j["checkpoint_step"] = checkpointStep;
  j["checkpoint_dir"] = checkpointDir;
  j["results_dir"] = runDir;
  j["exit_status"] = 75;
  return j.dump(2);
}

std::string runInfo(const std::string& version, const fs::path& dir, bool withTable) {
  if (!fs::is_directory(dir))
    throw std::runtime_error("not a folder: " + dir.string());
  const fs::path summary = dir / "summary.txt", s1p = dir / "s11.s1p", settingsFile = dir / "settings.json";
  const bool hasSummary = fs::is_regular_file(summary), hasS11 = fs::is_regular_file(s1p);
  if (!hasSummary && !hasS11)
    throw std::runtime_error(dir.string() + " has neither summary.txt nor s11.s1p: not a saved run");
  Json j = header(version);
  j["mode"] = "run_info";
  j["run_dir"] = fs::absolute(dir).string();
  Json changed = Json::array(), notes = Json::array();
  j["board"] = nullptr;
  j["profile"] = nullptr;
  j["variant"] = nullptr;
  j["variant_status"] = nullptr;
  j["band_hz"] = nullptr;
  j["sweep_hz"] = nullptr;
  j["grid"] = nullptr;
  j["steps"] = nullptr;
  j["time_step_s"] = nullptr;
  j["backend"] = nullptr;
  if (hasSummary) {
    std::istringstream in(slurp(summary));
    std::string line, rest;
    while (std::getline(in, line)) {
      if (after(line, "board: ", rest)) {
        j["board"] = rest;
      } else if (after(line, "profile: ", rest)) {
        const auto slash = rest.find(" / ");
        const auto open = rest.rfind(" [");
        j["profile"] = rest.substr(0, slash);
        if (slash != std::string::npos) {
          j["variant"] = rest.substr(
              slash + 3, open != std::string::npos && open > slash ? open - slash - 3 : std::string::npos);
          if (open != std::string::npos && rest.back() == ']')
            j["variant_status"] = rest.substr(open + 2, rest.size() - open - 3);
        }
      } else if (after(line, "band of interest: ", rest) || after(line, "S11 reported: ", rest)) {
        double a = 0, b = 0;
        if (std::sscanf(rest.c_str(), "%lf to %lf GHz", &a, &b) == 2)
          j[line.compare(0, 4, "band") == 0 ? "band_hz" : "sweep_hz"] = {a * 1e9, b * 1e9};
      } else if (after(line, "grid: ", rest)) {
        unsigned long nx = 0, ny = 0, nz = 0;
        double cell = 0;
        if (std::sscanf(rest.c_str(), "%lu x %lu x %lu cells, smallest cell %lf mm", &nx, &ny, &nz, &cell) ==
            4)
          j["grid"] = {
              {"cells", {nx, ny, nz}}, {"cell_count", nx * ny * nz}, {"smallest_cell_m", cell * 1e-3}};
      } else if (after(line, "steps: ", rest)) {
        unsigned long steps = 0;
        double ps = 0;
        char backend[32] = {};
        if (std::sscanf(rest.c_str(), "%lu, time step %lf ps, backend %31s", &steps, &ps, backend) == 3 ||
            std::sscanf(rest.c_str(), "%lu, dt %lf ps, backend %31s", &steps, &ps, backend) == 3) {
          j["steps"] = steps;
          j["time_step_s"] = ps * 1e-12;
          std::string b = backend;
          std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return std::tolower(c); });
          j["backend"] = b;
        }
      } else if (after(line, "changed from default: ", rest)) {
        changed.push_back(rest);
      } else if (after(line, "note: ", rest)) {
        notes.push_back(rest);
      }
    }
  }
  j["changed_settings"] = changed;
  j["notes"] = notes;
  j["settings"] = nullptr;
  if (fs::is_regular_file(settingsFile)) {
    const Json s = Json::parse(slurp(settingsFile), nullptr, false);
    if (!s.is_discarded())
      j["settings"] = s; // the user's deviations as the run saved them
  }
  j["s11"] = nullptr;
  if (hasS11)
    j["s11"] = s11Info(s1p, withTable);
  j["files"] = {{"summary", hasSummary},
                {"settings", fs::is_regular_file(settingsFile)},
                {"board", fs::is_regular_file(dir / "board.json")},
                {"s11", hasS11}};
  return j.dump(2);
}

} // namespace app::clijson
