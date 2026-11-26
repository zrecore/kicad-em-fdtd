#include "fdtd_vk/gpu_line2d.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "core/resources.hpp"
#include "shared/fdtd_params.h"
#include "vkcore/buffer.hpp"

namespace fdtd_vk {

GpuLine2d::GpuLine2d(vkcore::Context& ctx)
    : ctx_(ctx), pipeline_(std::make_unique<vkcore::ComputePipeline>(
                     ctx, core::resourceDir("shaders", FDTD_SHADER_DIR) + "/line2d_rb.comp.spv", 4,
                     sizeof(fdtd_gpu::Line2dParams))) {}
GpuLine2d::~GpuLine2d() = default;

fdtd::line2d::Solution GpuLine2d::solve(const fdtd::line2d::Raster& r,
                                        const fdtd::line2d::SolveOptions& options) {
  const std::size_t n = r.nx * r.ny;
  vkcore::Buffer gx(ctx_, n * sizeof(float)), gy(ctx_, n * sizeof(float)),
      held(ctx_, n * sizeof(std::uint32_t)), phi(ctx_, n * sizeof(float));
  auto fgx = gx.as<float>(), fgy = gy.as<float>(), fphi = phi.as<float>();
  auto fheld = held.as<std::uint32_t>();
  for (std::size_t k = 0; k < n; ++k) {
    fgx[k] = static_cast<float>(r.gx[k]);
    fgy[k] = static_cast<float>(r.gy[k]);
    fheld[k] = r.fixed[k];
    fphi[k] = r.fixed[k] ? static_cast<float>(r.value[k]) : 0.0f;
  }
  double omega = options.omega;
  if (omega <= 0)
    omega =
        std::min(1.96, 2.0 / (1.0 + std::sin(std::numbers::pi / static_cast<double>(std::max(r.nx, r.ny)))));
  const double tolerance = std::max(options.tolerance, 1e-6);
  const auto groups = static_cast<std::uint32_t>(std::min<std::size_t>((n + 255) / 256, 4096));
  fdtd_gpu::Line2dParams red{static_cast<std::uint32_t>(r.nx), static_cast<std::uint32_t>(r.ny), 0,
                             static_cast<float>(omega)};
  fdtd_gpu::Line2dParams black = red;
  black.colour = 1;
  const void* pushes[2] = {&red, &black};
  vkcore::Buffer* buffers[4] = {&gx, &gy, &held, &phi};

  fdtd::line2d::Solution out;
  out.nx = r.nx;
  out.ny = r.ny;
  out.cell = r.h;
  std::vector<float> before(n);
  double best = 1e30;
  int stalled = 0;
  while (out.iterations < options.maxIterations) {
    std::copy(fphi.begin(), fphi.end(), before.begin());
    pipeline_->runSequence(buffers, pushes, groups, kSweepsPerBatch);
    out.iterations += kSweepsPerBatch;
    double change = 0;
    for (std::size_t k = 0; k < n; ++k)
      change = std::max(change, std::abs(static_cast<double>(fphi[k]) - static_cast<double>(before[k])));
    out.change = change;
    if (change < tolerance) {
      out.converged = true;
      break;
    }
    // A float potential stops improving at its round-off floor, which over-relaxation raises above the
    // tolerance: five batches without a new best change, all below 1e-4, count as settled.
    if (change < 0.9 * best) {
      best = change;
      stalled = 0;
    } else if (best < 1e-4 && ++stalled >= 5) {
      out.converged = true;
      break;
    }
  }
  out.phi.assign(fphi.begin(), fphi.end());
  out.capacitance = fdtd::line2d::energyCapacitance(r, out.phi);
  return out;
}

fdtd::line2d::RasterSolver GpuLine2d::solver(const fdtd::line2d::SolveOptions& options) {
  return [this, options](const fdtd::line2d::Raster& r) { return solve(r, options); };
}

} // namespace fdtd_vk
