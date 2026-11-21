// Parameter sweeps: run the same board once per value of one setting, one results folder per point, and keep
// the S11 curves so they can be drawn over each other. A sweep is a driver around the ordinary run (prepare,
// runOnePort, writeResults): nothing here changes how a single run works. Docs/PLAN.md, Next steps 11g.
#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "app/paths.hpp"
#include "app/prepare.hpp"
#include "app/settings.hpp"
#include "board/snapshot.hpp"
#include "board/voxelize.hpp"
#include "fdtd/sparam.hpp"
#include "sim/simulation.hpp"

namespace app {

struct SweepSpec {
  std::string key;                 // a settings registry key (see `emfdtd --list-settings`)
  std::vector<std::string> values; // each as the user typed it
};

// "a,b,c" is a list; "start:stop:count" is `count` evenly spaced numbers from start to stop (count >= 2).
// Throws SettingsError for an empty list, a bad range or more than `maxPoints` values.
std::vector<std::string> parseSweepValues(const std::string& text, std::size_t maxPoints = 25);

struct SweepPoint {
  std::string value, label;      // label: "key = value"
  fdtd::SParam1 s;               // empty if the point failed
  double bestDb = 0, bestHz = 0; // the best match in the sweep
  std::filesystem::path dir;     // the point's results folder
  std::string error;             // set if this point could not run
};

struct SweepEnv {
  const board::Snapshot& snapshot;
  const std::string snapshotPath;
  const Catalog& catalog;
  const ProjectPaths& paths;
  const board::Selection& selection;
  sim::EngineFactory factory;
  std::string backendName;
  bool gpu = false;
  double deviceBytes = 0;
  // called before a point starts (point == nullptr) and after it ends (its result)
  std::function<void(std::size_t index, std::size_t count, const SweepPoint* done)> progress;
  std::function<bool()> shouldStop;
  // called with each point's prepared model and options before it runs, to attach viewer feeds: optional
  std::function<void(Prepared&)> beforeRun;
  std::vector<std::string>
      notes;         // added to every point's run notes (for example the standing geometry overrides)
  std::string stamp; // timestamp for the sweep's folder
};

// Runs every point, in order, each from `base` with the one key changed. A point that fails (the value is out
// of range, the model does not fit) is recorded with its error and the sweep goes on. Results go to
// <results>/sweep-<stamp>/<key>-<value>/ and a combined sweep.json.
std::vector<SweepPoint> runSweep(const SweepEnv& env, const Settings& base, const SweepSpec& spec);

} // namespace app
