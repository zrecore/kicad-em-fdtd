// GPU FDTD backend: same scheme, storage layout and operation order as the CPU reference.
// Built from a fdtd::Solver<float> so coefficients, spacing tables and initial fields are shared, not redone.
#pragma once
#include <complex>
#include <memory>
#include <vector>

#include "core/units.hpp"
#include "fdtd/solver.hpp"
#include "shared/fdtd_params.h"
#include "vkcore/compute.hpp"

namespace fdtd_vk {

// One sample of a phasor list: a field component at a node (grid.hpp node index).
struct PhasorChannel {
  fdtd::Kind kind;
  fdtd::Axis component;
  std::uint32_t node;
};

class GpuSolver {
public:
  GpuSolver(vkcore::Context& ctx, const fdtd::Solver<float>& init);
  // n leapfrog steps in one submission (keep a submit to tens of ms; see docs/PLAN.md watchdog notes).
  // Ports and CPML configured on the CPU solver are carried over. Long calls are split into submits of at
  // most kMaxStepsPerSubmit steps.
  vkcore::Timing step(std::uint32_t n);
  static constexpr std::uint32_t kMaxStepsPerSubmit = 64;
  // Port voltage (E * edge length) after each step so far, and the source voltage that drove it.
  const std::vector<double>& portVoltage(std::size_t port) const { return portV_[port]; }
  const std::vector<double>& portSourceVoltage(std::size_t port) const { return portVs_[port]; }
  std::span<const float> field(fdtd::Kind kind, fdtd::Axis component) const;
  std::size_t nodeCount() const { return nodeCount_; }
  std::uint64_t stepsDone() const { return stepsDone_; }
  std::size_t paletteMaterials() const { return paletteMaterials_; }

  // Running DFT (Kahan-compensated) of a list of channels at N frequencies, accumulated on the GPU. One
  // kernel serves every list: display voxels, copper-face cells and the far-field box differ only in their
  // channels (docs/PLAN.md, Next steps 11). Returns the list's handle.
  std::size_t addPhasorList(std::vector<PhasorChannel> channels, std::vector<double> frequenciesHz);
  // Adds the current fields as one sample: X_k += value * weight * exp(-j 2 pi f_k t). E is at time
  // stepsDone*dt and H half a step earlier. weight is the time the sample stands for (every*dt when sampling
  // every `every` steps), so X_k approximates the Fourier integral.
  void accumulate(std::size_t list, double weight);
  std::complex<double> phasor(std::size_t list, std::size_t frequency, std::size_t channel) const;
  std::size_t channelCount(std::size_t list) const { return lists_[list]->channels; }

private:
  struct PhasorList {
    std::size_t channels = 0;
    std::vector<double> frequencies;
    std::unique_ptr<vkcore::Buffer> list, acc, trig;
  };
  vkcore::Context& ctx_;
  std::vector<std::unique_ptr<PhasorList>> lists_;
  std::unique_ptr<vkcore::ComputePipeline> accumPipeline_; // created with the first list
  std::size_t nodeCount_;
  std::size_t paletteMaterials_ = 0;
  std::uint32_t stepsPerSubmit_ = 1;                     // sized so one submit stays near 25 ms
  std::vector<std::unique_ptr<vkcore::Buffer>> buffers_; // ex ey ez hx hy hz mat tables palette
  std::vector<vkcore::Buffer*> bound_;
  std::unique_ptr<vkcore::ComputePipeline> pipeline_;
  std::vector<fdtd_gpu::StepParams> sequence_; // one entry per dispatch of a step, in order
  std::vector<const void*> pushes_;
  std::vector<fdtd::LumpedPort> ports_;
  std::vector<double> portDl_;
  std::vector<std::vector<double>> portV_, portVs_;
  double dt_ = 0;
  std::uint64_t stepsDone_ = 0;
};

} // namespace fdtd_vk
