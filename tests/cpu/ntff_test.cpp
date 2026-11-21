// BDD scenarios for the near-to-far-field transform (docs/PLAN.md, Next steps 11f).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>

#include "core/units.hpp"
#include "fdtd/ntff.hpp"

using Catch::Approx;
using namespace fdtd;

namespace {
const double kEta = std::sqrt(units::kMu0 / units::kEps0);
}

SCENARIO("The far-field integral of a short current element is the textbook pattern") {
  GIVEN("one electric current element I l = 2 A m along z at the origin, at 1 GHz") {
    NtffPlan plan;
    plan.channels.push_back({Kind::H, Z, 0});
    plan.elements.push_back({'J', Z, {0, 0, 0}, 1.0, 1.0, 0, std::numeric_limits<std::uint32_t>::max()});
    const std::complex<double> il(2.0, 0.0);
    const auto ff = farField(plan, [&](std::uint32_t) { return il; }, 1e9, 180, 72);
    const double k = 2 * std::numbers::pi * 1e9 / units::kSpeedOfLight;
    THEN("U(theta) = eta k^2 |I l|^2 sin^2(theta) / (32 pi^2) at every angle, within 1e-12 relative") {
      for (std::size_t i = 0; i < ff.nTheta; i += 7)
        for (std::size_t j = 0; j < ff.nPhi; j += 11) {
          const double s = std::sin(ff.theta[i]);
          const double expect =
              kEta * k * k * std::norm(il) * s * s / (32 * std::numbers::pi * std::numbers::pi);
          REQUIRE(ff.u[i * ff.nPhi + j] == Approx(expect).epsilon(1e-12));
        }
    }
    AND_THEN("the radiated power is eta (k |I l|)^2 / (12 pi), within 2e-4 relative") {
      REQUIRE(ff.power ==
              Approx(kEta * std::pow(k * std::abs(il), 2) / (12 * std::numbers::pi)).epsilon(2e-4));
    }
    AND_THEN("the directivity is 1.5 (1.76 dBi), within 2e-4") {
      REQUIRE(ff.directivity() == Approx(1.5).epsilon(2e-4));
    }
  }
  GIVEN("a magnetic current element along z of the same strength") {
    NtffPlan plan;
    plan.channels.push_back({Kind::E, Z, 0});
    plan.elements.push_back({'M', Z, {0, 0, 0}, 1.0, 1.0, 0, std::numeric_limits<std::uint32_t>::max()});
    const auto ff =
        farField(plan, [](std::uint32_t) { return std::complex<double>(2.0, 0.0); }, 1e9, 180, 72);
    THEN("it radiates the same pattern with M / eta in place of J: power and directivity 1.5") {
      NtffPlan e;
      e.channels.push_back({Kind::H, Z, 0});
      e.elements.push_back({'J', Z, {0, 0, 0}, 1.0, 1.0, 0, std::numeric_limits<std::uint32_t>::max()});
      const auto fe =
          farField(e, [](std::uint32_t) { return std::complex<double>(2.0 / kEta, 0.0); }, 1e9, 180, 72);
      REQUIRE(ff.power == Approx(fe.power).epsilon(1e-9));
      REQUIRE(ff.directivity() == Approx(1.5).epsilon(2e-4));
    }
  }
  GIVEN("an element displaced along x by a quarter wavelength and a second one at the origin, in phase") {
    NtffPlan plan;
    plan.channels.push_back({Kind::H, Z, 0});
    const auto none = std::numeric_limits<std::uint32_t>::max();
    const double lambda = units::kSpeedOfLight / 1e9;
    plan.elements.push_back({'J', Z, {-lambda / 8, 0, 0}, 1.0, 1.0, 0, none});
    plan.elements.push_back({'J', Z, {lambda / 8, 0, 0}, 1.0, 1.0, 0, none});
    plan.centre = {0, 0, 0};
    const auto ff =
        farField(plan, [](std::uint32_t) { return std::complex<double>(1.0, 0.0); }, 1e9, 180, 360);
    THEN("the pair has the array factor 2 cos(k d sin(theta) cos(phi) / 2): power at (90 deg, 0) is "
         "|2 cos(pi/4 ... )| times the single element's, within 1e-6") {
      // two elements a quarter wavelength apart, in phase: along x the factor is 2 cos(pi/4) = sqrt 2
      NtffPlan one;
      one.channels = plan.channels;
      one.elements.push_back({'J', Z, {0, 0, 0}, 1.0, 1.0, 0, none});
      const auto f1 =
          farField(one, [](std::uint32_t) { return std::complex<double>(1.0, 0.0); }, 1e9, 180, 360);
      // theta index 90 is 90.25 deg; compare the same cell: phi index 0 is 0.5 deg, so the factor is
      // evaluated at those angles
      const std::size_t it = 90, ip = 0;
      const double st = std::sin(ff.theta[it]), cp = std::cos(ff.phi[ip]);
      const double k = 2 * std::numbers::pi * 1e9 / units::kSpeedOfLight;
      const double af = 2 * std::cos(k * (lambda / 4) * st * cp / 2);
      REQUIRE(ff.u[it * ff.nPhi + ip] == Approx(af * af * f1.u[it * f1.nPhi + ip]).epsilon(1e-9));
    }
  }
}

#include "fdtd/solver.hpp"
#include "sim/simulation.hpp"

namespace {

struct DipoleResult {
  double fluxOverFar, farOverPort, directivity;
  AntennaFigures figures;
  double gammaMagSq;
};

// A vertical PEC wire of 2 * arms + 1 cells with a 50 ohm port in its middle cell, in an n^3 box with a CPML
// of `pml` cells, a Huygens box `margin` cells inside the CPML, 1.5 GHz. Returns the ratios the scenario
// checks.
DipoleResult shortDipole(std::size_t n, double cell, std::size_t arms, std::size_t pml, std::size_t margin,
                         int steps, std::size_t nTheta = 90) {
  constexpr double kFreq = 1.5e9;
  const auto grid = Grid::uniform({n, n, n}, {static_cast<double>(n) * cell, static_cast<double>(n) * cell,
                                              static_cast<double>(n) * cell});
  Solver<double> s(grid, 0.95);
  s.enableCpml({.cells = pml});
  const std::size_t c = n / 2;
  std::vector<PecEdge> wire;
  for (std::size_t a = 1; a <= arms; ++a) {
    wire.push_back({Z, grid.nodeIndex(c, c, c - a)});
    wire.push_back({Z, grid.nodeIndex(c, c, c + a)});
  }
  s.forcePecEdges(wire);
  s.addLumpedPort({c, c, c, Z, 50.0, sim::bandPulse(0.9e9, 2.1e9)});
  const auto plan = makeNtffPlan(grid, {pml + margin, pml + margin, pml + margin},
                                 {n - pml - margin, n - pml - margin, n - pml - margin});
  std::vector<std::complex<double>> x(plan.channels.size());
  std::complex<double> vPort = 0, iPort = 0, vEdge = 0,
                       vSource = 0; // vEdge, vSource: as s11FromPort stamps them
  double eOld = 0;
  const double dl = grid.cellSize(Z, c);
  for (int step = 0; step < steps; ++step) {
    s.step();
    const double t = s.time(), dt = s.dt();
    const auto pe = std::polar(dt, -2 * std::numbers::pi * kFreq * t);
    const auto ph = std::polar(dt, -2 * std::numbers::pi * kFreq * (t - 0.5 * dt));
    for (std::size_t k = 0; k < x.size(); ++k) {
      const auto& ch = plan.channels[k];
      x[k] += s.field(ch.kind, ch.component)[ch.node] * (ch.kind == Kind::E ? pe : ph);
    }
    // the port current is the mean of the steps' fields, at (n + 1/2) dt: take the voltage there too (the
    // dipole is mostly reactive, so a half-step skew would swamp the real power)
    const double eNew = s.e(Z, c, c, c);
    const auto pm = std::polar(dt, -2 * std::numbers::pi * kFreq * (t - 0.5 * dt));
    vPort += 0.5 * (eOld + eNew) * dl * pm;
    iPort += s.portCurrent(0) * pm;
    vEdge += eNew * dl * pe;
    vSource += s.portSourceVoltage(0) * pm;
    eOld = eNew;
  }
  const PhasorOf at = [&](std::uint32_t k) { return x[k]; };
  const auto ff = farField(plan, at, kFreq, nTheta, 72);
  const double flux = surfaceFlux(plan, at);
  const double pIn = std::abs((vPort * std::conj(iPort)).real()); // transform convention: no 1/2
  const std::complex<double> s11 = 2.0 * vEdge / vSource - 1.0;
  return {flux / ff.power, 2 * ff.power / pIn, ff.directivity(), antennaFigures(ff, vSource, s11, 50.0),
          std::norm(s11)};
}

} // namespace

TEST_CASE("Short dipole convergence table", "[.measure]") {
  std::printf("%5s %6s %4s %4s %6s %6s | %10s %12s %10s\n", "n", "cell", "arms", "pml", "margin", "steps",
              "flux/far", "2 far/P_in", "D");
  struct Case {
    std::size_t n;
    double cell;
    std::size_t arms, pml, margin;
    int steps;
  };
  for (const Case c :
       {Case{48, 3e-3, 3, 8, 3, 3500}, Case{48, 3e-3, 3, 8, 3, 6000}, Case{48, 3e-3, 3, 8, 6, 3500},
        Case{48, 3e-3, 3, 8, 1, 3500}, Case{48, 3e-3, 3, 12, 3, 3500}, Case{36, 4e-3, 2, 8, 3, 2600},
        Case{60, 2.4e-3, 4, 8, 3, 4400}, Case{72, 2e-3, 5, 8, 3, 5200}}) {
    const auto r = shortDipole(c.n, c.cell, c.arms, c.pml, c.margin, c.steps);
    std::printf("%5zu %4.1fmm %4zu %4zu %6zu %6d | %10.5f %12.5f %10.5f\n", c.n, c.cell * 1e3, c.arms, c.pml,
                c.margin, c.steps, r.fluxOverFar, r.farOverPort, r.directivity);
  }
}

SCENARIO("A short dipole in free space: the transform's power matches the surface flux and the port power",
         "[slow][ntff]") {
  GIVEN("a 7-cell vertical wire (0.1 wavelength) with a 50 ohm port at its centre, in a CPML box, 1.5 GHz") {
    WHEN("the pulse is run in for 3500 steps and the phasors at 1.5 GHz are accumulated every step") {
      const auto r = shortDipole(48, 3e-3, 3, 8, 3, 3500);
      THEN("the far-field power equals the power through the box within 0.3 % (measured 0.05 to 0.12 % over "
           "cell sizes 2 to 4 mm, box distances and CPML thicknesses)") {
        REQUIRE(r.fluxOverFar == Approx(1.0).epsilon(3e-3));
      }
      AND_THEN(
          "twice the far-field power equals the port's input power within 0.3 % (measured under 0.07 %): "
          "everything the port delivers is radiated, nothing else is lost") {
        REQUIRE(r.farOverPort == Approx(1.0).epsilon(3e-3));
      }
      AND_THEN("the directivity is 1.5 within 1 % (measured 1.5059 to 1.5071: the wire is 0.1 wavelength, "
               "not infinitely short)") {
        REQUIRE(r.directivity == Approx(1.5).epsilon(1e-2));
      }
    }
  }
}

SCENARIO("A half-wave dipole has a directivity of 2.15 dBi", "[slow][ntff]") {
  GIVEN("a 33-cell wire (0.495 wavelength at 1.5 GHz, 3 mm cells) with a port at its centre, in a CPML box") {
    WHEN("it is run for 4500 steps and the far field at 1.5 GHz is integrated on a 180 x 72 grid") {
      const auto r = shortDipole(84, 3e-3, 16, 8, 3, 4500, 180);
      THEN("the directivity is within 0.1 dB of 2.15 dBi (measured 2.21 dBi; the thin wire is one cell wide "
           "and its current is not exactly sinusoidal) and the power balances within 0.3 %") {
        REQUIRE(10 * std::log10(r.directivity) == Approx(2.15).margin(0.1));
        REQUIRE(r.fluxOverFar == Approx(1.0).epsilon(3e-3));
        REQUIRE(r.farOverPort == Approx(1.0).epsilon(3e-3));
      }
      AND_THEN("the figures made from S11 and the source alone agree: the radiation efficiency is 1 within "
               "0.5 % (measured 1.0008: the wire is lossless), the gain equals the directivity times that, "
               "and the realised gain equals the gain times 1 - |S11|^2 (measured |S11|^2 0.23 against 50 "
               "ohm)") {
        REQUIRE(r.figures.efficiency == Approx(1.0).epsilon(5e-3));
        REQUIRE(r.figures.gain == Approx(r.figures.efficiency * r.directivity).epsilon(1e-12));
        REQUIRE(r.figures.realisedGain == Approx(r.figures.gain * (1 - r.gammaMagSq)).epsilon(1e-12));
        REQUIRE(r.gammaMagSq > 0.1);
        REQUIRE(r.gammaMagSq < 0.4);
      }
    }
  }
}
