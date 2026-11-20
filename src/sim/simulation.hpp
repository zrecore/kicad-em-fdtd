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
  double dt = 0;
  std::vector<double> portVoltage, portSource; // aggregated, for plots and checks
};

// Modulated-Gaussian pulse covering the band; its parameters are returned through the options' band.
std::function<double(double)> bandPulse(double fMinHz, double fMaxHz);

RunResult runOnePort(const board::Model& model, const RunOptions& opt, const EngineFactory& makeEngine);

} // namespace sim
