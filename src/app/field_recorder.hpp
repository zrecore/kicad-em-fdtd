// The field data a run records for the 3D and slice views, the surface current, the power flow and the far
// field: the display layout, the phasor accumulators that fill during the solve, the far-field transform
// after it, and the saved form (run_store). Shared by the window (apps/emfdtd/ui.cpp) and by the headless
// command line with --keep-fields (apps/emfdtd/main.cpp), so both write the same files. docs/ux.md, "Saved
// runs".
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "app/phasor_feed.hpp"
#include "app/prepare.hpp"
#include "app/run_store.hpp"
#include "board/snapshot.hpp"
#include "fdtd/ntff.hpp"
#include "fdtd/surface_current.hpp"
#include "sim/simulation.hpp"
#include "viz/phasor.hpp"
#include "viz/slice.hpp"
#include "viz/volume.hpp"

namespace app {

struct FieldRecorder {
  // The display layout (see layout()); the window reads these to draw.
  viz::Plane plane{};
  viz::DisplayVolume dv;
  fdtd::Axis fieldAxis = fdtd::Z;
  std::size_t kTop = 0, kBottom = 0; // the outer copper plane nodes
  std::vector<double> frequencies;   // the animation frequencies across the sweep
  fdtd::SurfaceCurrentList currentList;
  fdtd::NtffPlan ntffPlan;
  std::vector<double> ntffFreqs;
  std::string ntffNote; // why there is no far field, or empty
  bool hasNtff = false;

  // The accumulators; null until allocate() (and for a quantity the board does not have).
  std::unique_ptr<viz::PhasorAccumulator> phasors, volPhasors, currentH, poyntingH, ntffAcc;
  std::vector<FarResult> farResults;

  void reset();
  // Display plane, volume box, current list and far-field box for the prepared model. No big allocation.
  void layout(const board::Snapshot& snap, const Prepared& prepared);
  // Bytes saved/loaded files take for the current layout (the .f16 files and the far-field table).
  std::uint64_t savedBytes() const;
  // Creates the accumulators for the layout.
  void allocate();
  // Adds the feeds to opt.onSample/onFinish (earlier callbacks still run first). `refreshed` runs whenever
  // a viewer copy was refreshed. The feeds are owned here and must outlive the run.
  void attach(sim::RunOptions& opt, const fdtd::Grid& g, std::mutex& mu,
              const std::function<void()>& refreshed);
  // After the solve: the far-field figures from the box phasors (needs the S11 and the source). The caller
  // stores them in `farResults` (under its own lock if a viewer reads them).
  std::vector<FarResult> computeFar(const sim::RunResult& r, double impedanceOhm, int nTheta = 36,
                                    int nPhi = 72) const;
  // fields/ in the run folder; throws on a write error.
  void save(const std::filesystem::path& runDir) const;

private:
  std::vector<std::unique_ptr<PhasorFeed>> feeds_;
  std::unique_ptr<PhasorFeed> farFeed_;
};

// The saved-fields size a dry run can report: layout only, no accumulators.
std::uint64_t estimateFieldBytes(const board::Snapshot& snap, const Prepared& prepared);

} // namespace app
