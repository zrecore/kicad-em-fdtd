// Adapts GpuSolver to the simulation driver's Engine interface.
#pragma once
#include "fdtd_vk/gpu_solver.hpp"
#include "sim/engine.hpp"

namespace fdtd_vk {

class GpuEngine : public sim::Engine {
public:
  GpuEngine(vkcore::Context& ctx, const fdtd::Solver<float>& init) : gpu_(ctx, init), dt_(init.dt()) {}
  void advance(std::uint32_t steps) override { gpu_.step(steps); }
  const std::vector<double>& edgeVoltage(std::size_t edge) const override { return gpu_.portVoltage(edge); }
  std::span<const float> field(fdtd::Kind kind, fdtd::Axis a) const override { return gpu_.field(kind, a); }
  double time() const override { return static_cast<double>(gpu_.stepsDone()) * dt_; }
  std::size_t addPhasorList(std::vector<fdtd::PhasorChannel> channels,
                            std::vector<double> frequenciesHz) override {
    return gpu_.addPhasorList(std::move(channels), std::move(frequenciesHz));
  }
  void accumulate(std::size_t list, double weight) override { gpu_.accumulate(list, weight); }
  std::complex<double> phasor(std::size_t list, std::size_t k, std::size_t c) const override {
    return gpu_.phasor(list, k, c);
  }
  void copyPhasors(std::size_t list, std::span<float> re, std::span<float> im) const override {
    gpu_.copyPhasors(list, re, im);
  }
  const GpuSolver& solver() const { return gpu_; }

private:
  GpuSolver gpu_;
  double dt_;
};

} // namespace fdtd_vk
