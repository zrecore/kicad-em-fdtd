// The model selection in the sidecar file next to the board (never in the KiCAD design): the nets the user
// picked, each two-terminal part's treatment and each part's body treatment. The net walk is not stored: it
// is recomputed from the picked nets and the snapshot, so it follows the board when it changes.
#pragma once
#include <filesystem>
#include <set>
#include <string>

#include "board/voxelize.hpp"

namespace app {

struct StoredSelection {
  std::set<std::string> pickedNets;
  std::set<std::string> extraGroundNets;
  std::map<std::string, board::PartTreatment> parts;
  std::map<std::string, board::BodyTreatment> bodies;
  board::BodyTreatment defaultBody;
};

void saveSelection(const std::filesystem::path& file, const StoredSelection& s);
// A missing file gives an empty selection; a file that cannot be read throws SettingsError (never silently
// replaced).
StoredSelection loadSelection(const std::filesystem::path& file);

} // namespace app
