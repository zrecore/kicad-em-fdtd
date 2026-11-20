#include "sim/simulation.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace sim {
namespace {

class CpuEngine : public Engine {
public:
  explicit CpuEngine(fdtd::Solver<float>& s) : s_(s) {
    for (const fdtd::LumpedPort& p : s.ports()) {
      const std::array<std::size_t, 3> idx{p.i, p.j, p.k};
      dl_.push_back(s.grid().cellSize(p.component, idx[p.component]));
    }
    series_.resize(dl_.size());
  }
  void advance(std::uint32_t steps) override {
    for (std::uint32_t n = 0; n < steps; ++n) {
      s_.step();
      for (std::size_t i = 0; i < dl_.size(); ++i) {
        const fdtd::LumpedPort& p = s_.ports()[i];
        series_[i].push_back(static_cast<double>(s_.e(p.component, p.i, p.j, p.k)) * dl_[i]);
      }
    }
  }
  const std::vector<double>& edgeVoltage(std::size_t edge) const override { return series_[edge]; }
  std::span<const float> field(fdtd::Kind kind, fdtd::Axis a) const override { return s_.field(kind, a); }
  double time() const override { return s_.time(); }

private:
  fdtd::Solver<float>& s_;
  std::vector<double> dl_;
  std::vector<std::vector<double>> series_;
};

// Port voltage after step n: mean over the parallel strips of the signed sum along a strip.
double portVoltageAt(const board::PortPlan& port, const Engine& e, std::size_t n) {
  std::vector<double> strip(port.parallel, 0.0);
  for (std::size_t i = 0; i < port.edges.size(); ++i)
    strip[port.edges[i].strip] += port.edges[i].sign * e.edgeVoltage(i)[n];
  double sum = 0;
  for (double v : strip)
    sum += v;
  return sum / static_cast<double>(port.parallel);
}

} // namespace

std::unique_ptr<Engine> makeCpuEngine(fdtd::Solver<float>& solver) {
  return std::make_unique<CpuEngine>(solver);
}

std::function<double(double)> bandPulse(double fMinHz, double fMaxHz) {
  const double f0 = 0.5 * (fMinHz + fMaxHz), bandwidth = fMaxHz - fMinHz;
  const double tau = 1.1 / bandwidth; // spectrum falls to ~5 % at the band edges
  const double t0 = 4.0 * tau;
  return [=](double t) {
    const double u = (t - t0) / tau;
    return std::exp(-u * u) * std::cos(2 * std::numbers::pi * f0 * (t - t0));
  };
}

fdtd::Solver<float> buildSolver(const board::Model& model, const RunOptions& opt,
                                std::function<double(double)> waveform) {
  fdtd::Solver<float> solver(model.grid, opt.courant);
  solver.setMaterials(model.materials, model.cellMaterial);
  solver.forcePecEdges(model.pecEdges);
  if (opt.cpmlCells)
    solver.enableCpml({.cells = opt.cpmlCells});
  if (model.ports.empty())
    throw std::invalid_argument("the model has no port to drive");
  for (std::size_t p = 0; p < model.ports.size(); ++p)
    for (const board::PortEdge& pe : model.ports[p].edges) {
      fdtd::LumpedPort lp = pe.edge;
      const double scale =
          p == 0 ? pe.sign * pe.share : 0.0; // only port 0 is driven; others are passive loads
      lp.voltage = [waveform, scale](double t) { return scale * waveform(t); };
      solver.addLumpedPort(std::move(lp));
    }
  return solver;
}

RunResult runOnePort(const board::Model& model, const RunOptions& opt, const EngineFactory& makeEngine) {
  const double exMin = opt.excitationMinHz > 0 ? opt.excitationMinHz : opt.fMinHz;
  const double exMax = opt.excitationMaxHz > 0 ? opt.excitationMaxHz : opt.fMaxHz;
  const auto pulse = bandPulse(exMin, exMax);
  auto solver = buildSolver(model, opt, pulse);
  auto engine = makeEngine(solver);
  const board::PortPlan& port = model.ports[0];
  RunResult r;
  r.dt = solver.dt();
  const double tau = 1.1 / (exMax - exMin);
  const std::size_t pulseSteps = static_cast<std::size_t>(8.0 * tau / r.dt) + 1;
  const std::uint32_t kChunk = static_cast<std::uint32_t>(opt.sampleEvery);
  double peak = 0;
  std::vector<double> chunkMax;
  while (r.steps < opt.maxSteps) {
    engine->advance(kChunk);
    if (opt.onSample)
      opt.onSample(*engine, engine->time(), static_cast<double>(kChunk) * r.dt);
    double recent = 0;
    for (std::size_t n = r.steps; n < r.steps + kChunk; ++n) {
      r.portVoltage.push_back(portVoltageAt(port, *engine, n));
      r.portSource.push_back(pulse((static_cast<double>(n) + 0.5) * r.dt));
      recent = std::max(recent, std::abs(r.portVoltage.back()));
    }
    r.steps += kChunk;
    if (opt.onProgress)
      opt.onProgress(r);
    if (opt.shouldStop && opt.shouldStop())
      break;
    chunkMax.push_back(recent);
    peak = std::max(peak, recent);
    // decay is judged over the last ~200 steps (an oscillating signal passes through zero within a chunk)
    const std::size_t windowChunks = std::max<std::size_t>(1, 200 / kChunk);
    recent = *std::max_element(chunkMax.end() -
                                   static_cast<std::ptrdiff_t>(std::min(windowChunks, chunkMax.size())),
                               chunkMax.end());
    if (r.steps > pulseSteps && recent < opt.decay * peak)
      break;
  }
  std::vector<double> freqs(opt.points);
  for (std::size_t i = 0; i < opt.points; ++i)
    freqs[i] =
        opt.fMinHz + (opt.fMaxHz - opt.fMinHz) * static_cast<double>(i) / static_cast<double>(opt.points - 1);
  r.s = fdtd::s11FromPort(r.portVoltage, r.portSource, r.dt, freqs, port.impedance);
  return r;
}

} // namespace sim
