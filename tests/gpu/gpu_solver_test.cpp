#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <complex>
#include <numbers>

#include "fdtd_vk/gpu_solver.hpp"

namespace {

#ifdef FDTD_VALIDATION
constexpr bool kValidation = true;
#else
constexpr bool kValidation = false;
#endif

// Largest |gpu - cpu| over all six field arrays, relative to the largest field value. `where` names the
// array and node of the worst difference (to tell operation-order noise in a slab from a port bug).
double relativeDifference(const fdtd::Solver<float>& cpu, const fdtd_vk::GpuSolver& gpu,
                          std::string* where = nullptr) {
  double maxDiff = 0, maxVal = 0;
  std::string worst;
  for (auto kind : {fdtd::Kind::E, fdtd::Kind::H})
    for (fdtd::Axis a : {fdtd::X, fdtd::Y, fdtd::Z}) {
      const auto c = cpu.field(kind, a);
      const auto g = gpu.field(kind, a);
      for (std::size_t i = 0; i < c.size(); ++i) {
        const double d = std::abs(double(c[i]) - double(g[i]));
        if (d > maxDiff) {
          maxDiff = d;
          worst = std::string(kind == fdtd::Kind::E ? "E" : "H") + "xyz"[a] + " node " + std::to_string(i) +
                  " cpu " + std::to_string(c[i]) + " gpu " + std::to_string(g[i]);
        }
        maxVal = std::max(maxVal, std::abs(double(c[i])));
      }
    }
  if (where)
    *where = worst + " (peak " + std::to_string(maxVal) + ")";
  return maxVal > 0 ? maxDiff / maxVal : maxDiff;
}

void seedPulse(fdtd::Solver<float>& s, std::size_t i, std::size_t j, std::size_t k) {
  s.mutableField(fdtd::Kind::E, fdtd::Z)[s.grid().nodeIndex(i, j, k)] = 1.0f;
}

} // namespace

SCENARIO("The GPU solver matches the CPU reference", "[gpu]") {
  GIVEN("a PEC cavity with an initial Ez pulse, on a uniform and on a graded grid") {
    vkcore::Context ctx({.validation = kValidation});
    const bool graded = GENERATE(false, true);
    std::array<std::vector<double>, 3> edges;
    for (int a = 0; a < 3; ++a) {
      double x = 0;
      edges[a].push_back(x);
      for (int i = 0; i < 24; ++i) {
        // graded: cells grow by 4% each, so neighbouring cell sizes differ
        x += 1e-3 * (graded ? std::pow(1.04, i) : 1.0);
        edges[a].push_back(x);
      }
    }
    fdtd::Solver<float> cpu(fdtd::Grid(edges), 0.95);
    seedPulse(cpu, 12, 12, 11);
    fdtd_vk::GpuSolver gpu(ctx, cpu);

    WHEN("both advance 200 steps") {
      for (int n = 0; n < 200; ++n)
        cpu.step();
      gpu.step(200);
      THEN("every field value agrees within 1e-4 of the peak (float rounding only)") {
        INFO(std::string(graded ? "graded" : "uniform") + " grid");
        REQUIRE(relativeDifference(cpu, gpu) < 1e-4);
      }
      AND_THEN("the validation layer reported no errors") {
        REQUIRE(ctx.validationErrors() == 0);
      }
    }
  }
}

SCENARIO("The GPU solver handles several materials through the palette index", "[gpu]") {
  GIVEN("a cavity holding a dielectric block, a lossy block and a PEC post") {
    vkcore::Context ctx({.validation = kValidation});
    std::array<std::vector<double>, 3> edges;
    for (auto& e : edges)
      for (int i = 0; i <= 24; ++i)
        e.push_back(1e-3 * i);
    fdtd::Solver<float> cpu(fdtd::Grid(edges), 0.95);
    std::vector<std::uint8_t> cellMat(24 * 24 * 24, 0);
    auto at = [](int i, int j, int k) { return static_cast<std::size_t>(i + 24 * (j + 24 * k)); };
    for (int k = 4; k < 10; ++k)
      for (int j = 4; j < 10; ++j)
        for (int i = 4; i < 10; ++i)
          cellMat[at(i, j, k)] = 1;
    for (int k = 14; k < 20; ++k)
      for (int j = 14; j < 20; ++j)
        for (int i = 14; i < 20; ++i)
          cellMat[at(i, j, k)] = 2;
    cellMat[at(12, 12, 4)] = 3;
    cpu.setMaterials({{1.0, 0.0, false}, {4.4, 0.0, false}, {1.0, 5.0, false}, {1.0, 0.0, true}}, cellMat);
    seedPulse(cpu, 12, 12, 11);
    fdtd_vk::GpuSolver gpu(ctx, cpu);

    WHEN("both advance 200 steps") {
      for (int n = 0; n < 200; ++n)
        cpu.step();
      gpu.step(200);
      THEN("fields agree within 1e-4 of the peak") {
        REQUIRE(relativeDifference(cpu, gpu) < 1e-4);
      }
      AND_THEN("the palette holds far fewer entries than nodes") {
        REQUIRE(gpu.paletteMaterials() > 3);
        REQUIRE(gpu.paletteMaterials() <= 256);
      }
      AND_THEN("the validation layer reported no errors") {
        REQUIRE(ctx.validationErrors() == 0);
      }
    }
  }
}

SCENARIO("The GPU solver matches the CPU reference with CPML and a lumped port", "[gpu]") {
  GIVEN("a graded 28-cell box with a 4-cell CPML on every wall and a 50 ohm port driven by a pulse") {
    vkcore::Context ctx({.validation = kValidation});
    std::array<std::vector<double>, 3> edges;
    for (auto& e : edges) {
      double x = 0;
      e.push_back(x);
      for (int i = 0; i < 28; ++i)
        e.push_back(x += 1e-3 * std::pow(1.02, i));
    }
    fdtd::Solver<float> cpu(fdtd::Grid(edges), 0.95);
    const int variant = GENERATE(0, 1, 2); // 0: both, 1: CPML only (seeded pulse), 2: port only
    INFO("variant " << variant << " (0 both, 1 CPML only, 2 port only)");
    if (variant != 2)
      cpu.enableCpml({.cells = 4});
    if (variant != 1)
      cpu.addLumpedPort({14, 14, 13, fdtd::Z, 50.0, [](double t) {
                           const double tau = 2e-11, u = (t - 4 * tau) / tau;
                           return -u * std::exp(-u * u);
                         }});
    else
      seedPulse(cpu, 14, 14, 13);
    fdtd_vk::GpuSolver gpu(ctx, cpu);

    WHEN("both advance 150 steps (more than one submit)") {
      for (int n = 0; n < 150; ++n)
        cpu.step();
      gpu.step(150);
      THEN("every field value agrees within 1e-4 of the peak (1e-3 with both: the CPML has absorbed most "
           "of the port's field by step 150, so the same float noise is a larger share of what is left)") {
        std::string where;
        const double diff = relativeDifference(cpu, gpu, &where);
        INFO("worst: " << where);
        REQUIRE(diff < (variant == 0 ? 1e-3 : 1e-4));
      }
      if (variant != 1) {
        AND_THEN("the logged port voltage matches the CPU's last port voltage within 1e-3 relative") {
          const double cpuV = cpu.e(fdtd::Z, 14, 14, 13) * (edges[2][14] - edges[2][13]);
          REQUIRE(gpu.portVoltage(0).size() == 150);
          REQUIRE(std::abs(gpu.portVoltage(0).back() - cpuV) <= 1e-3 * std::abs(cpuV) + 1e-12);
        }
      }
      AND_THEN("the validation layer reported no errors") {
        REQUIRE(ctx.validationErrors() == 0);
      }
    }
  }
}

SCENARIO("The GPU phasor accumulator matches a double-precision DFT of the same samples", "[gpu][phasor]") {
  GIVEN("a graded cavity with a pulse, and sample lists holding E and H channels at scattered nodes") {
    vkcore::Context ctx({.validation = kValidation});
    std::array<std::vector<double>, 3> edges;
    for (auto& e : edges) {
      double x = 0;
      e.push_back(x);
      for (int i = 0; i < 24; ++i)
        e.push_back(x += 1e-3 * std::pow(1.03, i));
    }
    fdtd::Solver<float> cpu(fdtd::Grid(edges), 0.95);
    seedPulse(cpu, 12, 12, 11);
    fdtd_vk::GpuSolver gpu(ctx, cpu);
    const fdtd::Grid& g = cpu.grid();
    std::vector<fdtd_vk::PhasorChannel> channels;
    for (std::uint32_t n = 0; n < 700; ++n) {
      const std::size_t i = 1 + n % 22, j = 1 + (n * 7) % 22, k = 1 + (n * 13) % 22;
      channels.push_back({n % 2 ? fdtd::Kind::E : fdtd::Kind::H, static_cast<fdtd::Axis>(n % 3),
                          static_cast<std::uint32_t>(g.nodeIndex(i, j, k))});
    }
    const std::vector<double> freqs{4e9, 9e9, 15e9};
    const std::size_t list = gpu.addPhasorList(channels, freqs);

    WHEN("it accumulates every 10 steps for 600 steps") {
      const std::uint32_t every = 10;
      const double weight = every * cpu.dt();
      std::vector<std::complex<double>> ref(freqs.size() * channels.size());
      for (std::uint32_t s = 0; s < 60; ++s) {
        gpu.step(every);
        gpu.accumulate(list, weight);
        const double tE = static_cast<double>(gpu.stepsDone()) * cpu.dt();
        for (std::size_t c = 0; c < channels.size(); ++c) {
          const auto& ch = channels[c];
          const double v = gpu.field(ch.kind, ch.component)[ch.node];
          const double t = ch.kind == fdtd::Kind::E ? tE : tE - 0.5 * cpu.dt();
          for (std::size_t f = 0; f < freqs.size(); ++f)
            ref[f * channels.size() + c] +=
                v * weight * std::polar(1.0, -2 * std::numbers::pi * freqs[f] * t);
        }
      }
      THEN("every phasor agrees within 1e-4 of the peak magnitude") {
        double peak = 0, worst = 0;
        for (const auto& r : ref)
          peak = std::max(peak, std::abs(r));
        for (std::size_t f = 0; f < freqs.size(); ++f)
          for (std::size_t c = 0; c < channels.size(); ++c)
            worst = std::max(worst, std::abs(gpu.phasor(list, f, c) - ref[f * channels.size() + c]));
        INFO("peak " << peak << ", worst difference " << worst);
        REQUIRE(peak > 0);
        REQUIRE(worst / peak < 1e-4);
      }
      AND_THEN("the validation layer reported no errors") {
        REQUIRE(ctx.validationErrors() == 0);
      }
    }
  }
}
