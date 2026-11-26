#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <complex>
#include <numbers>

#include "fdtd_vk/gpu_engine.hpp"
#include "fdtd_vk/gpu_solver.hpp"
#include "sim/simulation.hpp"

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

SCENARIO("The GPU phasor accumulator keeps its precision over many samples", "[gpu][phasor]") {
  GIVEN("a static Ez value and a sample list of one channel at one frequency") {
    vkcore::Context ctx({.validation = kValidation});
    std::array<std::vector<double>, 3> edges;
    for (auto& e : edges) {
      double x = 0;
      e.push_back(x);
      for (int i = 0; i < 24; ++i)
        e.push_back(x += 1e-3);
    }
    fdtd::Solver<float> cpu(fdtd::Grid(edges), 0.95);
    seedPulse(cpu, 12, 12, 11);
    fdtd_vk::GpuSolver gpu(ctx, cpu);
    const std::vector<fdtd_vk::PhasorChannel> channels{
        {fdtd::Kind::E, fdtd::Z, static_cast<std::uint32_t>(cpu.grid().nodeIndex(12, 12, 11))}};
    const std::size_t list = gpu.addPhasorList(channels, {4e9});

    WHEN("it adds the same sample 20000 times without stepping") {
      constexpr int kSamples = 20000;
      const double weight = 1e-3;
      for (int n = 0; n < kSamples; ++n)
        gpu.accumulate(list, weight);
      THEN("the sum is within 1e-5 of n times the weighted sample (a plain fp32 sum is off by about 1e-4)") {
        const double expected = kSamples * static_cast<double>(static_cast<float>(weight));
        const double got = gpu.phasor(list, 0, 0).real();
        INFO("expected " << expected << ", got " << got);
        REQUIRE(std::abs(got - expected) / expected < 1e-5);
      }
    }
  }
}

SCENARIO("The GPU phasor accumulator does not lose small samples added to a large sum", "[gpu][phasor]") {
  GIVEN("a static Ez value and a sample list of one channel at one frequency") {
    vkcore::Context ctx({.validation = kValidation});
    std::array<std::vector<double>, 3> edges;
    for (auto& e : edges) {
      double x = 0;
      e.push_back(x);
      for (int i = 0; i < 24; ++i)
        e.push_back(x += 1e-3);
    }
    fdtd::Solver<float> cpu(fdtd::Grid(edges), 0.95);
    seedPulse(cpu, 12, 12, 11);
    fdtd_vk::GpuSolver gpu(ctx, cpu);
    const std::vector<fdtd_vk::PhasorChannel> channels{
        {fdtd::Kind::E, fdtd::Z, static_cast<std::uint32_t>(cpu.grid().nodeIndex(12, 12, 11))}};
    const std::size_t list = gpu.addPhasorList(channels, {4e9});

    WHEN("it adds one sample of 1e8, then 30000 samples of 1, without stepping") {
      constexpr int kSmall = 30000;
      gpu.accumulate(list, 1e8);
      for (int n = 0; n < kSmall; ++n)
        gpu.accumulate(list, 1.0);
      THEN("the sum is within 1e-6 of 1e8 + 30000 (a plain fp32 sum stays at 1e8, whose spacing is 8)") {
        const double expected = 1e8 + kSmall;
        const double got = gpu.phasor(list, 0, 0).real();
        INFO("expected " << expected << ", got " << got);
        REQUIRE(std::abs(got - expected) / expected < 1e-6);
      }
    }
  }
}

SCENARIO("The GPU and CPU engines report the same phasors", "[gpu][phasor]") {
  GIVEN("the same pulsed cavity run on both engines, accumulating E and H channels every 10 steps") {
    vkcore::Context ctx({.validation = kValidation});
    std::array<std::vector<double>, 3> edges;
    for (auto& e : edges) {
      double x = 0;
      e.push_back(x);
      for (int i = 0; i < 20; ++i)
        e.push_back(x += 1e-3 * std::pow(1.03, i));
    }
    fdtd::Solver<float> a(fdtd::Grid(edges), 0.95), b(fdtd::Grid(edges), 0.95);
    seedPulse(a, 10, 10, 9);
    seedPulse(b, 10, 10, 9);
    auto cpu = sim::makeCpuEngine(a);
    fdtd_vk::GpuEngine gpu(ctx, b);
    std::vector<fdtd::PhasorChannel> channels;
    for (std::uint32_t n = 0; n < 300; ++n)
      channels.push_back(
          {n % 2 ? fdtd::Kind::E : fdtd::Kind::H, static_cast<fdtd::Axis>(n % 3),
           static_cast<std::uint32_t>(a.grid().nodeIndex(1 + n % 18, 1 + (n * 7) % 18, 1 + (n * 5) % 18))});
    const std::vector<double> freqs{5e9, 11e9};
    const auto lc = cpu->addPhasorList(channels, freqs), lg = gpu.addPhasorList(channels, freqs);
    WHEN("both run 300 steps") {
      for (int s = 0; s < 30; ++s) {
        cpu->advance(10);
        gpu.advance(10);
        cpu->accumulate(lc, 10 * a.dt());
        gpu.accumulate(lg, 10 * b.dt());
      }
      THEN("the phasors agree within 1e-3 of the peak (the fields differ by float rounding)") {
        double peak = 0, worst = 0;
        for (std::size_t f = 0; f < freqs.size(); ++f)
          for (std::size_t c = 0; c < channels.size(); ++c) {
            peak = std::max(peak, std::abs(cpu->phasor(lc, f, c)));
            worst = std::max(worst, std::abs(cpu->phasor(lc, f, c) - gpu.phasor(lg, f, c)));
          }
        REQUIRE(peak > 0);
        REQUIRE(worst / peak < 1e-3);
      }
    }
  }
}

SCENARIO("The GPU solver matches the CPU reference with a lumped R, L, C part on an edge", "[gpu][lumped]") {
  GIVEN("a graded 20-cell PEC box, a 50 ohm port driven by a pulse and a bead-like parallel 600 ohm, 5 nH, "
        "0.5 pF part on another edge") {
    vkcore::Context ctx({.validation = kValidation});
    std::array<std::vector<double>, 3> edges;
    for (auto& e : edges) {
      double x = 0;
      e.push_back(x);
      for (int i = 0; i < 20; ++i)
        e.push_back(x += 1e-3 * std::pow(1.02, i));
    }
    fdtd::Solver<float> cpu(fdtd::Grid(edges), 0.95);
    cpu.addLumpedPort({10, 10, 9, fdtd::Z, 50.0, [](double t) {
                         const double tau = 2e-11, u = (t - 4 * tau) / tau;
                         return -u * std::exp(-u * u);
                       }});
    fdtd::LumpedPort part{6, 10, 9, fdtd::Z, 600.0, [](double) { return 0.0; }};
    part.inductance = 5e-9;
    part.capacitance = 0.5e-12;
    cpu.addLumpedPort(part);
    fdtd_vk::GpuSolver gpu(ctx, cpu);
    WHEN("both advance 400 steps") {
      for (int n = 0; n < 400; ++n)
        cpu.step();
      gpu.step(400);
      THEN("every field value agrees within 1e-3 of the peak (float rounding), and the part's own voltage "
           "within 1e-3 relative") {
        std::string where;
        const double diff = relativeDifference(cpu, gpu, &where);
        INFO("worst: " << where);
        REQUIRE(diff < 1e-3);
        const double cpuV = cpu.e(fdtd::Z, 6, 10, 9) * (edges[2][10] - edges[2][9]);
        REQUIRE(std::abs(cpuV) > 0);
        REQUIRE(std::abs(gpu.portVoltage(1).back() - cpuV) <= 1e-3 * std::abs(cpuV) + 1e-12);
      }
      AND_THEN("the validation layer reported no errors") {
        REQUIRE(ctx.validationErrors() == 0);
      }
    }
  }
}

SCENARIO("The GPU accumulator really is compensated: 20 000 small terms added to a large one are kept",
         "[gpu][phasor]") {
  GIVEN("a field node held at 1.0, a phasor at 0 Hz (cos 1, sin 0), and no time stepping") {
    vkcore::Context ctx({.validation = kValidation});
    const auto grid = fdtd::Grid::uniform({6, 6, 6}, {6e-3, 6e-3, 6e-3});
    fdtd::Solver<float> cpuSolver(grid, 0.95);
    seedPulse(cpuSolver, 3, 3, 3);
    fdtd_vk::GpuSolver gpu(ctx, cpuSolver);
    const std::vector<fdtd::PhasorChannel> channels{
        {fdtd::Kind::E, fdtd::Z, static_cast<std::uint32_t>(grid.nodeIndex(3, 3, 3))}};
    const auto list = gpu.addPhasorList(channels, {0.0});
    WHEN("one sample with weight 1e8 is accumulated, then 20 000 samples with weight 1") {
      gpu.accumulate(list, 1e8);
      for (int n = 0; n < 20000; ++n)
        gpu.accumulate(list, 1.0);
      THEN("the phasor is 1.0002e8 within 1e-6 relative (a plain float sum stays at 1e8: its spacing "
           "there is 8, so each added 1 is lost)") {
        const double expect = 1.0002e8;
        const double got = gpu.phasor(list, 0, 0).real();
        INFO("got " << got << ", expected " << expect << ", plain float sum would give 1e8");
        REQUIRE(std::abs(got - expect) <= 1e-6 * expect);
        REQUIRE(std::abs(gpu.phasor(list, 0, 0).imag()) <= 1e-6 * expect);
      }
      AND_THEN("the validation layer reported no errors") {
        REQUIRE(ctx.validationErrors() == 0);
      }
    }
  }
}

SCENARIO("Staged device memory (discrete-GPU path) gives the same result as the unified path",
         "[gpu][staged]") {
  GIVEN("the same CPML box with a pulsed port and a phasor list, run on a unified and on a staged context") {
    vkcore::Context unified({.validation = kValidation});
    vkcore::Context staged({.validation = kValidation, .stagedBuffers = true});
    REQUIRE_FALSE(staged.unifiedMemory());
    std::array<std::vector<double>, 3> edges;
    for (auto& e : edges) {
      double x = 0;
      e.push_back(x);
      for (int i = 0; i < 28; ++i)
        e.push_back(x += 1e-3 * std::pow(1.02, i));
    }
    fdtd::Solver<float> cpu(fdtd::Grid(edges), 0.95);
    cpu.enableCpml({.cells = 4});
    cpu.addLumpedPort({14, 14, 13, fdtd::Z, 50.0, [](double t) {
                         const double tau = 2e-11, u = (t - 4 * tau) / tau;
                         return -u * std::exp(-u * u);
                       }});
    fdtd_vk::GpuSolver a(unified, cpu), b(staged, cpu);
    const std::vector<fdtd::PhasorChannel> channels = {
        {fdtd::Kind::E, fdtd::Z, static_cast<std::uint32_t>(cpu.grid().nodeIndex(14, 14, 13))},
        {fdtd::Kind::H, fdtd::X, static_cast<std::uint32_t>(cpu.grid().nodeIndex(10, 12, 12))}};
    const std::vector<double> freqs = {2.0e9, 3.1e9};
    const auto la = a.addPhasorList(channels, freqs), lb = b.addPhasorList(channels, freqs);

    WHEN("both advance 150 steps (more than two submits), sampling the phasors every 5 steps") {
      for (int n = 0; n < 150; n += 5) {
        a.step(5);
        b.step(5);
        a.accumulate(la, 5 * cpu.dt());
        b.accumulate(lb, 5 * cpu.dt());
      }
      THEN("every field value is identical (same kernels, same arithmetic)") {
        for (auto kind : {fdtd::Kind::E, fdtd::Kind::H})
          for (fdtd::Axis ax : {fdtd::X, fdtd::Y, fdtd::Z}) {
            const auto x = a.field(kind, ax), y = b.field(kind, ax);
            REQUIRE(std::equal(x.begin(), x.end(), y.begin(), y.end()));
          }
      }
      AND_THEN("the logged port voltage and the phasors are identical") {
        REQUIRE(a.portVoltage(0) == b.portVoltage(0));
        for (std::size_t k = 0; k < freqs.size(); ++k)
          for (std::size_t c = 0; c < channels.size(); ++c)
            REQUIRE(a.phasor(la, k, c) == b.phasor(lb, k, c));
      }
      AND_THEN("the fields moved (the comparison is not of two zero arrays) and no validation errors") {
        double peak = 0;
        for (float v : b.field(fdtd::Kind::E, fdtd::Z))
          peak = std::max(peak, std::abs(double(v)));
        REQUIRE(peak > 0);
        REQUIRE(staged.validationErrors() == 0);
      }
    }
  }
}
