// What a finished run leaves in its folder (<project>/em-fdtd/results/run-<time>/): the S11 as Touchstone,
// the settings actually used, a copy of the board snapshot, and a plain-text summary of every assumption.
#pragma once
#include <filesystem>
#include <string>

#include "app/prepare.hpp"
#include "app/settings.hpp"
#include "board/snapshot.hpp"

namespace app {

void writeResults(const std::filesystem::path& runDir, const std::filesystem::path& snapshotFile,
                  const board::Snapshot& snapshot, const Settings& settings, const Prepared& prepared,
                  const Estimate& estimate, const sim::RunResult& result, const std::string& backendName);

// Opens a folder in the system file manager (best effort; returns false if the command failed).
bool openFolder(const std::filesystem::path& dir);

} // namespace app
