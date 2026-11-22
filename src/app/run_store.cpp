#include "app/run_store.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <system_error>

#include "app/run_archive.hpp"
#include "fdtd/sparam.hpp"

namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace app {

namespace {
Json farJson(const FarResult& r) {
  const fdtd::FarField& f = r.field;
  const fdtd::AntennaFigures& a = r.figures;
  return {{"frequency_hz", r.frequencyHz},
          {"s11_db", r.s11Db},
          {"n_theta", f.nTheta},
          {"n_phi", f.nPhi},
          {"theta", f.theta},
          {"phi", f.phi},
          {"u", f.u},
          {"power", f.power},
          {"peak", f.peak},
          {"peak_theta", f.peakTheta},
          {"peak_phi", f.peakPhi},
          {"figures",
           {a.radiatedPower, a.availablePower, a.acceptedPower, a.efficiency, a.directivity, a.gain,
            a.realisedGain, a.peakThetaDeg, a.peakPhiDeg}}};
}

FarResult farFrom(const Json& j) {
  FarResult r;
  r.frequencyHz = j.at("frequency_hz");
  r.s11Db = j.at("s11_db");
  fdtd::FarField& f = r.field;
  f.nTheta = j.at("n_theta");
  f.nPhi = j.at("n_phi");
  f.theta = j.at("theta").get<std::vector<double>>();
  f.phi = j.at("phi").get<std::vector<double>>();
  f.u = j.at("u").get<std::vector<double>>();
  f.power = j.at("power");
  f.peak = j.at("peak");
  f.peakTheta = j.at("peak_theta");
  f.peakPhi = j.at("peak_phi");
  const auto v = j.at("figures").get<std::vector<double>>();
  if (v.size() != 9)
    throw std::runtime_error("farfield.json: figures");
  fdtd::AntennaFigures& a = r.figures;
  a = {v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]};
  return r;
}

std::string slurp(const fs::path& p) {
  std::ifstream in(p);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
} // namespace

std::uint64_t folderBytes(const fs::path& dir) {
  std::uint64_t n = 0;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    if (it->is_regular_file(ec))
      n += it->file_size(ec);
  return n;
}

void saveFields(const fs::path& runDir, const std::vector<NamedPhasors>& phasors,
                const std::vector<FarResult>& far) {
  const fs::path dir = runDir / "fields";
  fs::create_directories(dir);
  for (const auto& [name, acc] : phasors)
    if (acc)
      savePhasors(dir / (name + ".f16"), *acc);
  if (!far.empty()) {
    Json arr = Json::array();
    for (const FarResult& r : far)
      arr.push_back(farJson(r));
    std::ofstream(dir / "farfield.json") << arr.dump();
  }
}

std::unique_ptr<viz::PhasorAccumulator> loadField(const fs::path& runDir, const std::string& name) {
  const fs::path file = runDir / "fields" / (name + ".f16");
  if (!fs::exists(file))
    return nullptr;
  return loadPhasors(file);
}

std::vector<FarResult> loadFar(const fs::path& runDir) {
  const fs::path file = runDir / "fields" / "farfield.json";
  std::vector<FarResult> out;
  if (!fs::exists(file))
    return out;
  for (const Json& j : Json::parse(slurp(file)))
    out.push_back(farFrom(j));
  return out;
}

void writeManifest(const fs::path& runDir, const RunManifest& m) {
  Json j{{"schema", 1},         {"id", m.id},           {"time", m.time},
         {"board", m.board},    {"profile", m.profile}, {"backend", m.backend},
         {"best_db", m.bestDb}, {"best_hz", m.bestHz},  {"wall_seconds", m.wallSeconds},
         {"steps", m.steps},    {"fields", m.fields}};
  std::ofstream(runDir / "run.json") << j.dump(2) << "\n";
}

static RunManifest readManifest(const fs::path& dir) {
  RunManifest m;
  m.dir = dir;
  m.id = dir.filename().string();
  m.time = m.id.size() > 4 ? m.id.substr(4) : m.id;
  try {
    const Json j = Json::parse(slurp(dir / "run.json"));
    m.board = j.value("board", "");
    m.profile = j.value("profile", "");
    m.backend = j.value("backend", "");
    m.bestDb = j.value("best_db", 0.0);
    m.bestHz = j.value("best_hz", 0.0);
    m.wallSeconds = j.value("wall_seconds", 0.0);
    m.steps = j.value("steps", std::size_t{0});
    m.fields = j.value("fields", false) && fs::exists(dir / "fields");
  } catch (const std::exception&) { // an older run, or a damaged manifest: what the S11 file says
    try {
      std::ifstream s(dir / "s11.s1p");
      if (s) {
        const fdtd::SParam1 p = fdtd::readTouchstone(s);
        for (std::size_t i = 0; i < p.s11.size(); ++i) {
          const double db = 20 * std::log10(std::max(std::abs(p.s11[i]), 1e-6));
          if (i == 0 || db < m.bestDb) {
            m.bestDb = db;
            m.bestHz = p.frequenciesHz[i];
          }
        }
      }
    } catch (const std::exception&) {
    }
    m.fields = fs::exists(dir / "fields");
  }
  return m;
}

std::vector<RunManifest> scanRuns(const fs::path& root) {
  std::vector<RunManifest> runs;
  std::error_code ec;
  for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
    if (it->is_directory(ec) && it->path().filename().string().rfind("run-", 0) == 0) {
      RunManifest m = readManifest(it->path());
      m.bytes = folderBytes(it->path());
      runs.push_back(std::move(m));
    }
  std::sort(runs.begin(), runs.end(), [](const RunManifest& a, const RunManifest& b) { return a.id > b.id; });
  Json arr = Json::array();
  for (const RunManifest& m : runs)
    arr.push_back({{"id", m.id},
                   {"board", m.board},
                   {"best_db", m.bestDb},
                   {"best_hz", m.bestHz},
                   {"bytes", m.bytes},
                   {"fields", m.fields}});
  if (!runs.empty())
    std::ofstream(root / "index.json") << arr.dump(2) << "\n";
  return runs;
}

std::uint64_t pruneFields(const fs::path& root, std::size_t keep) {
  std::uint64_t freed = 0;
  const auto runs = scanRuns(root);
  for (std::size_t i = keep; i < runs.size(); ++i) {
    const fs::path f = runs[i].dir / "fields";
    std::error_code ec;
    if (!fs::exists(f, ec))
      continue;
    freed += folderBytes(f);
    fs::remove_all(f, ec);
    RunManifest m = runs[i];
    m.fields = false;
    if (fs::exists(runs[i].dir / "run.json"))
      writeManifest(runs[i].dir, m);
  }
  if (freed)
    scanRuns(root); // refresh the index
  return freed;
}

} // namespace app
