#include "fdtd_vk/gpu_solver.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <stdexcept>

#include "core/resources.hpp"

namespace fdtd_vk {
namespace {
constexpr std::size_t kMaxPalette = 256;
constexpr std::uint32_t kGroups = 8192; // grid-stride loop: a fixed dispatch covers any grid size

template <class T> void upload(vkcore::Buffer& b, std::size_t floatOffset, std::span<const T> src) {
  std::copy(src.begin(), src.end(), b.as<float>().begin() + static_cast<std::ptrdiff_t>(floatOffset));
}

// The two other axes of a derivative axis, in increasing order (matches the shader).
std::array<fdtd::Axis, 2> otherAxes(fdtd::Axis d) {
  return {d == fdtd::X ? fdtd::Y : fdtd::X, d == fdtd::Z ? fdtd::Y : fdtd::Z};
}
} // namespace

GpuSolver::GpuSolver(vkcore::Context& ctx, const fdtd::Solver<float>& init)
    : ctx_(ctx), nodeCount_(init.grid().nodeCount()), ports_(init.ports()), dt_(init.dt()) {
  const fdtd::Grid& g = init.grid();
  const std::size_t fieldBytes = nodeCount_ * sizeof(float);
  // The simulation state is device-resident: in VRAM on a discrete GPU (a staging copy brings the initial
  // data in and the few read-backs out), the plain host-visible buffer on unified memory.
  const auto resident = vkcore::Residency::Device;
  for (int i = 0; i < 6; ++i)
    buffers_.push_back(std::make_unique<vkcore::Buffer>(ctx, fieldBytes, resident));
  buffers_.push_back(
      std::make_unique<vkcore::Buffer>(ctx, fieldBytes, resident)); // 6: packed material indices

  // 7: tables: 1/cell and 1/dual sizes, then (with CPML) b and a at cells (H) and at nodes (E), per axis.
  // Keep one submit near 25 ms (macOS GPU watchdog) at the measured ~2000 Mcells/s: steps = 5e7 / nodes.
  stepsPerSubmit_ = static_cast<std::uint32_t>(
      std::clamp<std::size_t>(50'000'000 / std::max<std::size_t>(nodeCount_, 1), 1, kMaxStepsPerSubmit));
  const std::size_t pml = init.cpmlCells();
  std::size_t tableFloats = 0;
  for (fdtd::Axis a : {fdtd::X, fdtd::Y, fdtd::Z})
    tableFloats += g.cells(a) + g.nodes(a) + (pml ? 2 * (g.cells(a) + g.nodes(a)) : 0);
  buffers_.push_back(std::make_unique<vkcore::Buffer>(ctx, tableFloats * sizeof(float), resident));

  for (fdtd::Axis a : {fdtd::X, fdtd::Y, fdtd::Z}) {
    upload<float>(*buffers_[a], 0, init.field(fdtd::Kind::E, a));
    upload<float>(*buffers_[3 + a], 0, init.field(fdtd::Kind::H, a));
  }
  // Edge coefficients depend only on (ca, cb), and few distinct pairs exist: store a 1-byte palette index per
  // edge (3 per node, packed in one word) instead of six floats per node.
  std::map<std::pair<float, float>, std::uint32_t> palette;
  std::vector<std::array<float, 2>> paletteData;
  auto* mat = reinterpret_cast<std::uint32_t*>(buffers_[6]->as<float>().data());
  std::fill(mat, mat + nodeCount_, 0u);
  for (fdtd::Axis a : {fdtd::X, fdtd::Y, fdtd::Z}) {
    const auto ca = init.ca(a), cb = init.cb(a);
    for (std::size_t n = 0; n < nodeCount_; ++n) {
      const auto [it, added] =
          palette.try_emplace({ca[n], cb[n]}, static_cast<std::uint32_t>(palette.size()));
      if (added) {
        if (palette.size() > kMaxPalette)
          throw std::runtime_error("more than 256 distinct edge materials; widen the material index");
        paletteData.push_back({ca[n], cb[n]});
      }
      mat[n] |= it->second << (8 * a);
    }
  }
  paletteMaterials_ = palette.size();

  std::array<std::uint32_t, 3> cellOff{}, dualOff{};
  std::size_t off = 0;
  for (fdtd::Axis a : {fdtd::X, fdtd::Y, fdtd::Z}) {
    cellOff[a] = static_cast<std::uint32_t>(off);
    upload<float>(*buffers_[7], off, init.invCell(a));
    off += g.cells(a);
  }
  for (fdtd::Axis a : {fdtd::X, fdtd::Y, fdtd::Z}) {
    dualOff[a] = static_cast<std::uint32_t>(off);
    upload<float>(*buffers_[7], off, init.invDual(a));
    off += g.nodes(a);
  }
  fdtd_gpu::StepParams base{static_cast<std::uint32_t>(g.nodes(fdtd::X)),
                            static_cast<std::uint32_t>(g.nodes(fdtd::Y)),
                            static_cast<std::uint32_t>(g.nodes(fdtd::Z)),
                            fdtd_gpu::kPhaseH,
                            static_cast<float>(init.dt() / fdtd::units::kMu0),
                            cellOff[0],
                            cellOff[1],
                            cellOff[2],
                            dualOff[0],
                            dualOff[1],
                            dualOff[2]};
  base.pmlCells = static_cast<std::uint32_t>(pml);
  if (pml) {
    for (fdtd::Axis a : {fdtd::X, fdtd::Y, fdtd::Z}) {
      base.pmlBH[a] = static_cast<std::uint32_t>(off);
      upload<float>(*buffers_[7], off, init.cpmlBH(a));
      off += g.cells(a);
      base.pmlAH[a] = static_cast<std::uint32_t>(off);
      upload<float>(*buffers_[7], off, init.cpmlAH(a));
      off += g.cells(a);
      base.pmlBE[a] = static_cast<std::uint32_t>(off);
      upload<float>(*buffers_[7], off, init.cpmlBE(a));
      off += g.nodes(a);
      base.pmlAE[a] = static_cast<std::uint32_t>(off);
      upload<float>(*buffers_[7], off, init.cpmlAE(a));
      off += g.nodes(a);
    }
  }

  buffers_.push_back(
      std::make_unique<vkcore::Buffer>(ctx, kMaxPalette * 2 * sizeof(float), resident)); // 8: palette
  std::copy(&paletteData[0][0], &paletteData[0][0] + 2 * paletteData.size(),
            buffers_[8]->as<float>().begin());

  // 9: CPML psi, only on the 2*pml layers next to the walls (H terms then E terms, two per derivative axis).
  std::size_t psiFloats = 1;
  if (pml) {
    psiFloats = 0;
    for (fdtd::Axis d : {fdtd::X, fdtd::Y, fdtd::Z}) {
      const auto [u, v] = otherAxes(d);
      psiFloats += 4 * 2 * pml * g.nodes(u) * g.nodes(v); // (H + E) * 2 terms * 2*pml layers
    }
  }
  buffers_.push_back(std::make_unique<vkcore::Buffer>(ctx, psiFloats * sizeof(float), resident));
  std::fill(buffers_[9]->as<float>().begin(), buffers_[9]->as<float>().end(), 0.0f);

  // 10: ports: counter header, port table, then the per-submit waveform and E log.
  const std::size_t portCount = ports_.size();
  base.portCount = static_cast<std::uint32_t>(portCount);
  base.maxSteps = kMaxStepsPerSubmit;
  const std::size_t portWords = 4 + 8 * portCount + 2 * kMaxStepsPerSubmit * portCount;
  buffers_.push_back(std::make_unique<vkcore::Buffer>(ctx, portWords * sizeof(float), resident));
  auto words = buffers_[10]->as<float>();
  std::fill(words.begin(), words.end(), 0.0f);
  for (std::size_t p = 0; p < portCount; ++p) {
    const fdtd::LumpedPort& port = ports_[p];
    const std::array<std::size_t, 3> idx{port.i, port.j, port.k};
    const std::uint32_t node = static_cast<std::uint32_t>(g.nodeIndex(port.i, port.j, port.k));
    const std::uint32_t comp = static_cast<std::uint32_t>(port.component);
    const float coupling = static_cast<float>(1.0 / (port.resistance * init.portArea(port)));
    std::memcpy(&words[4 + 8 * p], &node, 4);
    std::memcpy(&words[4 + 8 * p + 1], &comp, 4);
    words[4 + 8 * p + 2] = coupling;
    const double dlEdge = g.cellSize(port.component, idx[port.component]);
    words[4 + 8 * p + 4] =
        port.inductance > 0 ? static_cast<float>(init.dt() * dlEdge / (2 * port.inductance)) : 0.0f;
    words[4 + 8 * p + 5] = static_cast<float>(1.0 / init.portArea(port));
    portDl_.push_back(g.cellSize(port.component, idx[port.component]));
  }
  portV_.resize(portCount);
  portVs_.resize(portCount);

  // The dispatch order of one leapfrog step.
  auto add = [&](std::uint32_t phase) {
    sequence_.push_back(base);
    sequence_.back().phase = phase;
  };
  add(fdtd_gpu::kPhaseH);
  if (pml)
    for (std::uint32_t d = 0; d < 3; ++d)
      add(fdtd_gpu::kPhaseCpmlH + d);
  add(fdtd_gpu::kPhaseE);
  if (pml)
    for (std::uint32_t d = 0; d < 3; ++d)
      add(fdtd_gpu::kPhaseCpmlE + d);
  if (portCount) {
    add(fdtd_gpu::kPhasePorts);
    add(fdtd_gpu::kPhaseCounter);
  }
  for (const auto& s : sequence_)
    pushes_.push_back(&s);

  for (auto& b : buffers_) {
    b->upload(); // staged buffers: the initial state goes to VRAM once
    bound_.push_back(b.get());
  }
  pipeline_ = std::make_unique<vkcore::ComputePipeline>(
      ctx, core::resourceDir("shaders", FDTD_SHADER_DIR) + "/fdtd_step.comp.spv", 11,
      sizeof(fdtd_gpu::StepParams));
}

vkcore::Timing GpuSolver::step(std::uint32_t n) {
  vkcore::Timing total{};
  auto words = buffers_[10]->as<float>();
  const std::size_t portCount = ports_.size();
  while (n > 0) {
    const std::uint32_t chunk = std::min(n, stepsPerSubmit_);
    if (portCount) {
      std::memset(&words[0], 0, sizeof(std::uint32_t)); // device step counter back to 0
      float* wave = &words[4 + 8 * portCount];
      for (std::uint32_t s = 0; s < chunk; ++s)
        for (std::size_t p = 0; p < portCount; ++p) {
          if (!ports_[p].record) { // a passive load: no source, no history
            wave[s * portCount + p] = 0.0f;
            continue;
          }
          const double vs = ports_[p].voltage((static_cast<double>(stepsDone_ + s) + 0.5) * dt_);
          wave[s * portCount + p] = static_cast<float>(vs);
          portVs_[p].push_back(vs);
        }
    }
    if (portCount)
      buffers_[10]->upload(); // counter header and source waveform (a few kB)
    const auto t = pipeline_->runSequence(bound_, pushes_, kGroups, chunk);
    total.gpuMs += t.gpuMs;
    total.wallMs += t.wallMs;
    if (portCount) {
      buffers_[10]->download(); // the E log of this submit
      const float* log = &words[4 + 8 * portCount + kMaxStepsPerSubmit * portCount];
      for (std::uint32_t s = 0; s < chunk; ++s)
        for (std::size_t p = 0; p < portCount; ++p)
          if (ports_[p].record)
            portV_[p].push_back(static_cast<double>(log[s * portCount + p]) * portDl_[p]);
    }
    stepsDone_ += chunk;
    n -= chunk;
  }
  return total;
}

std::span<const float> GpuSolver::field(fdtd::Kind kind, fdtd::Axis component) const {
  const std::size_t index = (kind == fdtd::Kind::E ? 0 : 3) + component;
  if (fieldSynced_[index] != stepsDone_) { // staged buffers: bring the field back once per step count
    buffers_[index]->download();
    fieldSynced_[index] = stepsDone_;
  }
  return buffers_[index]->as<float>().first(nodeCount_);
}

namespace {
constexpr std::uint32_t kGpuTag = 0x53'55'50'47; // "GPUS"
constexpr std::uint32_t kGpuVersion = 2;         // 2: passive loads keep no voltage history
} // namespace

void GpuSolver::saveState(core::ByteWriter& w) const {
  if (!lists_.empty())
    throw std::runtime_error("checkpoints do not cover phasor lists (animation views)");
  w.put(kGpuTag);
  w.put(kGpuVersion);
  w.put<std::uint64_t>(nodeCount_);
  w.put<std::uint64_t>(ports_.size());
  w.put<std::uint64_t>(buffers_[9]->as<float>().size());
  w.put<std::uint64_t>(stepsDone_);
  for (int i = 0; i < 6; ++i) {
    buffers_[i]->download();
    w.putArray<float>(buffers_[i]->as<float>().first(nodeCount_));
  }
  buffers_[9]->download();
  w.putArray<float>(buffers_[9]->as<float>());
  buffers_[10]->download();
  const auto words = buffers_[10]->as<float>();
  for (std::size_t p = 0; p < ports_.size(); ++p) {
    w.put(words[4 + 8 * p + 6]); // inductor current
    w.put(words[4 + 8 * p + 7]); // E at the end of the previous step
  }
  for (std::size_t p = 0; p < ports_.size(); ++p)
    if (ports_[p].record)
      w.putArray(portV_[p]);
  for (std::size_t p = 0; p < ports_.size(); ++p)
    if (ports_[p].record)
      w.putArray(portVs_[p]);
}

void GpuSolver::restoreState(core::ByteReader& r) {
  if (!lists_.empty())
    throw std::runtime_error("checkpoints do not cover phasor lists (animation views)");
  if (r.get<std::uint32_t>() != kGpuTag || r.get<std::uint32_t>() != kGpuVersion)
    throw std::runtime_error("the checkpoint's GPU state has another format");
  if (r.get<std::uint64_t>() != nodeCount_)
    throw std::runtime_error("the checkpoint was written for a grid of another size");
  if (r.get<std::uint64_t>() != ports_.size())
    throw std::runtime_error("the checkpoint was written for another number of ports");
  if (r.get<std::uint64_t>() != buffers_[9]->as<float>().size())
    throw std::runtime_error("the checkpoint was written with another number of CPML layers");
  const std::uint64_t steps = r.get<std::uint64_t>();
  std::vector<float> tmp;
  for (int i = 0; i < 6; ++i) {
    r.getArray(tmp, nodeCount_);
    std::copy(tmp.begin(), tmp.end(), buffers_[i]->as<float>().begin());
    buffers_[i]->upload();
  }
  r.getArray(tmp, buffers_[9]->as<float>().size());
  std::copy(tmp.begin(), tmp.end(), buffers_[9]->as<float>().begin());
  buffers_[9]->upload();
  auto words = buffers_[10]->as<float>();
  for (std::size_t p = 0; p < ports_.size(); ++p) {
    words[4 + 8 * p + 6] = r.get<float>();
    words[4 + 8 * p + 7] = r.get<float>();
  }
  buffers_[10]->upload();
  for (std::size_t p = 0; p < ports_.size(); ++p)
    if (ports_[p].record)
      r.getArray(portV_[p], steps);
  for (std::size_t p = 0; p < ports_.size(); ++p)
    if (ports_[p].record)
      r.getArray(portVs_[p], steps);
  stepsDone_ = steps;
  fieldSynced_.fill(~0ull); // the host views of the fields are stale now
}

} // namespace fdtd_vk
