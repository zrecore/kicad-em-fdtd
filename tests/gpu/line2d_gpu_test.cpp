// BDD scenarios for the GPU 2D cross-section solve: it must give the capacitance and impedance the double
// precision CPU reference gives (fdtd/line2d.hpp), in float32.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "fdtd/line2d.hpp"
#include "fdtd_vk/gpu_line2d.hpp"

using Catch::Approx;
namespace l2 = fdtd::line2d;

namespace {
constexpr double mm = 1e-3;
}

SCENARIO("The GPU 2D solve agrees with the CPU reference", "[gpu][line2d]") {
  vkcore::Context ctx;
  fdtd_vk::GpuLine2d gpu(ctx);
  GIVEN("a 1 mm microstrip on 0.5 mm of er 4.3, 0.05 mm cells") {
    const auto section = l2::microstripSection(1 * mm, 0.5 * mm, 4.3);
    WHEN("both backends solve the rasterised section with the dielectric and in air") {
      for (const bool air : {false, true}) {
        const l2::Raster r = l2::rasterise(section, 0.05 * mm, air);
        l2::SolveOptions cpuOptions;
        cpuOptions.tolerance = 1e-9;
        const l2::Solution cpu = l2::solveRaster(r, cpuOptions);
        const l2::Solution g = gpu.solve(r);
        THEN(std::string("the capacitance is within 0.1 % of double precision ") +
             (air ? "in air" : "with the dielectric")) {
          REQUIRE(cpu.converged);
          REQUIRE(g.converged);
          INFO("cpu " << cpu.capacitance << " gpu " << g.capacitance << ", sweeps cpu " << cpu.iterations
                      << " gpu " << g.iterations);
          REQUIRE(g.capacitance == Approx(cpu.capacitance).epsilon(1e-3));
        }
      }
    }
    WHEN("the impedance is taken through the GPU solver") {
      const l2::LineResult cpu = l2::lineImpedance(section, 0.05 * mm);
      const l2::LineResult g = l2::lineImpedance(section, 0.05 * mm, gpu.solver());
      THEN("it agrees with the CPU within 0.2 %, and is a real microstrip impedance") {
        INFO("cpu " << cpu.z0Ohm << " gpu " << g.z0Ohm);
        REQUIRE(g.converged);
        REQUIRE(g.z0Ohm == Approx(cpu.z0Ohm).epsilon(2e-3));
        REQUIRE(g.z0Ohm > 20);
        REQUIRE(g.z0Ohm < 120);
      }
    }
  }
  GIVEN("a coplanar line with 35 um copper and ground on one side only") {
    const auto section =
        l2::groundedCoplanarSection(0.8 * mm, 0.3 * mm, 0.5 * mm, 4.3, 0.035 * mm, true, false);
    THEN("the extrapolated impedance through the GPU agrees with the CPU within 0.5 %") {
      const l2::LineResult cpu = l2::lineImpedanceExtrapolated(section, 0.07 * mm);
      const l2::LineResult g = l2::lineImpedanceExtrapolated(section, 0.07 * mm, gpu.solver());
      INFO("cpu " << cpu.z0Ohm << " gpu " << g.z0Ohm << ", sweeps " << g.iterations);
      REQUIRE(g.z0Ohm == Approx(cpu.z0Ohm).epsilon(5e-3));
      REQUIRE(g.converged);
    }
  }
  GIVEN("a graded grid on the Nordic feed, 38.5 um of copper") {
    const auto section = l2::groundedCoplanarSection(0.762 * mm, 0.154 * mm, 1.44 * mm, 4.5, 0.0385 * mm);
    l2::GridOptions graded;
    graded.growth = 1.3;
    graded.maxCellFactor = 8;
    THEN("the GPU solve on it agrees with the CPU within 0.5 %, and is a 50 ohm line") {
      const l2::LineResult cpu =
          l2::lineImpedanceExtrapolated(section, 0.0385 * mm, l2::SolveOptions{}, graded);
      const l2::LineResult g = l2::lineImpedanceExtrapolated(section, 0.0385 * mm, gpu.solver(), graded);
      INFO("cpu " << cpu.z0Ohm << " gpu " << g.z0Ohm);
      REQUIRE(g.converged);
      REQUIRE(g.z0Ohm == Approx(cpu.z0Ohm).epsilon(5e-3));
      REQUIRE(g.z0Ohm == Approx(50.0).epsilon(0.03));
    }
  }
}

SCENARIO("Report: CPU and GPU time of an extrapolated solve", "[.report][gpu]") {
  vkcore::Context ctx;
  fdtd_vk::GpuLine2d gpu(ctx);
  const auto section = l2::groundedCoplanarSection(0.762 * mm, 0.154 * mm, 1.44 * mm, 4.5, 0.0385 * mm);
  l2::GridOptions graded;
  graded.growth = 1.3;
  graded.maxCellFactor = 8;
  for (const bool useGrid : {false, true}) {
    const l2::GridOptions grid = useGrid ? graded : l2::GridOptions{};
    const auto t0 = std::chrono::steady_clock::now();
    const l2::LineResult cpu = l2::lineImpedanceExtrapolated(section, 0.0385 * mm, l2::SolveOptions{}, grid);
    const auto t1 = std::chrono::steady_clock::now();
    const l2::LineResult g = l2::lineImpedanceExtrapolated(section, 0.0385 * mm, gpu.solver(), grid);
    const auto t2 = std::chrono::steady_clock::now();
    std::printf("%s grid: cpu %.2f ohm in %.2f s, gpu %.2f ohm in %.2f s\n", useGrid ? "graded" : "uniform",
                cpu.z0Ohm, std::chrono::duration<double>(t1 - t0).count(), g.z0Ohm,
                std::chrono::duration<double>(t2 - t1).count());
  }
}
