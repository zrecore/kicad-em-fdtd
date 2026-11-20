#include "app/paths.hpp"

#include <chrono>
#include <cstdlib>
#include <ctime>

#include "app/settings.hpp"

namespace fs = std::filesystem;

namespace app {

fs::path userConfigDir() {
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
    return fs::path(xdg) / "kicad-em-fdtd";
#if defined(_WIN32)
  if (const char* appdata = std::getenv("APPDATA"))
    return fs::path(appdata) / "kicad-em-fdtd";
#elif defined(__APPLE__)
  if (const char* home = std::getenv("HOME"))
    return fs::path(home) / "Library" / "Application Support" / "kicad-em-fdtd";
#endif
  if (const char* home = std::getenv("HOME"))
    return fs::path(home) / ".config" / "kicad-em-fdtd";
  return fs::current_path() / "kicad-em-fdtd-config";
}

ProjectPaths::ProjectPaths(const fs::path& projectDir) {
  if (!fs::is_directory(projectDir))
    throw SettingsError("project folder does not exist: " + projectDir.string());
  for (const auto& part : projectDir)
    if (part.string().size() >= 8 && part.string().compare(part.string().size() - 8, 8, "-backups") == 0)
      throw SettingsError("refusing to write inside a KiCAD backups folder: " + projectDir.string());
  root = projectDir / "em-fdtd";
  settingsFile = root / "settings.json";
  selectionFile = root / "selection.json";
  resultsRoot = root / "results";
}

fs::path ProjectPaths::newRunDir(const std::string& stamp) const {
  fs::create_directories(resultsRoot);
  fs::path dir = resultsRoot / ("run-" + stamp);
  for (int n = 2; fs::exists(dir); ++n)
    dir = resultsRoot / ("run-" + stamp + "-" + std::to_string(n));
  fs::create_directory(dir);
  return dir;
}

std::string timestamp() {
  const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", std::localtime(&t));
  return buf;
}

} // namespace app
