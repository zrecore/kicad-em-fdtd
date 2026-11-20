// Numerical BDD scenarios for the CPU reference solver. Each states its tolerance in the THEN.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "core/units.hpp"
#include "fdtd/dft.hpp"
#include "fdtd/grid.hpp"
#include "fdtd/solver.hpp"

using Catch::Approx;
using namespace fdtd;

SCENARIO("The time step is limited by the smallest cell (CFL)") {
  GIVEN("a uniform 1 mm cubic grid") {
    const auto grid = Grid::uniform({20, 20, 20}, {0.02, 0.02, 0.02});
    THEN("the stable step is dx / (c sqrt(3))") {
      REQUIRE(maxStableDt(grid) == Approx(1e-3 / (units::kSpeedOfLight * std::sqrt(3.0))).epsilon(1e-12));
    }
  }
  GIVEN("a graded grid whose z cells are five times finer") {
    const auto grid = Grid::uniform({10, 10, 40}, {0.01, 0.01, 0.01}); // dz = 0.25 mm, dx = dy = 1 mm
    THEN("the stable step is set by the finest axis, not the coarse ones") {
      const double dt = maxStableDt(grid);
      REQUIRE(dt < 1e-3 / (units::kSpeedOfLight * std::sqrt(3.0)));
      REQUIRE(dt == Approx(1.0 / (units::kSpeedOfLight * std::sqrt(1e6 + 1e6 + 16e6))).epsilon(1e-12));
    }
  }
}

SCENARIO("The scheme is stable below the CFL limit and unstable above it", "[slow]") {
  GIVEN("a PEC box excited by a short Gaussian pulse") {
    const auto grid = Grid::uniform({16, 16, 16}, {0.016, 0.016, 0.016});
    auto pulse = [](double t) { return std::exp(-std::pow((t - 4e-11) / 1e-11, 2)); };
    const SoftSource src{Z, 8, 8, 8, pulse};
    WHEN("the time step is 99% of the limit and the run is long") {
      Solver<double> s(grid, 0.99);
      s.addSource(src);
      for (int n = 0; n < 4000; ++n)
        s.step();
      THEN("the energy stays bounded (below 1e-12 J here)") {
        REQUIRE(s.energy() < 1e-12);
      }
    }
    WHEN("the time step is 110% of the limit") {
      Solver<double> s(grid, 1.0, 1.1 * maxStableDt(grid));
      s.addSource(src);
      for (int n = 0; n < 200; ++n)
        s.step();
      THEN("the energy is no longer bounded (huge, infinite or NaN)") {
        const double e = s.energy();
        REQUIRE_FALSE((std::isfinite(e) && e < 1e-6));
      }
    }
  }
}

SCENARIO("A PEC cavity rings at its analytic resonant frequencies", "[slow]") {
  GIVEN("a 30 x 20 x 25 mm PEC cavity with 1 mm cells, excited at its centre") {
    const double a = 0.030, b = 0.020, d = 0.025;
    const auto grid = Grid::uniform({30, 20, 25}, {a, b, d});
    Solver<double> s(grid, 0.99);
    // Derivative of a Gaussian: zero mean, so the source leaves no static (DC) field to leak into the
    // spectrum.
    auto pulse = [](double t) {
      const double x = (t - 6e-11) / 1.5e-11;
      return -x * std::exp(-x * x);
    };
    s.addSource({Z, 15, 10, 12, pulse}); // exact centre: only modes with Ez nonzero there can ring
    // Analytic TE101 / TM110 frequencies: f = c/2 sqrt((m/a)^2 + (n/b)^2 + (p/d)^2)
    auto f = [&](int m, int n, int p) {
      return units::kSpeedOfLight / 2 *
             std::sqrt(std::pow(m / a, 2) + std::pow(n / b, 2) + std::pow(p / d, 2));
    };
    const double f110 = f(1, 1, 0); // dominant mode with E_z nonzero
    WHEN("the Ez time series at the centre is Fourier transformed") {
      std::vector<double> freqs;
      for (double x = 0.85 * f110; x < 1.15 * f110; x += 5e6)
        freqs.push_back(x);
      RunningDft dft(freqs);
      for (int n = 0; n < 8000; ++n) {
        s.step();
        dft.add(s.time(), s.e(Z, 15, 10, 12));
      }
      THEN("the strongest peak is within 1% of the analytic TM110 frequency") {
        std::size_t best = 0;
        for (std::size_t i = 1; i < freqs.size(); ++i)
          if (std::abs(dft.at(i)) > std::abs(dft.at(best)))
            best = i;
        REQUIRE(freqs[best] == Approx(f110).epsilon(0.01));
      }
    }
  }
}

SCENARIO("Compensated summation keeps long accumulations accurate") {
  GIVEN("a million additions of 0.1 to a large running total") {
    RunningDft<float> dft(
        {0.0}); // f = 0: the transform is the plain sum; float is the GPU accumulator's twin
    dft.add(0.0, 1e8);
    for (int i = 0; i < 1'000'000; ++i)
      dft.add(0.0, 0.1);
    THEN("the compensated float sum is within 1e-6 relative (a naive float sum would be off by 1e-3)") {
      REQUIRE(dft.at(0).real() == Approx(1e8 + 1e5).epsilon(1e-6));
    }
  }
}
