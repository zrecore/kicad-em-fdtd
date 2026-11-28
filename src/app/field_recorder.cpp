#include "app/field_recorder.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>

#include "fdtd/impedance.hpp"
#include "fdtd/sparam.hpp"

namespace app {

void FieldRecorder::reset() {
  phasors.reset();
  volPhasors.reset();
  currentH.reset();
  poyntingH.reset();
  ntffAcc.reset();
  farResults.clear();
  feeds_.clear();
  farFeed_.reset();
  currentList = {};
  ntffNote.clear();
  ntffFreqs.clear();
  hasNtff = false;
}

void FieldRecorder::layout(const board::Snapshot& snap, const Prepared& prepared) {
  reset();
  const fdtd::Grid& g = prepared.model.grid;
  const auto& m = prepared.model;
  // Ez on a plane mid-way between the outer copper layers, at 25 frequencies across the sweep.
  kTop = 0;
  kBottom = g.nodes(fdtd::Z);
  for (std::size_t i = 0; i < snap.stackup.size(); ++i)
    if (snap.stackup[i].copper) {
      kTop = std::max(kTop, m.copperPlaneNode[i]);
      kBottom = std::min(kBottom, m.copperPlaneNode[i]);
    }
  plane = viz::makePlane(g, fdtd::Z, (kTop + kBottom) / 2);
  const auto& sweep = prepared.plan.sweepHz;
  frequencies.assign(std::max<std::size_t>(2, prepared.plan.animationFrequencies), 0.0);
  for (std::size_t i = 0; i < frequencies.size(); ++i)
    frequencies[i] = sweep[0] + (sweep[1] - sweep[0]) * static_cast<double>(i) / (frequencies.size() - 1);
  fieldAxis = prepared.plan.fieldComponent == "ex"   ? fdtd::X
              : prepared.plan.fieldComponent == "ey" ? fdtd::Y
                                                     : fdtd::Z;
  // the volume covers the board and a margin around it: 30 % sideways, and as far up and down
  double bx0 = 1e9, bx1 = -1e9, by0 = 1e9, by1 = -1e9;
  for (const auto& pt : snap.outline) {
    bx0 = std::min(bx0, pt.x - m.originX);
    bx1 = std::max(bx1, pt.x - m.originX);
    by0 = std::min(by0, pt.y - m.originY);
    by1 = std::max(by1, pt.y - m.originY);
  }
  const double side = std::max(bx1 - bx0, by1 - by0), zc = g.edge(fdtd::Z, (kTop + kBottom) / 2);
  viz::VolumeBox box;
  box.lo = {std::max(0.0, bx0 - 0.3 * side), std::max(0.0, by0 - 0.3 * side), std::max(0.0, zc - 0.4 * side)};
  box.hi = {std::min(g.length(fdtd::X), bx1 + 0.3 * side), std::min(g.length(fdtd::Y), by1 + 0.3 * side),
            std::min(g.length(fdtd::Z), zc + 0.4 * side)};
  dv = viz::makeDisplayVolume(g, prepared.plan.volumeResolution, box);
  currentList = fdtd::surfaceCurrentList(g, m.pecEdges);
  // Far field: a box in free space inside the absorbing layer, at up to five frequencies of the sweep
  const std::size_t pml = prepared.plan.pmlCells + 2;
  std::array<std::size_t, 3> lo{}, hi{};
  bool ok = true;
  for (int a = 0; a < 3; ++a) {
    lo[static_cast<std::size_t>(a)] = pml;
    hi[static_cast<std::size_t>(a)] = g.nodes(static_cast<fdtd::Axis>(a)) - 1 - pml;
    ok = ok && hi[static_cast<std::size_t>(a)] > lo[static_cast<std::size_t>(a)] + 4;
  }
  // the box must clear the board and the dielectric: laterally the outline, vertically the copper planes
  if (ok)
    ok = g.edge(fdtd::X, lo[0]) < bx0 && g.edge(fdtd::X, hi[0]) > bx1 && g.edge(fdtd::Y, lo[1]) < by0 &&
         g.edge(fdtd::Y, hi[1]) > by1 && g.edge(fdtd::Z, lo[2]) < g.edge(fdtd::Z, kBottom) &&
         g.edge(fdtd::Z, hi[2]) > g.edge(fdtd::Z, kTop);
  if (!ok) {
    ntffNote = "No far field: the board fills the space inside the absorbing layer. Increase the air margin "
               "(Settings) to get one.";
    return;
  }
  ntffPlan = fdtd::makeNtffPlan(g, lo, hi);
  const std::size_t count = std::min<std::size_t>(5, frequencies.size());
  for (std::size_t i = 0; i < count; ++i)
    ntffFreqs.push_back(frequencies[count == 1 ? 0 : i * (frequencies.size() - 1) / (count - 1)]);
  hasNtff = true;
}

namespace {
// One saved file: magic, version, two counts, the frequencies, then per frequency a peak and re and im as
// half floats (run_archive.cpp savePhasors).
std::uint64_t fileBytes(std::size_t freqs, std::size_t voxels) {
  return 4 + 4 + 8 + 8 + 8ull * freqs + freqs * (4ull + 4ull * voxels);
}
} // namespace

std::uint64_t FieldRecorder::savedBytes() const {
  const std::size_t f = frequencies.size();
  std::uint64_t n =
      fileBytes(f, plane.nu * plane.nv) + fileBytes(f, dv.voxels()) + fileBytes(f, 6 * plane.nu * plane.nv);
  if (!currentList.channels.empty())
    n += fileBytes(f, currentList.channels.size());
  if (hasNtff) {
    n += fileBytes(ntffFreqs.size(), ntffPlan.channels.size());
    n += 600u * 1024; // farfield.json: five frequencies of a 36 x 72 table as text
  }
  return n;
}

void FieldRecorder::allocate() {
  phasors = std::make_unique<viz::PhasorAccumulator>(frequencies, plane.nu * plane.nv);
  volPhasors = std::make_unique<viz::PhasorAccumulator>(frequencies, dv.voxels());
  currentH = currentList.channels.empty()
                 ? nullptr
                 : std::make_unique<viz::PhasorAccumulator>(frequencies, currentList.channels.size());
  poyntingH = std::make_unique<viz::PhasorAccumulator>(frequencies, 6 * plane.nu * plane.nv);
  ntffAcc = hasNtff ? std::make_unique<viz::PhasorAccumulator>(ntffFreqs, ntffPlan.channels.size()) : nullptr;
}

void FieldRecorder::attach(sim::RunOptions& opt, const fdtd::Grid& g, std::mutex& mu,
                           const std::function<void()>& refreshed) {
  // Each accumulates on the backend (one phasor list each); the viewer's copies refresh about once a second
  // and at the end.
  auto at = [&](const std::array<std::size_t, 3>& idx) {
    return static_cast<std::uint32_t>(g.nodeIndex(idx[0], idx[1], idx[2]));
  };
  auto planeNode = [&](std::size_t a, std::size_t b) {
    std::array<std::size_t, 3> idx{};
    idx[plane.normal] = plane.index;
    idx[plane.u] = a;
    idx[plane.v] = b;
    return at(idx);
  };
  std::vector<fdtd::PhasorChannel> sliceChannels, volumeChannels, powerChannels;
  for (std::size_t b = 0; b < plane.nv; ++b)
    for (std::size_t a = 0; a < plane.nu; ++a) {
      const std::uint32_t node = planeNode(a, b);
      sliceChannels.push_back({fdtd::Kind::E, fieldAxis, node});
      for (fdtd::Kind kind : {fdtd::Kind::E, fdtd::Kind::H}) // E and H, all three components, at every node
        for (fdtd::Axis ax : {fdtd::X, fdtd::Y, fdtd::Z})
          powerChannels.push_back({kind, ax, node});
    }
  for (std::size_t k = 0; k < dv.dims[2]; ++k)
    for (std::size_t j = 0; j < dv.dims[1]; ++j)
      for (std::size_t i = 0; i < dv.dims[0]; ++i)
        volumeChannels.push_back(
            {fdtd::Kind::E, fieldAxis, at({dv.nodeOf[0][i], dv.nodeOf[1][j], dv.nodeOf[2][k]})});
  feeds_.clear();
  feeds_.push_back(std::make_unique<PhasorFeed>(*phasors, std::move(sliceChannels), frequencies));
  feeds_.push_back(std::make_unique<PhasorFeed>(*volPhasors, std::move(volumeChannels), frequencies));
  if (currentH)
    feeds_.push_back(std::make_unique<PhasorFeed>(*currentH, currentList.channels, frequencies));
  feeds_.push_back(std::make_unique<PhasorFeed>(*poyntingH, std::move(powerChannels), frequencies));
  farFeed_.reset();
  if (ntffAcc)
    farFeed_ = std::make_unique<PhasorFeed>(*ntffAcc, ntffPlan.channels, ntffFreqs);
  auto before = opt.onSample;
  opt.onSample = [this, before, &mu, refreshed](sim::Engine& eng, double t, double dtSample) {
    if (before)
      before(eng, t, dtSample);
    bool any = false;
    for (auto& f : feeds_)
      any = f->sample(eng, dtSample, mu) || any;
    if (farFeed_)
      any = farFeed_->sample(eng, dtSample, mu, 4.0) || any;
    if (any && refreshed)
      refreshed();
  };
  auto beforeFinish = opt.onFinish;
  opt.onFinish = [this, beforeFinish, &mu, refreshed](sim::Engine& eng) {
    if (beforeFinish)
      beforeFinish(eng);
    for (auto& f : feeds_)
      f->sync(eng, mu);
    if (farFeed_)
      farFeed_->sync(eng, mu);
    if (refreshed)
      refreshed();
  };
}

std::vector<FarResult> FieldRecorder::computeFar(const sim::RunResult& r, double impedanceOhm, int nTheta,
                                                 int nPhi) const {
  if (!ntffAcc)
    return {};
  const auto s11 = fdtd::s11FromPort(r.portVoltage, r.portSource, r.dt, ntffFreqs, impedanceOhm);
  const auto vs = fdtd::sourceSpectrum(r.portSource, r.dt, ntffFreqs);
  const auto accepted = fdtd::acceptedPower(r.stripVoltage, r.portSource, r.dt, r.stripResistance, ntffFreqs);
  std::vector<FarResult> out;
  for (std::size_t k = 0; k < ntffFreqs.size(); ++k) {
    FarResult fr;
    fr.frequencyHz = ntffFreqs[k];
    fr.field = fdtd::farField(
        ntffPlan, [&](std::uint32_t ch) { return std::complex<double>(ntffAcc->phasor(k, ch)); },
        ntffFreqs[k], nTheta, nPhi);
    fr.figures = fdtd::antennaFigures(fr.field, vs[k], s11.s11[k], impedanceOhm, std::max(accepted[k], 0.0));
    fr.s11Db = 20 * std::log10(std::max(std::abs(s11.s11[k]), 1e-6));
    out.push_back(std::move(fr));
  }
  return out;
}

void FieldRecorder::save(const std::filesystem::path& runDir) const {
  saveFields(runDir,
             {{"slice_e", phasors.get()},
              {"volume_e", volPhasors.get()},
              {"current_h", currentH.get()},
              {"power_eh", poyntingH.get()},
              {"ntff", ntffAcc.get()}},
             farResults);
}

std::uint64_t estimateFieldBytes(const board::Snapshot& snap, const Prepared& prepared) {
  FieldRecorder rec;
  rec.layout(snap, prepared);
  return rec.savedBytes();
}

} // namespace app
