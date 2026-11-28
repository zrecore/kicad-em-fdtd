// Machine-readable output of the `emfdtd` command line (`--json`; docs/cli-json.md). Every function returns
// one JSON document as text, with "schema": 1, "tool": "emfdtd" and the program version, so assistants and
// scripts do not parse the human text. Additive changes keep the schema number; a breaking change bumps it.
#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "app/prepare.hpp"
#include "app/settings.hpp"

namespace app::clijson {

inline constexpr int kSchema = 1;

// {"error": "..."} (with the header), for any failure in --json mode.
std::string error(const std::string& version, const std::string& message);

// `emfdtd --list-settings --json`: every setting with its typed value and default.
std::string settingsList(const std::string& version, const Settings& settings);

// One Vulkan device, as `--list-devices` prints it. `index` is the number `--device` accepts (usable devices
// only, enumeration order); empty for a device below Vulkan 1.2.
struct DeviceRow {
  std::optional<int> index;
  std::string name, uuid, driver;
  std::uint64_t memoryBytes = 0;
  unsigned vulkanMajor = 0, vulkanMinor = 0;
  bool discrete = false, usable = false;
};
std::string devicesList(const std::string& version, const std::vector<DeviceRow>& devices);

// What a dry run prints and a run adds to: the plan, the estimate and the notes.
struct RunFacts {
  std::string profile, variant, variantStatus;
  std::array<double, 2> bandHz{}, sweepHz{}, excitationHz{};
  std::array<std::size_t, 3> cells{};
  std::size_t cellCount = 0, estimatedSteps = 0;
  double smallestCellM = 0, timeStepS = 0, estimatedMemoryBytes = 0, estimatedSeconds = 0;
  bool exceedsMemory = false;
  std::uint64_t fieldBytes = 0; // size of the saved field data a --keep-fields run would write; 0: not known
  std::string backend, device;  // backend: "gpu" or "cpu"; device: the GPU's name, empty on the CPU
  std::vector<std::string> changedSettings, notes;
};
RunFacts runFacts(const Settings& settings, const Prepared& prepared, const Estimate& estimate, bool gpu,
                  const std::string& device, const std::vector<std::string>& extraNotes);

// `emfdtd SNAPSHOT --dry-run --json`.
std::string dryRun(const std::string& version, const RunFacts& facts);

// What a finished run adds.
struct RunOutcome {
  std::string runDir;
  std::size_t steps = 0;
  double timeStepS = 0, wallSeconds = 0, mcellsPerSecond = 0;
  std::optional<double> bestHz, bestDb; // the deepest S11 point; empty if the run produced none
  int exitStatus = 0;
  // peak kernel-bound buffer bytes (0 on the CPU: the document then has null)
  std::uint64_t gpuBytesAllocated = 0;
  std::size_t resumedFromStep = 0; // 0: not a resumed run (the document then has "resumed_from_step": null)
  // --keep-fields: asked for, and whether the field data was kept (with its size, or the reason it was not)
  bool fieldsRequested = false, fieldsKept = false;
  std::uint64_t fieldsBytes = 0;
  std::string fieldsNote;
};
// `emfdtd SNAPSHOT --json` after a headless run.
std::string run(const std::string& version, const RunFacts& facts, const RunOutcome& outcome);
// A run that wrote a checkpoint on request and stopped (exit status 75): docs/checkpoint.md.
std::string paused(const std::string& version, const std::string& runDir, std::size_t checkpointStep,
                   const std::string& checkpointDir);

// `emfdtd --run-info RUNDIR --json`: a saved run folder, no solve. `withTable` adds every S11 point. Throws
// std::runtime_error (the message goes into {"error": ...}) if the folder has neither a summary nor S11.
std::string runInfo(const std::string& version, const std::filesystem::path& runDir, bool withTable);

} // namespace app::clijson
