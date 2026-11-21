// BDD scenarios for field probes (docs/PLAN.md, Next steps 11h).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>

#include "app/probes.hpp"
#include "app/settings.hpp"
#include "core/units.hpp"
#include "fdtd/dft.hpp"
#include "fdtd/solver.hpp"

using Catch::Approx;
using namespace fdtd;

SCENARIO("Probe text is read as points in millimetres") {
  GIVEN("the accepted and refused forms") {
    THEN("semicolons and newlines separate probes and spaces are ignored") {
      const auto p = app::parseProbes("1, 2, 3; 4.5,5,-6\n7,8,9");
      REQUIRE(p.size() == 3);
      REQUIRE(p[1].xMm == Approx(4.5));
      REQUIRE(p[1].zMm == Approx(-6));
      REQUIRE(p[2].yMm == Approx(8));
    }
    THEN("empty text is no probes, and a wrong count, a bad number or too many are refused") {
      REQUIRE(app::parseProbes("  ").empty());
      REQUIRE_THROWS_AS(app::parseProbes("1,2"), app::SettingsError);
      REQUIRE_THROWS_AS(app::parseProbes("1,2,3,4"), app::SettingsError);
      REQUIRE_THROWS_AS(app::parseProbes("1,2,x"), app::SettingsError);
      std::string many;
      for (int i = 0; i < 17; ++i)
        many += "0,0,0;";
      REQUIRE_THROWS_AS(app::parseProbes(many), app::SettingsError);
    }
  }
}

SCENARIO("A probe finds the nearest node, with the board's offset and the bottom plane") {
  GIVEN("a uniform 1 mm grid whose node (0, 0) is at board (5, 7) mm and whose bottom copper plane is at "
        "z = 3 mm") {
    const auto grid = Grid::uniform({20, 20, 20}, {0.02, 0.02, 0.02});
    THEN("a probe at board (9.2, 11.8, 2.1) mm lands on node (4, 5, 5)") {
      const auto node = app::probeNode(grid, {9.2, 11.8, 2.1}, 5e-3, 7e-3, 3e-3);
      REQUIRE(node == grid.nodeIndex(4, 5, 5));
    }
  }
}

SCENARIO("Probes in a PEC cavity read the mode shape at resonance", "[slow]") {
  GIVEN("a 30 x 20 x 25 mm PEC cavity excited at its centre, and probes away from the source at (11, 10), "
        "(7, 10) "
        "and (9, 5) mm") {
    const double a = 0.030, b = 0.020, d = 0.025;
    const auto grid = Grid::uniform({30, 20, 25}, {a, b, d});
    Solver<double> s(grid, 0.99);
    auto pulse = [](double t) {
      const double x = (t - 6e-11) / 1.5e-11;
      return -x * std::exp(-x * x);
    };
    s.addSource({Z, 15, 10, 12, pulse});
    const double f110 = units::kSpeedOfLight / 2 * std::sqrt(std::pow(1 / a, 2) + std::pow(1 / b, 2));
    const std::vector<app::ProbeSpec> probes{{11, 10, 12}, {7, 10, 12}, {9, 5, 12}};
    std::vector<std::uint32_t> nodes;
    for (const auto& p : probes)
      nodes.push_back(app::probeNode(grid, p, 0, 0, 0));
    WHEN("the three probes' Ez spectra are taken over 8000 steps and read at the TM110 frequency") {
      std::vector<RunningDft<double>> dft(3, RunningDft<double>({f110}));
      for (int n = 0; n < 8000; ++n) {
        s.step();
        for (std::size_t p = 0; p < 3; ++p)
          dft[p].add(s.time(), s.field(Kind::E, Z)[nodes[p]]);
      }
      const double ref = std::abs(dft[0].at(0)); // x = 11 mm
      THEN("the ratios to the first probe follow the TM110 mode shape sin(pi x / a) sin(pi y / b), within "
           "1.5 %") {
        const double pi = std::numbers::pi;
        const double r1 = std::sin(pi * 7e-3 / a) / std::sin(pi * 11e-3 / a);
        const double r2 = std::sin(pi * 9e-3 / a) * std::sin(pi * 5e-3 / b) / std::sin(pi * 11e-3 / a);
        REQUIRE(ref > 0);
        REQUIRE(std::abs(dft[1].at(0)) / ref == Approx(r1).epsilon(0.015));
        REQUIRE(std::abs(dft[2].at(0)) / ref == Approx(r2).epsilon(0.015));
      }
    }
  }
}

SCENARIO("Probe spectra are written as CSV") {
  GIVEN("two probes and two frequencies") {
    const std::vector<app::ProbeSpec> probes{{1, 2, 3}, {4, 5, 6}};
    const std::vector<std::vector<std::complex<double>>> ph{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}};
    const auto csv = app::probesCsv({2.4e9, 2.5e9}, probes, ph);
    THEN("there is a header and one row per frequency with every real and imaginary part") {
      REQUIRE(
          csv.find(
              "f_hz,probe1_(1_2_3_mm)_re,probe1_(1_2_3_mm)_im,probe2_(4_5_6_mm)_re,probe2_(4_5_6_mm)_im\n") ==
          0);
      REQUIRE(csv.find("\n2400000000,1,2,5,6\n") != std::string::npos);
      REQUIRE(csv.find("\n2500000000,3,4,7,8\n") != std::string::npos);
    }
  }
}
