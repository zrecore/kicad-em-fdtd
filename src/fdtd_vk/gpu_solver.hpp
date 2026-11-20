// GPU FDTD backend: same scheme, storage layout and operation order as the CPU reference.
// Built from a fdtd::Solver<float> so coefficients, spacing tables and initial fields are shared, not redone.
#pragma once
#include <memory>
#include <vector>

#include "core/units.hpp"
#include "fdtd/solver.hpp"
#include "shared/fdtd_params.h"
#include "vkcore/compute.hpp"

namespace fdtd_vk {

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

private:
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
