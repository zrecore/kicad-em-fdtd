// GPU backend of the 2D cross-section solve (fdtd/line2d.hpp): red-black over-relaxation in float32, the same
// iteration as the CPU reference. Metal has no fp64, so the potential is single precision; the capacitance is
// summed on the CPU in double from the read-back potential. The sweeps run in batches; after each batch the
// largest change of the potential over the batch is the stopping test.
#pragma once
#include <memory>

#include "fdtd/line2d.hpp"
#include "vkcore/compute.hpp"

namespace fdtd_vk {

class GpuLine2d {
public:
  explicit GpuLine2d(vkcore::Context& ctx);
  ~GpuLine2d();
  // options.tolerance is the change over one batch (a float potential cannot settle below about 1e-7, so
  // anything smaller is raised to 1e-6; a change that stops improving below 1e-4 also ends the solve,
  // converged); options.maxIterations counts sweeps; omega 0 picks the model-problem optimum capped at 1.96
  // (float round-off makes the last few hundredths of the CPU's cap unstable). Jacobi is not offered: the
  // red-black sweeps are cheaper and converge faster.
  fdtd::line2d::Solution solve(const fdtd::line2d::Raster& raster,
                               const fdtd::line2d::SolveOptions& options = {});
  // For lineImpedance / lineImpedanceExtrapolated.
  fdtd::line2d::RasterSolver solver(const fdtd::line2d::SolveOptions& options = {});

  static constexpr std::uint32_t kSweepsPerBatch = 100; // each batch is one short submit

private:
  vkcore::Context& ctx_;
  std::unique_ptr<vkcore::ComputePipeline> pipeline_;
};

} // namespace fdtd_vk
