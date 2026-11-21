// Numerical BDD scenarios for the absorbing boundary (CPML) and the lumped port of the CPU reference.
#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <numbers>

#include "fdtd/grid.hpp"
#include "fdtd/solver.hpp"
#include "fdtd/sparam.hpp"
#include "sim/simulation.hpp"

using Catch::Approx;
using namespace fdtd;

namespace {

// Derivative-of-Gaussian pulse: no DC content, so nothing is left trapped in the box.
double pulse(double t) {
  const double tau = 2e-11, u = (t - 4 * tau) / tau;
  return -u * std::exp(-u * u);
}

// Energy left after `steps` steps.
double finalEnergy(Solver<double>& s, int steps) {
  for (int n = 0; n < steps; ++n)
    s.step();
  return s.energy();
}

// Energy a closed PEC box of the same grid keeps for ever: the soft source's two lobes partly cancel, so the
// reference for "absorbed" is what a lossless box retains, not the mid-pulse peak.
double pecReference(const Grid& grid, int steps) {
  Solver<double> s(grid, 0.95);
  s.addSource({Z, 20, 20, 20, pulse});
  return finalEnergy(s, steps);
}

} // namespace

SCENARIO("The CPML absorbs outgoing waves", "[slow]") {
  GIVEN("a 40 mm cube with 1 mm cells and a pulse at its centre") {
    const auto grid = Grid::uniform({40, 40, 40}, {0.04, 0.04, 0.04});
    const double kept = pecReference(grid, 600);
    REQUIRE(kept > 0);
    WHEN("an 8-cell CPML backs every wall") {
      Solver<double> s(grid, 0.95);
      s.enableCpml({});
      s.addSource({Z, 20, 20, 20, pulse});
      THEN("less than 1e-6 (-60 dB) of the PEC-box energy is left after 600 steps") {
        REQUIRE(finalEnergy(s, 600) < 1e-6 * kept);
      }
    }
    WHEN("the CPML is used on a graded grid (cells grow 3 % per cell toward the high walls)") {
      std::array<std::vector<double>, 3> edges;
      for (auto& e : edges) {
        double x = 0;
        e.push_back(x);
        for (int i = 0; i < 40; ++i)
          e.push_back(x += 1e-3 * std::pow(1.03, i));
      }
      const double gradedKept = pecReference(Grid(edges), 600);
      Solver<double> s{Grid(edges), 0.95};
      s.enableCpml({});
      s.addSource({Z, 20, 20, 20, pulse});
      THEN("less than 1e-6 of the PEC-box energy (same graded grid) is left after 600 steps") {
        REQUIRE(finalEnergy(s, 600) < 1e-6 * gradedKept);
      }
    }
  }
}

SCENARIO("A lumped resistive port conserves energy", "[slow]") {
  GIVEN("a 24 mm PEC cavity with a 50 ohm port (source pulse in series) across one Ez edge") {
    const auto grid = Grid::uniform({24, 24, 24}, {0.024, 0.024, 0.024});
    Solver<double> s(grid, 0.95);
    constexpr double kR = 50.0;
    s.addLumpedPort({12, 12, 11, Z, kR, [](double t) { return pulse(t); }});
    WHEN("the pulse has been delivered and the cavity then rings for 800 steps") {
      double delivered = 0, dissipated = 0;
      for (int n = 0; n < 800; ++n) {
        s.step();
        const double i = s.portCurrent(0), dt = s.dt();
        delivered += s.portSourceVoltage(0) * i * dt;
        dissipated += i * i * kR * dt;
      }
      THEN("stored energy equals delivered minus dissipated, within 6 %") {
        const double expected = delivered - dissipated;
        REQUIRE(expected > 0);
        REQUIRE(s.energy() == Approx(expected).epsilon(0.06));
      }
      AND_THEN("the resistor absorbed a real share of the pulse (the port is not an open circuit)") {
        REQUIRE(dissipated > 0.05 * delivered);
      }
    }
  }
}

SCENARIO("S11 is extracted from the port voltage and source", "[sparam]") {
  GIVEN("a synthetic port record: V = (Vs + G*Vs(t - tau)) / 2 with G = 0.5 and tau = 100 ps") {
    constexpr double dt = 2e-12, tau = 100e-12, gamma = 0.5;
    const auto source = [](double t) { return pulse(t); };
    std::vector<double> v, vs;
    for (int n = 0; n < 4000; ++n) {
      const double tm = (n + 0.5) * dt, t1 = (n + 1.0) * dt;
      vs.push_back(source(tm));
      v.push_back(0.5 * (source(t1) + gamma * source(t1 - tau)));
    }
    WHEN("S11 is computed at 2, 3 and 4 GHz (inside the pulse spectrum)") {
      const auto s = s11FromPort(v, vs, dt, {2e9, 3e9, 4e9}, 50.0);
      THEN("it equals G * exp(-j 2 pi f tau) within 1e-3 (the half-step offset of Vs is handled)") {
        for (std::size_t i = 0; i < 3; ++i) {
          const auto expected = std::polar(gamma, -2 * 3.14159265358979323846 * s.frequenciesHz[i] * tau);
          REQUIRE(std::abs(s.s11[i] - expected) < 1e-3);
        }
      }
    }
  }
}

SCENARIO("A lumped capacitor on an edge gives the analytic RC response", "[slow][lumped]") {
  GIVEN("a source of 50 ohm driving one Ez edge that also carries 10 pF, in a small PEC cavity") {
    const auto grid = Grid::uniform({24, 24, 24}, {0.024, 0.024, 0.024});
    Solver<double> s(grid, 0.95);
    constexpr double kR = 50.0, kC = 10e-12;
    const auto drive = sim::bandPulse(0.1e9, 1.5e9);
    LumpedPort port{12, 12, 11, Z, kR, drive};
    port.capacitance = kC;
    s.addLumpedPort(std::move(port));
    WHEN("the pulse is run in and V(edge) / Vs is taken at 318 MHz and at 1 GHz") {
      const double dl = grid.cellSize(Z, 11);
      std::vector<std::complex<double>> v(2), vs(2);
      const double f[2] = {1 / (2 * std::numbers::pi * kR * kC), 1e9};
      for (int n = 0; n < 6000; ++n) {
        s.step();
        const double t = s.time(), dt = s.dt();
        for (int q = 0; q < 2; ++q) {
          const auto w = std::polar(dt, -2 * std::numbers::pi * f[q] * t);
          v[q] += s.e(Z, 12, 12, 11) * dl * w;
          vs[q] += s.portSourceVoltage(0) * w;
        }
      }
      THEN("the ratio is 1 / (1 + j w R C) within 4 % in magnitude and 0.04 rad in phase (the cell's own "
           "permittivity and the cavity add a little)") {
        for (int q = 0; q < 2; ++q) {
          const std::complex<double> expect =
              1.0 / (1.0 + std::complex<double>(0, 2 * std::numbers::pi * f[q] * kR * kC));
          const auto got = v[q] / vs[q];
          CHECK(std::abs(got) == Approx(std::abs(expect)).epsilon(0.04));
          CHECK(std::abs(std::arg(got) - std::arg(expect)) < 0.04);
        }
      }
    }
  }
}
