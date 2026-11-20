#include "app/results.hpp"

#include <cstdlib>
#include <fstream>

namespace fs = std::filesystem;

namespace app {

void writeResults(const fs::path& runDir, const fs::path& snapshotFile, const board::Snapshot& snapshot,
                  const Settings& settings, const Prepared& prepared, const Estimate& estimate,
                  const sim::RunResult& result, const std::string& backendName) {
  const Variant& variant = settings.currentVariant();
  {
    std::ofstream out(runDir / "s11.s1p");
    fdtd::writeTouchstone(out, result.s,
                          "kicad-em-fdtd S11, board " + snapshot.boardName + ", profile " +
                              settings.text("profile"));
  }
  std::ofstream(runDir / "settings.json") << settings.toJson();
  fs::copy_file(snapshotFile, runDir / "board.json", fs::copy_options::overwrite_existing);

  std::ofstream sum(runDir / "summary.txt");
  const fdtd::Grid& g = prepared.model.grid;
  sum << "kicad-em-fdtd run summary\n"
      << "board: " << snapshot.boardName << "\n"
      << "profile: " << settings.text("profile") << " / " << variant.name << " [" << variant.status << "]\n"
      << "source of the band: " << variant.source << "\n"
      << "band of interest: " << prepared.plan.bandHz[0] * 1e-9 << " to " << prepared.plan.bandHz[1] * 1e-9
      << " GHz\n"
      << "S11 reported: " << prepared.plan.sweepHz[0] * 1e-9 << " to " << prepared.plan.sweepHz[1] * 1e-9
      << " GHz\n"
      << "grid: " << g.cells(fdtd::X) << " x " << g.cells(fdtd::Y) << " x " << g.cells(fdtd::Z)
      << " cells, smallest cell " << estimate.smallestCell * 1e3 << " mm\n"
      << "steps: " << result.steps << ", time step " << result.dt * 1e12 << " ps, backend " << backendName
      << "\n";
  for (const std::string& key : settings.modifiedKeys())
    sum << "changed from default: " << key << "\n";
  for (const std::string& n : prepared.model.notes)
    sum << "note: " << n << "\n";
}

bool openFolder(const fs::path& dir) {
#if defined(__APPLE__)
  const std::string cmd = "open \"" + dir.string() + "\"";
#elif defined(_WIN32)
  const std::string cmd = "explorer \"" + dir.string() + "\"";
#else
  const std::string cmd = "xdg-open \"" + dir.string() + "\"";
#endif
  return std::system(cmd.c_str()) == 0;
}

} // namespace app
