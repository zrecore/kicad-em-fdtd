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
  std::size_t addPhasorList(std::vector<fdtd::PhasorChannel> channels,
                            std::vector<double> frequenciesHz) override {
    return bank_.add(std::move(channels), std::move(frequenciesHz));
  }
  void accumulate(std::size_t list, double weight) override {
    bank_.accumulate(
        list, weight, s_.time(), 0.5 * s_.dt(),
        [&](fdtd::Kind kind, fdtd::Axis a, std::uint32_t node) -> double { return s_.field(kind, a)[node]; });
  }
  std::complex<double> phasor(std::size_t list, std::size_t k, std::size_t c) const override {
    return bank_.phasor(list, k, c);
  }
  void copyPhasors(std::size_t list, std::span<float> re, std::span<float> im) const override {
    bank_.copy(list, re, im);
  }
  void saveState(core::ByteWriter& w) const override {
    if (!bank_.empty())
      throw std::runtime_error("checkpoints do not cover phasor lists (animation views)");
    s_.saveState(w);
    w.put<std::uint64_t>(series_.size());
    for (const auto& v : series_)
      w.putArray(v);
  }
  void restoreState(core::ByteReader& r) override {
    if (!bank_.empty())
      throw std::runtime_error("checkpoints do not cover phasor lists (animation views)");
    s_.restoreState(r);
    if (r.get<std::uint64_t>() != series_.size())
      throw std::runtime_error("the checkpoint was written for another number of port edges");
    for (auto& v : series_)
      r.getArray(v);
  }

private:
  fdtd::PhasorBank bank_;
  fdtd::Solver<float>& s_;
  std::vector<double> dl_;
  std::vector<std::vector<double>> series_;
};

// Voltage of every parallel strip after step n: the signed sum along the strip.
std::vector<double> stripVoltagesAt(const board::PortPlan& port, const Engine& e, std::size_t n) {
  std::vector<double> strip(port.parallel, 0.0);
  for (std::size_t i = 0; i < port.edges.size(); ++i)
    strip[port.edges[i].strip] += port.edges[i].sign * e.edgeVoltage(i)[n];
  return strip;
}

constexpr std::uint32_t kHostTag = 0x54'53'4F'48; // "HOST"
constexpr std::uint32_t kHostVersion = 1;

// What runOnePort itself holds between advances (the decay window and the port histories S11 comes from).
struct HostState {
  RunResult* r;
  double* peak;
  std::vector<double>* chunkMax;
};

void saveHost(core::ByteWriter& w, const HostState& h) {
  w.put(kHostTag);
  w.put(kHostVersion);
  w.put<std::uint64_t>(h.r->steps);
  w.put(h.r->progress);
  w.put(*h.peak);
  w.putArray(*h.chunkMax);
  w.putArray(h.r->portVoltage);
  w.putArray(h.r->portSource);
  w.put<std::uint64_t>(h.r->stripVoltage.size());
  for (const auto& v : h.r->stripVoltage)
    w.putArray(v);
}

void restoreHost(core::ByteReader& rd, const HostState& h, std::size_t chunk) {
  if (rd.get<std::uint32_t>() != kHostTag || rd.get<std::uint32_t>() != kHostVersion)
    throw std::runtime_error("the checkpoint's run state has another format");
  const std::size_t steps = static_cast<std::size_t>(rd.get<std::uint64_t>());
  h.r->progress = rd.get<double>();
  *h.peak = rd.get<double>();
  rd.getArray(*h.chunkMax);
  rd.getArray(h.r->portVoltage, steps);
  rd.getArray(h.r->portSource, steps);
  if (rd.get<std::uint64_t>() != h.r->stripVoltage.size())
    throw std::runtime_error("the checkpoint was written for another number of port strips");
  for (auto& v : h.r->stripVoltage)
    rd.getArray(v, steps);
  if (steps % chunk != 0 || h.chunkMax->size() != steps / chunk)
    throw std::runtime_error("the checkpoint was written with another sample interval");
  h.r->steps = steps;
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
  if (model.ports.empty() || model.ports.front().lumped) // lumped parts are loads, never the source
    throw std::invalid_argument("the model has no port to drive");
  for (std::size_t p = 0; p < model.ports.size(); ++p)
    for (const board::PortEdge& pe : model.ports[p].edges) {
      fdtd::LumpedPort lp = pe.edge;
      const double scale =
          p == 0 ? pe.sign * pe.share : 0.0; // only port 0 is driven; others are passive loads
      lp.voltage = [waveform, scale](double t) { return scale * waveform(t); };
      lp.record = p == 0; // the passive loads need no voltage history: only port 0's strips are read
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
  r.stripVoltage.assign(port.parallel, {});
  r.stripResistance = port.impedance * static_cast<double>(port.parallel);
  const double tau = 1.1 / (exMax - exMin);
  const std::size_t pulseSteps = static_cast<std::size_t>(8.0 * tau / r.dt) + 1;
  const std::uint32_t kChunk = static_cast<std::uint32_t>(opt.sampleEvery);
  double peak = 0;
  std::vector<double> chunkMax;
  const HostState host{&r, &peak, &chunkMax};
  if (opt.resume) { // continue where a checkpoint left off: the same model and options, by the caller's check
    core::ByteReader hr(opt.resume->host), er(opt.resume->engine);
    restoreHost(hr, host, kChunk);
    engine->restoreState(er);
    if (!hr.atEnd() || !er.atEnd() || r.steps != opt.resume->steps)
      throw std::runtime_error("the checkpoint is inconsistent");
    for (std::size_t i = 0; i < port.edges.size(); ++i)
      if (engine->edgeVoltage(i).size() != r.steps)
        throw std::runtime_error("the checkpoint's port histories do not match its step count");
    r.resumedFromStep = r.steps;
  }
  while (r.steps < opt.maxSteps) {
    engine->advance(kChunk);
    if (opt.onSample)
      opt.onSample(*engine, engine->time(), static_cast<double>(kChunk) * r.dt);
    double recent = 0;
    for (std::size_t n = r.steps; n < r.steps + kChunk; ++n) {
      const auto strips = stripVoltagesAt(port, *engine, n);
      double mean = 0;
      for (std::size_t k = 0; k < strips.size(); ++k) {
        r.stripVoltage[k].push_back(strips[k]);
        mean += strips[k];
      }
      r.portVoltage.push_back(mean / static_cast<double>(port.parallel));
      r.portSource.push_back(pulse((static_cast<double>(n) + 0.5) * r.dt));
      recent = std::max(recent, std::abs(r.portVoltage.back()));
    }
    r.steps += kChunk;
    chunkMax.push_back(recent);
    peak = std::max(peak, recent);
    // decay is judged over the last ~200 steps (an oscillating signal passes through zero within a chunk)
    const std::size_t windowChunks = std::max<std::size_t>(1, 200 / kChunk);
    recent = *std::max_element(chunkMax.end() -
                                   static_cast<std::ptrdiff_t>(std::min(windowChunks, chunkMax.size())),
                               chunkMax.end());
    // The pulse is still being injected for pulseSteps (most of a long run), then the signal rings down to
    // the stop level: 60 % for the first, 40 % for the decay (log scale), so the bar moves from the start.
    double estimate = 0.6 * std::min(1.0, static_cast<double>(r.steps) / static_cast<double>(pulseSteps));
    if (r.steps > pulseSteps && peak > 0 && recent > 0 && opt.decay > 0 && opt.decay < 1)
      estimate = 0.6 + 0.4 * std::clamp(std::log(peak / recent) / std::log(1.0 / opt.decay), 0.0, 1.0);
    r.progress = std::max(r.progress, std::min(estimate, 0.99));
    if (opt.onProgress)
      opt.onProgress(r);
    if (opt.shouldStop && opt.shouldStop())
      break;
    if (r.steps > pulseSteps && recent < opt.decay * peak) {
      r.progress = 1.0;
      break;
    }
    if (opt.onCheckpoint) { // the run goes on: a quiet point, the GPU idle between advances
      const bool pause = opt.pauseRequested && opt.pauseRequested();
      if (pause || (opt.checkpointDue && opt.checkpointDue(r.steps))) {
        Checkpoint cp;
        cp.steps = r.steps;
        core::ByteWriter hw, ew;
        saveHost(hw, host);
        engine->saveState(ew);
        cp.host = std::move(hw.bytes());
        cp.engine = std::move(ew.bytes());
        opt.onCheckpoint(cp);
        if (pause) {
          r.paused = true;
          return r; // no onFinish, no S11: the run is not over
        }
      }
    }
  }
  if (opt.onFinish)
    opt.onFinish(*engine);
  std::vector<double> freqs(opt.points);
  for (std::size_t i = 0; i < opt.points; ++i)
    freqs[i] =
        opt.fMinHz + (opt.fMaxHz - opt.fMinHz) * static_cast<double>(i) / static_cast<double>(opt.points - 1);
  r.s = fdtd::s11FromPort(r.portVoltage, r.portSource, r.dt, freqs, port.impedance);
  return r;
}

} // namespace sim
