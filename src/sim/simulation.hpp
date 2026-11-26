// One-port S11 run: voxelized model -> solver -> excited at port 0 -> S11 over a band.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "board/voxelize.hpp"
#include "fdtd/sparam.hpp"
#include "sim/engine.hpp"

namespace sim {

struct RunResult;

// The state of a run at a quiet point between advances, opaque outside this module: what the driver itself
// holds (step count, decay window, port histories) and what the engine holds. See docs/checkpoint.md.
struct Checkpoint {
  std::size_t steps = 0;
  std::vector<std::uint8_t> host, engine;
};

struct RunOptions {
  double fMinHz = 2.0e9, fMaxHz = 3.0e9;           // the reported S11 sweep
  double excitationMinHz = 0, excitationMaxHz = 0; // source spectrum; 0 = same as the sweep
  std::size_t points = 101;
  std::size_t cpmlCells = 8;
  double courant = 0.95;
  std::size_t maxSteps = 200000;
  // Steps per advance(), which is also the cadence of onSample (Nyquist: 1/(2*16*dt) is far above the band).
  std::size_t sampleEvery = 16;
  // Called after every advance with the engine, the time of E, and the time covered since the previous call.
  std::function<void(Engine&, double, double)> onSample;
  // Called once after the last sample, with the engine still alive (read accumulated phasors here).
  std::function<void(Engine&)> onFinish;
  // Called after every advance with the partial result (steps, aggregated port voltage and source so far).
  std::function<void(const RunResult&)> onProgress;
  std::function<bool()> shouldStop; // polled after every advance; true ends the run early (a closed window)
  double decay =
      1e-3; // stop once the port voltage over the last 200 steps is below this fraction of its peak
  // Checkpoints. `resume` continues a run from a saved state (the model and options must be the ones it was
  // saved with: the caller checks that). After every advance, once the run's bookkeeping is done and it has
  // not ended, `checkpointDue(steps)` is asked, and `pauseRequested()`; if either says yes the state is
  // handed to `onCheckpoint`; a pause then ends the run at once with RunResult::paused (no onFinish, no S11).
  const Checkpoint* resume = nullptr;
  std::function<bool(std::size_t)> checkpointDue;
  std::function<bool()> pauseRequested;
  std::function<void(const Checkpoint&)> onCheckpoint;
};

// The solver configured from a model: materials, PEC edges, CPML and the port-0 edges driven by `waveform`.
fdtd::Solver<float> buildSolver(const board::Model& model, const RunOptions& opt,
                                std::function<double(double)> waveform);

// Backend factory: given the configured solver, make an engine (CPU or GPU).
using EngineFactory = std::function<std::unique_ptr<Engine>(fdtd::Solver<float>&)>;
std::unique_ptr<Engine> makeCpuEngine(fdtd::Solver<float>& solver);

struct RunResult {
  fdtd::SParam1 s;
  std::size_t steps = 0;
  // How far the run is, 0 to 1, for a progress display. The run ends when the port voltage has decayed to
  // `decay` of its peak, so the total step count is not known in advance: 60 % is the pulse being injected
  // (steps against the pulse length), 40 % the ring-down on a log scale (peak / recent against the stop
  // level). Never goes backwards. Only an estimate.
  double progress = 0;
  double dt = 0;
  std::vector<double> portVoltage, portSource; // aggregated, for plots and checks
  // The voltage of every parallel strip of the driven port after each step, [strip][step], and a strip's
  // source resistance (the port impedance times the number of strips): with them the power the port really
  // delivers is the sum over strips of V_k I_k, which stays correct when the strips see different voltages
  // (docs/PLAN.md, the wide-port finding).
  std::vector<std::vector<double>> stripVoltage;
  double stripResistance = 0;
  bool paused = false;             // ended at a pause request after a checkpoint: s is empty
  std::size_t resumedFromStep = 0; // 0: not a resumed run
};

// Modulated-Gaussian pulse covering the band; its parameters are returned through the options' band.
std::function<double(double)> bandPulse(double fMinHz, double fMaxHz);

RunResult runOnePort(const board::Model& model, const RunOptions& opt, const EngineFactory& makeEngine);

} // namespace sim
