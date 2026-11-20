#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <numbers>

#include "sim/simulation.hpp"

SCENARIO("The CPU engine's phasor lists match a DFT of the recorded fields", "[phasor]") {
  GIVEN("a graded cavity with a pulse and a list of E and H channels") {
    std::array<std::vector<double>, 3> edges;
    for (auto& e : edges) {
      double x = 0;
      e.push_back(x);
      for (int i = 0; i < 16; ++i)
        e.push_back(x += 1e-3 * std::pow(1.03, i));
    }
    fdtd::Solver<float> solver(fdtd::Grid(edges), 0.95);
    solver.mutableField(fdtd::Kind::E, fdtd::Z)[solver.grid().nodeIndex(8, 8, 7)] = 1.0f;
    auto engine = sim::makeCpuEngine(solver);
    std::vector<fdtd::PhasorChannel> channels;
    for (std::uint32_t n = 0; n < 40; ++n)
      channels.push_back({n % 2 ? fdtd::Kind::E : fdtd::Kind::H, static_cast<fdtd::Axis>(n % 3),
                          static_cast<std::uint32_t>(
                              solver.grid().nodeIndex(1 + n % 14, 1 + (n * 5) % 14, 1 + (n * 3) % 14))});
    const std::vector<double> freqs{3e9, 8e9};
    const auto list = engine->addPhasorList(channels, freqs);

    WHEN("it accumulates every 8 steps for 160 steps") {
      std::vector<std::complex<double>> ref(freqs.size() * channels.size());
      const double dt = solver.dt(), weight = 8 * dt;
      for (int s = 0; s < 20; ++s) {
        engine->advance(8);
        engine->accumulate(list, weight);
        for (std::size_t c = 0; c < channels.size(); ++c) {
          const auto& ch = channels[c];
          const double t = engine->time() - (ch.kind == fdtd::Kind::E ? 0.0 : 0.5 * dt);
          for (std::size_t f = 0; f < freqs.size(); ++f)
            ref[f * channels.size() + c] += double(engine->field(ch.kind, ch.component)[ch.node]) * weight *
                                            std::polar(1.0, -2 * std::numbers::pi * freqs[f] * t);
        }
      }
      THEN("every phasor agrees with the reference to 1e-9 of the peak (same samples, same arithmetic)") {
        double peak = 0, worst = 0;
        for (const auto& r : ref)
          peak = std::max(peak, std::abs(r));
        for (std::size_t f = 0; f < freqs.size(); ++f)
          for (std::size_t c = 0; c < channels.size(); ++c)
            worst = std::max(worst, std::abs(engine->phasor(list, f, c) - ref[f * channels.size() + c]));
        REQUIRE(peak > 0);
        REQUIRE(worst / peak < 1e-9);
      }
      AND_THEN("the bulk copy returns the same values in the accumulator's layout (k * channels + c)") {
        std::vector<float> re(freqs.size() * channels.size()), im(re.size());
        engine->copyPhasors(list, re, im);
        for (std::size_t f = 0; f < freqs.size(); ++f)
          for (std::size_t c = 0; c < channels.size(); ++c) {
            const auto x = engine->phasor(list, f, c);
            REQUIRE(std::abs(
                        x - std::complex<double>(re[f * channels.size() + c], im[f * channels.size() + c])) <=
                    1e-6 * (std::abs(x) + 1e-30));
          }
      }
    }
  }
}
