// Numerical BDD scenarios for the absorbing boundary (CPML) and the lumped port of the CPU reference.
#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <limits>
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

SCENARIO("A lumped capacitor on an edge adds exactly its capacitance to the port's RC response",
         "[slow][lumped]") {
  GIVEN("a source of 50 ohm driving one Ez edge of a 24 mm PEC cavity (1 mm cells), carrying C") {
    constexpr double kR = 50.0;
    // The capacitance the port sees: 1 / (V / Vs) = 1 + j w R C_eff, taken at (n + 1/2) dt where the port
    // couples the mean of E^n and E^(n+1) to the source, over a run long enough to ring down (6000 steps).
    auto effective = [&](double capacitance, double f) {
      const auto grid = Grid::uniform({24, 24, 24}, {0.024, 0.024, 0.024});
      Solver<double> s(grid, 0.95);
      LumpedPort port{12, 12, 11, Z, kR, sim::bandPulse(0.1e9, 1.5e9)};
      port.capacitance = capacitance;
      s.addLumpedPort(std::move(port));
      const double dl = grid.cellSize(Z, 11);
      std::complex<double> v = 0, vs = 0;
      double eOld = 0;
      for (int n = 0; n < 6000; ++n) {
        s.step();
        const double t = s.time() - 0.5 * s.dt(), dt = s.dt();
        const double eNew = s.e(Z, 12, 12, 11);
        const auto w = std::polar(dt, -2 * std::numbers::pi * f * t);
        v += 0.5 * (eOld + eNew) * dl * w;
        vs += s.portSourceVoltage(0) * w;
        eOld = eNew;
      }
      return (1.0 / (v / vs)).imag() / (2 * std::numbers::pi * f * kR);
    };
    WHEN("C is 3 pF and 10 pF, at 318 MHz and 1 GHz, and also 0 pF") {
      const double c0 = effective(0, 1e9), c3 = effective(3e-12, 1e9), c10 = effective(10e-12, 1e9),
                   c10low = effective(10e-12, 318e6);
      THEN("the difference between the two is 7 pF within 0.05 % (measured 0.001 %): the lumped part "
           "adds exactly C") {
        CHECK(c10 - c3 == Approx(7e-12).epsilon(5e-4));
      }
      AND_THEN("the response is the same at 318 MHz and 1 GHz within 0.1 % (no dispersion; measured "
               "under 0.1 fF of 10 pF)") {
        CHECK(c10low == Approx(c10).epsilon(1e-3));
      }
      AND_THEN(
          "the surroundings add a constant stray of 0 to 5 times eps0 dl (measured 3.0 times, 26.6 fF)") {
        const double unit = 8.8541878128e-12 * 1e-3;
        CHECK(c0 > 0);
        CHECK(c0 < 5 * unit);
        CHECK(c3 - 3e-12 == Approx(c0).margin(0.5e-15));
      }
    }
  }
}

// Measurement, not a pass/fail scenario (hidden: run with "[.measure]"): how far the lumped capacitor's RC
// response is from 1 / (1 + j w R C) over frequency, capacitance, cell size and run length.
TEST_CASE("Lumped capacitor accuracy table", "[.measure]") {
  constexpr double kR = 50.0;
  std::printf("%8s %6s %6s %6s | %9s %9s | %s\n", "C(pF)", "cell", "steps", "band", "maxMag%", "maxPhs",
              "worst f(MHz)");
  for (double cp : {0.0, 1.0, 3.0, 10.0, 30.0})
    for (int cells : {24, 12})
      for (int steps : {3000, 6000, 12000}) {
        const auto grid = Grid::uniform({static_cast<std::size_t>(cells), static_cast<std::size_t>(cells),
                                         static_cast<std::size_t>(cells)},
                                        {0.024, 0.024, 0.024});
        Solver<double> s(grid, 0.95);
        const auto drive = sim::bandPulse(0.1e9, 1.5e9);
        LumpedPort port{static_cast<std::size_t>(cells / 2),
                        static_cast<std::size_t>(cells / 2),
                        static_cast<std::size_t>(cells / 2 - 1),
                        Z,
                        kR,
                        drive};
        port.capacitance = cp * 1e-12;
        s.addLumpedPort(std::move(port));
        const double dl = grid.cellSize(Z, static_cast<std::size_t>(cells / 2 - 1));
        std::vector<double> freqs;
        for (double f = 0.1e9; f <= 1.5e9 + 1; f += 0.1e9)
          freqs.push_back(f);
        std::vector<std::complex<double>> v(freqs.size()), vs(freqs.size());
        double eOld = 0;
        const auto at = static_cast<std::size_t>(cells / 2 - 1);
        for (int n = 0; n < steps; ++n) {
          s.step();
          const double t = s.time() - 0.5 * s.dt(), dt = s.dt();
          const double eNew =
              s.e(Z, static_cast<std::size_t>(cells / 2), static_cast<std::size_t>(cells / 2), at);
          const double ev = 0.5 * (eOld + eNew) * dl; // (n + 1/2) dt, where the source is also taken
          eOld = eNew;
          for (std::size_t q = 0; q < freqs.size(); ++q) {
            const auto w = std::polar(dt, -2 * std::numbers::pi * freqs[q] * t);
            v[q] += ev * w;
            vs[q] += s.portSourceVoltage(0) * w;
          }
        }
        // 1 / (V/Vs) = 1 + j w R C_eff: the capacitance the port sees, and how it varies with frequency
        double sum = 0, lo = 1e30, hi = -1e30, maxRe = 0;
        std::size_t used = 0;
        for (std::size_t q = 0; q < freqs.size(); ++q) {
          const auto inv = 1.0 / (v[q] / vs[q]);
          const double ceff = inv.imag() / (2 * std::numbers::pi * freqs[q] * kR);
          sum += ceff;
          lo = std::min(lo, ceff);
          hi = std::max(hi, ceff);
          maxRe = std::max(maxRe, std::abs(inv.real() - 1.0));
          ++used;
        }
        const double mean = sum / static_cast<double>(used) * 1e15; // fF
        std::printf(
            "%8.1f %5.1fmm %6d | Ceff mean %9.2f fF  stray %7.2f fF  spread %7.2f fF  max|Re-1| %.4f\n", cp,
            24.0 / cells, steps, mean, mean - cp * 1e3, (hi - lo) * 1e15, maxRe);
      }
}

// Measurement (hidden, "[.measure]"): the inductance and loss a lumped inductor presents at the port. With
// Y_e = (1 / (V/Vs) - 1) / R the edge admittance, Im(Y_e) = -1/(w L_eff) + w C_stray, where C_stray is the
// capacitor table's stray for the cell size (26.57 fF at 1 mm, 53.21 fF at 2 mm).
TEST_CASE("Lumped inductor accuracy table", "[.measure]") {
  constexpr double kR = 50.0;
  std::printf("band 0.1-1.5 GHz, R = 50 ohm; L_eff from Im(Ye) with the cell's stray capacitance removed\n");
  for (double lnh : {1.0, 3.0, 10.0, 30.0})
    for (int cells : {24, 12})
      for (int steps : {6000, 12000}) {
        const auto grid = Grid::uniform({static_cast<std::size_t>(cells), static_cast<std::size_t>(cells),
                                         static_cast<std::size_t>(cells)},
                                        {0.024, 0.024, 0.024});
        Solver<double> s(grid, 0.95);
        const std::size_t c2 = static_cast<std::size_t>(cells / 2);
        LumpedPort port{c2, c2, c2 - 1, Z, kR, sim::bandPulse(0.1e9, 1.5e9)};
        port.inductance = lnh * 1e-9;
        s.addLumpedPort(std::move(port));
        const double dl = grid.cellSize(Z, c2 - 1), cs = (cells == 24 ? 26.57e-15 : 53.21e-15);
        std::vector<double> freqs;
        for (double f = 0.1e9; f <= 1.5e9 + 1; f += 0.1e9)
          freqs.push_back(f);
        std::vector<std::complex<double>> v(freqs.size()), vs(freqs.size());
        double eOld = 0;
        for (int n = 0; n < steps; ++n) {
          s.step();
          const double t = s.time() - 0.5 * s.dt(), dt = s.dt(), eNew = s.e(Z, c2, c2, c2 - 1);
          for (std::size_t q = 0; q < freqs.size(); ++q) {
            const auto w = std::polar(dt, -2 * std::numbers::pi * freqs[q] * t);
            v[q] += 0.5 * (eOld + eNew) * dl * w;
            vs[q] += s.portSourceVoltage(0) * w;
          }
          eOld = eNew;
        }
        double lo = 1e30, hi = -1e30, sum = 0, maxG = 0;
        for (std::size_t q = 0; q < freqs.size(); ++q) {
          const auto ye = (1.0 / (v[q] / vs[q]) - 1.0) / kR;
          const double w = 2 * std::numbers::pi * freqs[q];
          const double leff = -1.0 / (w * (ye.imag() - w * cs));
          lo = std::min(lo, leff);
          hi = std::max(hi, leff);
          sum += leff;
          maxG = std::max(maxG, std::abs(ye.real()));
        }
        std::printf("L %5.1f nH cell %.0f mm steps %5d | L_eff mean %8.4f nH (%+.3f %%) spread %.4f nH  max "
                    "G %.2e S\n",
                    lnh, 24.0 / cells, steps, sum / static_cast<double>(freqs.size()) * 1e9,
                    100 * (sum / static_cast<double>(freqs.size()) / (lnh * 1e-9) - 1), (hi - lo) * 1e9,
                    maxG);
      }
}

SCENARIO("A lumped inductor does not make the scheme unstable, even a very small one on fine cells",
         "[slow][lumped]") {
  GIVEN("a 16 mm PEC cavity with 1 mm cells, a Gaussian pulse source and an inductor (no resistor) on an "
        "edge") {
    const auto grid = Grid::uniform({16, 16, 16}, {0.016, 0.016, 0.016});
    for (double lnh : {0.01, 1.0, 100.0}) {
      WHEN("the inductance is " + std::to_string(lnh) +
           " nH and the run is 20000 steps at 99 % of the CFL limit") {
        Solver<double> s(grid, 0.99);
        s.addSource({Z, 8, 8, 8, [](double t) { return std::exp(-std::pow((t - 4e-11) / 1e-11, 2)); }});
        LumpedPort port{4, 5, 6, Z, std::numeric_limits<double>::infinity(), [](double) { return 0.0; }};
        port.inductance = lnh * 1e-9;
        s.addLumpedPort(port);
        auto total = [&] { // field energy plus the inductor's (1/2) L I^2
          const double il = s.inductorCurrent(0);
          return s.energy() + 0.5 * port.inductance * il * il;
        };
        double settled = 0, peak = 0, last = 0;
        for (int n = 0; n < 20000; ++n) {
          s.step();
          if (n == 2000)
            settled = total();
          if (n > 2000)
            peak = std::max(peak, total());
          last = total();
        }
        INFO("L = " << lnh << " nH: energy after the pulse " << settled << " J, peak " << peak << " J, last "
                    << last << " J");
        THEN("the total energy after the pulse never exceeds its settled value by more than 0.2 % (measured "
             "0.05 %), and "
             "does not collapse to nothing (nothing but the inductor stores or dissipates)") {
          REQUIRE(std::isfinite(last));
          REQUIRE(peak <= 1.002 * settled);
          REQUIRE(last >= 0.5 * settled);
        }
      }
    }
  }
}

#include "fdtd/impedance.hpp"

SCENARIO("Impedance, VSWR and losses follow from the reflection coefficient") {
  using C = std::complex<double>;
  constexpr double kZ0 = 50.0;
  GIVEN("known loads against a 50 ohm reference") {
    THEN("a matched load (G = 0) is 50 ohm, VSWR 1, no mismatch loss") {
      CHECK(std::abs(fdtd::impedanceFromGamma(0, kZ0) - C(50, 0)) < 1e-12);
      CHECK(fdtd::vswr(0) == Approx(1.0).epsilon(1e-15));
      CHECK(fdtd::mismatchLossDb(0) == Approx(0.0).margin(1e-12));
    }
    THEN("G = 1/3 is a 100 ohm load, VSWR 2, return loss 9.542 dB, mismatch loss 0.5115 dB") {
      CHECK(std::abs(fdtd::impedanceFromGamma(1.0 / 3, kZ0) - C(100, 0)) < 1e-9);
      CHECK(fdtd::vswr(1.0 / 3) == Approx(2.0).epsilon(1e-12));
      CHECK(fdtd::returnLossDb(1.0 / 3) == Approx(9.5424250944).epsilon(1e-9));
      CHECK(fdtd::mismatchLossDb(1.0 / 3) == Approx(0.5115252245).epsilon(1e-9));
    }
    THEN("G = -1/3 is a 25 ohm load and VSWR 2") {
      CHECK(std::abs(fdtd::impedanceFromGamma(-1.0 / 3, kZ0) - C(25, 0)) < 1e-9);
      CHECK(fdtd::vswr(-1.0 / 3) == Approx(2.0).epsilon(1e-12));
    }
    THEN("G = j/3 gives Z = 50 (1 + j/3) / (1 - j/3) = 40 + j30 ohm (an inductive load)") {
      CHECK(std::abs(fdtd::impedanceFromGamma(C(0, 1.0 / 3), kZ0) - C(40, 30)) < 1e-9);
    }
    THEN("a short (G = -1) is 0 ohm and an open (G = 1) is infinite; both have infinite VSWR") {
      CHECK(std::abs(fdtd::impedanceFromGamma(-1, kZ0)) < 1e-9);
      CHECK(std::isinf(fdtd::impedanceFromGamma(1, kZ0).real()));
      CHECK(std::isinf(fdtd::vswr(-1)));
    }
    THEN("the round trip G -> Z -> G = (Z - z0) / (Z + z0) returns G within 1e-12") {
      for (const C g : {C(0.2, 0.1), C(-0.5, 0.3), C(0.05, -0.7)}) {
        const auto z = fdtd::impedanceFromGamma(g, kZ0);
        CHECK(std::abs((z - kZ0) / (z + kZ0) - g) < 1e-12);
      }
    }
  }
}
