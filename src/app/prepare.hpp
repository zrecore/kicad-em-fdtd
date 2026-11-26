// From a snapshot and the settings to a ready-to-run model, plus the estimate shown before the run. Shared by
// the command line and the preview screen, so both always agree.
#pragma once
#include "app/settings.hpp"
#include "board/voxelize.hpp"
#include "sim/simulation.hpp"

namespace app {

struct BoardFacts {
  double maxEpsilonR = 1;
  double minDielectricThickness = 0; // metres, 0 if the stackup has no dielectric
};
BoardFacts boardFacts(const board::Snapshot& s);

// Applies the PCB settings (a fab profile and the per-layer edits) to the board's dielectric layers, top to
// bottom, and returns one note per change saying where each value came from. A value left at 0 keeps the
// board's own. A fab profile built for another number of copper layers is not applied (and says so); values
// the user typed always are.
std::vector<std::string> applyPcbSettings(const Settings& settings, board::Snapshot& snapshot);
// Only the copper thickness part of it (what the user typed, else the board's, else the profile's); returns
// the note, empty when no copper layer has a thickness. The feed-line estimate needs just this.
std::string applyCopperThickness(const Settings& settings, board::Snapshot& snapshot);

struct Prepared {
  RunPlan plan;
  board::Model model;
  sim::RunOptions options;
};

// extraRefine: explicit refinement boxes (board coordinates) added to the automatic ones around ports.
Prepared prepare(const board::Snapshot& snapshot, const Settings& settings, const board::Selection& selection,
                 const std::vector<board::RefineBox>& extraRefine = {});

struct Estimate {
  std::size_t cells = 0, steps = 0;
  double memoryBytes = 0, seconds = 0;
  double smallestCell = 0, timeStep = 0;
  bool exceedsMemory = false;
};
// Rough, and labelled so in the UI: steps assume the pulse plus about twice its length of ring-down, speed is
// the throughput measured on the M1 Pro (docs/PLAN.md, Measured so far). `deviceBytes` is the memory budget.
Estimate estimate(const Prepared& p, bool gpu, double deviceBytes);

} // namespace app
