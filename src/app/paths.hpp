// Where files live. Per project (the folder holding the .kicad_pro): <project>/em-fdtd/ with settings.json
// (this board's saved settings), selection.json (component choices), antenna_rules.json (the project's own
// antenna rules, optional) and results/run-<time>/ for each run. Per user: saved radio profiles in the user's
// configuration directory. Nothing is written inside the KiCAD design files, and never under *-backups
// folders.
#pragma once
#include <filesystem>
#include <string>

namespace app {

std::filesystem::path userConfigDir(); // created on demand by the caller

struct ProjectPaths {
  explicit ProjectPaths(
      const std::filesystem::path& projectDir); // throws SettingsError if it is not a directory
  std::filesystem::path root, settingsFile, selectionFile, resultsRoot, antennaRulesFile;
  // Creates results/run-<stamp>, or run-<stamp>-2, -3, ... if that name exists, and returns it.
  std::filesystem::path newRunDir(const std::string& stamp) const;
};

std::string timestamp(); // local time, "YYYYMMDD-HHMMSS"

} // namespace app
