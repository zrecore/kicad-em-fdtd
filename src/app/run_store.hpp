// A project's saved runs (docs/PLAN.md, Next steps 16): what a finished run keeps so it can be opened again
// without solving, the manifest of each run, the list of runs, and pruning the bulky field data.
// Layout inside <project>/em-fdtd/results/run-<time>/: run.json (manifest), s11.s1p, settings.json,
// board.json, summary.txt, and fields/ (one .f16 phasor file per quantity, farfield.json). results/index.json
// is a cache of the runs, rebuilt by scanning the folders, so deleting or moving a run folder never corrupts
// it.
#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "fdtd/ntff.hpp"
#include "viz/phasor.hpp"

namespace app {

struct FarResult {
  double frequencyHz = 0;
  fdtd::FarField field;
  fdtd::AntennaFigures figures;
  double s11Db = 0;
};

// What run.json records. `fields` says whether fields/ exists.
struct RunManifest {
  std::string id, time, board, profile, backend;
  double bestDb = 0, bestHz = 0, wallSeconds = 0;
  std::size_t steps = 0;
  bool fields = false;
  std::uint64_t bytes = 0; // size of the folder, filled by scanRuns
  std::filesystem::path dir;
};

using NamedPhasors = std::pair<std::string, const viz::PhasorAccumulator*>; // a null pointer is skipped

// Writes fields/<name>.f16 for each, and fields/farfield.json when `far` is not empty.
void saveFields(const std::filesystem::path& runDir, const std::vector<NamedPhasors>& phasors,
                const std::vector<FarResult>& far);
// Null when the run kept no such quantity; throws std::runtime_error for a damaged file.
std::unique_ptr<viz::PhasorAccumulator> loadField(const std::filesystem::path& runDir,
                                                  const std::string& name);
std::vector<FarResult> loadFar(const std::filesystem::path& runDir);

void writeManifest(const std::filesystem::path& runDir, const RunManifest& m);
// The runs in `resultsRoot` newest first (run-* folders; one without run.json is listed with what its S11
// file says). Also rewrites index.json. Never throws for a damaged folder: it is listed as it can be read.
std::vector<RunManifest> scanRuns(const std::filesystem::path& resultsRoot);
// Deletes fields/ of every run except the `keep` newest and marks them in run.json. Returns the bytes freed.
std::uint64_t pruneFields(const std::filesystem::path& resultsRoot, std::size_t keep);
std::uint64_t folderBytes(const std::filesystem::path& dir);

} // namespace app
