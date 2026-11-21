#include "app/sweep.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "app/results.hpp"
#include "board/geometry_override.hpp"

namespace fs = std::filesystem;

namespace app {

std::vector<std::string> parseSweepValues(const std::string& text, std::size_t maxPoints) {
  std::vector<std::string> out;
  if (text.find(':') != std::string::npos) {
    std::stringstream ss(text);
    std::string a, b, c;
    std::getline(ss, a, ':');
    std::getline(ss, b, ':');
    std::getline(ss, c, ':');
    char *ea = nullptr, *eb = nullptr, *ec = nullptr;
    const double start = std::strtod(a.c_str(), &ea), stop = std::strtod(b.c_str(), &eb);
    const long count = std::strtol(c.c_str(), &ec, 10);
    if (a.empty() || b.empty() || c.empty() || *ea || *eb || *ec || count < 2)
      throw SettingsError("a range is start:stop:count with count 2 or more, got '" + text + "'");
    if (static_cast<std::size_t>(count) > maxPoints)
      throw SettingsError("at most " + std::to_string(maxPoints) + " points per sweep, got " +
                          std::to_string(count));
    for (long i = 0; i < count; ++i) {
      char buf[40];
      std::snprintf(buf, sizeof buf, "%.10g",
                    start + (stop - start) * static_cast<double>(i) / static_cast<double>(count - 1));
      out.push_back(buf);
    }
    return out;
  }
  std::stringstream ss(text);
  for (std::string v; std::getline(ss, v, ',');) {
    v.erase(0, v.find_first_not_of(' '));
    v.erase(v.find_last_not_of(' ') + 1);
    if (!v.empty())
      out.push_back(v);
  }
  if (out.empty())
    throw SettingsError("a sweep needs at least one value");
  if (out.size() > maxPoints)
    throw SettingsError("at most " + std::to_string(maxPoints) + " points per sweep, got " +
                        std::to_string(out.size()));
  return out;
}

namespace {

std::string safeName(const std::string& s) {
  std::string out;
  for (char c : s)
    out += (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_') ? c : '_';
  return out;
}

} // namespace

std::vector<SweepPoint> runSweep(const SweepEnv& env, const Settings& base, const SweepSpec& spec) {
  // A key "geom.NET.x" sweeps a geometry override (the value is the stretch in millimetres), not a setting.
  const bool geom = spec.key.rfind("geom.", 0) == 0;
  const SettingDef* def = geom ? nullptr : &base.def(spec.key); // throws if the key is unknown
  const fs::path root = env.paths.resultsRoot / ("sweep-" + env.stamp);
  fs::create_directories(root);
  std::vector<SweepPoint> points;
  for (std::size_t n = 0; n < spec.values.size(); ++n) {
    if (env.shouldStop && env.shouldStop())
      break;
    SweepPoint pt;
    pt.value = spec.values[n];
    pt.label = spec.key + " = " + pt.value;
    pt.dir = root / (safeName(spec.key) + "-" + safeName(pt.value));
    if (env.progress)
      env.progress(n, spec.values.size(), nullptr);
    try {
      Settings s = base;
      board::Snapshot changed;
      std::string geomNote;
      if (geom) {
        changed = env.snapshot;
        const auto o = board::parseGeometryOverride(spec.key.substr(5) + "=" + pt.value);
        geomNote = board::applyGeometryOverrides(changed, {o}).front();
      } else if (def->kind == Kind::Number) {
        char* end = nullptr;
        const double v = std::strtod(pt.value.c_str(), &end);
        if (end == pt.value.c_str() || *end)
          throw SettingsError("'" + pt.value + "' is not a number");
        s.set(spec.key, v);
      } else {
        s.set(spec.key, pt.value);
      }
      Prepared prepared = prepare(geom ? changed : env.snapshot, s, env.selection);
      prepared.model.notes.insert(prepared.model.notes.end(), env.notes.begin(), env.notes.end());
      if (geom)
        prepared.model.notes.push_back(geomNote);
      const Estimate est = estimate(prepared, env.gpu, env.deviceBytes);
      if (est.exceedsMemory)
        throw SettingsError("the model would not fit in device memory");
      if (env.beforeRun)
        env.beforeRun(prepared);
      prepared.options.shouldStop = env.shouldStop;
      const sim::RunResult r = sim::runOnePort(prepared.model, prepared.options, env.factory);
      fs::create_directories(pt.dir);
      writeResults(pt.dir, env.snapshotPath, geom ? changed : env.snapshot, s, prepared, est, r,
                   env.backendName);
      pt.s = r.s;
      std::size_t best = 0;
      for (std::size_t i = 1; i < r.s.s11.size(); ++i)
        if (std::abs(r.s.s11[i]) < std::abs(r.s.s11[best]))
          best = i;
      if (!r.s.s11.empty()) {
        pt.bestDb = 20 * std::log10(std::max(std::abs(r.s.s11[best]), 1e-6));
        pt.bestHz = r.s.frequenciesHz[best];
      }
    } catch (const std::exception& e) {
      pt.error = e.what();
    }
    if (env.progress)
      env.progress(n, spec.values.size(), &pt);
    points.push_back(std::move(pt));
  }
  // the combined file: one object per point with the S11 in real and imaginary parts
  std::ofstream out(root / "sweep.json");
  out << "{\n  \"key\": \"" << spec.key << "\",\n  \"points\": [\n";
  for (std::size_t n = 0; n < points.size(); ++n) {
    const SweepPoint& p = points[n];
    out << "    {\"value\": \"" << p.value << "\", \"dir\": \"" << p.dir.filename().string() << "\"";
    if (!p.error.empty())
      out << ", \"error\": \"" << p.error << "\"";
    else {
      out << ", \"best_db\": " << p.bestDb << ", \"best_ghz\": " << p.bestHz * 1e-9 << ", \"f_hz\": [";
      for (std::size_t i = 0; i < p.s.frequenciesHz.size(); ++i)
        out << (i ? ", " : "") << p.s.frequenciesHz[i];
      out << "], \"s11_re\": [";
      for (std::size_t i = 0; i < p.s.s11.size(); ++i)
        out << (i ? ", " : "") << p.s.s11[i].real();
      out << "], \"s11_im\": [";
      for (std::size_t i = 0; i < p.s.s11.size(); ++i)
        out << (i ? ", " : "") << p.s.s11[i].imag();
      out << "]";
    }
    out << "}" << (n + 1 < points.size() ? "," : "") << "\n";
  }
  out << "  ]\n}\n";
  return points;
}

} // namespace app
