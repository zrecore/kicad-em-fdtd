// End-to-end scenarios: snapshot -> voxelizer -> solver -> S11. Analytic references in each THEN.
#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmath>
#include <string>

#include "board/snapshot.hpp"
#include "fdtd/sparam.hpp"
#include "sim/simulation.hpp"

using Catch::Approx;

SCENARIO("A port driving a known resistive load reports the analytic reflection coefficient", "[slow]") {
  GIVEN("two copper pads 2 mm apart, a 50 ohm port across the gap and a passive 100 ohm port beside it") {
    // The gap is electrically tiny at 0.3-0.6 GHz, so the 50 ohm source sees the 100 ohm load (plus a small
    // gap capacitance): S11 = (100 - 50) / (100 + 50) = 1/3.
    const auto snap = board::parseSnapshot(R"({"format":"kicad-em-fdtd.snapshot","version":1,
      "board":{"name":"load"},
      "outline":[[0,0],[0.01,0],[0.01,0.01],[0,0.01]],
      "stackup":[{"name":"F.Cu","kind":"copper"},
                 {"name":"core","kind":"dielectric","thickness":0.0008,"epsilon_r":4.4,"loss_tangent":0.0}],
      "copper":[{"layer":"F.Cu","owner":"","net":"A","polygon":[[0.0012,0.0012],[0.0029,0.0012],[0.0029,0.0088],[0.0012,0.0088]]},
                {"layer":"F.Cu","owner":"","net":"B","polygon":[[0.0051,0.0012],[0.0088,0.0012],[0.0088,0.0088],[0.0051,0.0088]]}],
      "vias":[],"components":[],
      "ports":[{"name":"P1","layer":"F.Cu","a":[0.003,0.004],"b":[0.005,0.004],"width":0.0022,"impedance":50},
               {"name":"load","layer":"F.Cu","a":[0.003,0.007],"b":[0.005,0.007],"width":0.0022,"impedance":100}]})");
    const board::GridPlan plan{
        .lateralCell = 1e-3, .maxDielectricCell = 0.4e-3, .airMargin = 8e-3, .airCell = 1e-3};
    const auto model = board::voxelize(snap, board::Selection{}, plan, 0.45e9);
    sim::RunOptions opt;
    opt.fMinHz = 0.3e9;
    opt.fMaxHz = 0.6e9;
    opt.points = 7;
    opt.cpmlCells = 6;
    WHEN("the structure is run until the port voltage has decayed") {
      std::vector<double> progress;
      opt.onProgress = [&](const sim::RunResult& p) { progress.push_back(p.progress); };
      const auto r = sim::runOnePort(model, opt, sim::makeCpuEngine);
      AND_THEN("the progress estimate never goes backwards, moves from the first steps, and ends at 1") {
        REQUIRE(progress.size() > 4);
        REQUIRE(std::is_sorted(progress.begin(), progress.end()));
        REQUIRE(progress.front() > 0.0);
        REQUIRE(progress[progress.size() / 4] > progress.front());
        REQUIRE(progress[progress.size() / 2] < 1.0);
        REQUIRE(r.progress == 1.0);
      }
      THEN("|S11 - 1/3| is below 0.02 across the band") {
        for (const auto& s : r.s.s11)
          REQUIRE(std::abs(s - 1.0 / 3.0) < 0.02);
      }
      AND_THEN("the run stopped on decay, not on the step limit") {
        REQUIRE(r.steps < opt.maxSteps);
      }
      AND_THEN(
          "the power the port delivers, summed over its strips, is 1 - (1/3)^2 = 8/9 of the available power "
          "|Vs|^2 / (4 z0) within 0.03 at every frequency (a resistive load accepts what it does not "
          "reflect)") {
        const auto acc =
            fdtd::acceptedPower(r.stripVoltage, r.portSource, r.dt, r.stripResistance, r.s.frequenciesHz);
        const auto vs = fdtd::sourceSpectrum(r.portSource, r.dt, r.s.frequenciesHz);
        for (std::size_t i = 0; i < acc.size(); ++i) {
          INFO("f " << r.s.frequenciesHz[i] * 1e-9 << " GHz: " << acc[i] / (std::norm(vs[i]) / 200.0));
          REQUIRE(std::abs(acc[i] / (std::norm(vs[i]) / 200.0) - 8.0 / 9.0) < 0.01);
        }
      }
    }
  }
}

SCENARIO("A board with a lumped part but no port is refused, not driven through the part") {
  GIVEN("two pads joined by a fitted 100R resistor and no port") {
    const auto snap = board::parseSnapshot(R"({"format":"kicad-em-fdtd.snapshot","version":1,
      "board":{"name":"noport"},
      "outline":[[0,0],[0.01,0],[0.01,0.01],[0,0.01]],
      "stackup":[{"name":"F.Cu","kind":"copper"},
                 {"name":"core","kind":"dielectric","thickness":0.0008,"epsilon_r":4.4,"loss_tangent":0.0}],
      "copper":[{"layer":"F.Cu","owner":"R1","net":"A","polygon":[[0.003,0.0075],[0.004,0.0075],[0.004,0.0085],[0.003,0.0085]]},
                {"layer":"F.Cu","owner":"R1","net":"B","polygon":[[0.005,0.0075],[0.006,0.0075],[0.006,0.0085],[0.005,0.0085]]}],
      "vias":[],"ports":[],
      "components":[{"ref":"R1","value":"100R","x":0.0045,"y":0.008,"layer":"F.Cu",
        "pads":[{"number":"1","net":"A","x":0.0035,"y":0.008},{"number":"2","net":"B","x":0.0055,"y":0.008}]}]})");
    const board::GridPlan plan{
        .lateralCell = 1e-3, .maxDielectricCell = 0.4e-3, .airMargin = 2e-3, .airCell = 1e-3};
    const auto model = board::voxelize(snap, board::Selection{}, plan, 2.4e9);
    THEN("the resistor is in the model as a load") {
      REQUIRE(model.ports.size() == 1);
      REQUIRE(model.ports[0].lumped);
    }
    AND_THEN("building a solver says there is no port to drive") {
      sim::RunOptions opt;
      opt.cpmlCells = 0; // the tiny test domain has no room for an absorbing layer
      REQUIRE_THROWS_WITH(sim::buildSolver(model, opt, [](double) { return 0.0; }),
                          Catch::Matchers::ContainsSubstring("no port to drive"));
    }
  }
}

SCENARIO(
    "A narrow port on a lossless pad reflects everything; a port as wide as the pad appears to lose power",
    "[slow]") {
  GIVEN("a 3 x 3 mm pad over a ground plane on 1.6 mm FR-4 (no loss), in a closed PEC box, 1 to 3 GHz") {
    auto board = [](double width) {
      return board::parseSnapshot(
          R"({"format":"kicad-em-fdtd.snapshot","version":1,"board":{"name":"pad"},
      "outline":[[0,0],[0.012,0],[0.012,0.008],[0,0.008]],
      "stackup":[{"name":"F.Cu","kind":"copper"},
                 {"name":"core","kind":"dielectric","thickness":0.0016,"epsilon_r":4.4,"loss_tangent":0.0},
                 {"name":"B.Cu","kind":"copper"}],
      "copper":[{"layer":"B.Cu","owner":"","net":"G","polygon":[[0,0],[0.012,0],[0.012,0.008],[0,0.008]]},
                {"layer":"F.Cu","owner":"","net":"S","polygon":[[0.0028,0.0025],[0.0058,0.0025],[0.0058,0.0055],[0.0028,0.0055]]}],
      "vias":[],"components":[],
      "ports":[{"name":"P1","layer":"F.Cu","layer_b":"B.Cu","a":[0.0032,0.004],"width":)" +
          std::to_string(width) + R"(,"impedance":50}]})");
    };
    const board::GridPlan plan{
        .lateralCell = 0.25e-3, .maxDielectricCell = 0.4e-3, .airMargin = 2e-3, .airCell = 0.5e-3};
    auto run = [&](double width) {
      const auto model = board::voxelize(board(width), board::Selection{}, plan, 2e9);
      sim::RunOptions opt;
      opt.fMinHz = 1.0e9;
      opt.fMaxHz = 3.0e9;
      opt.points = 3;
      opt.cpmlCells = 0;
      opt.maxSteps = 20000;
      opt.decay = 1e-12;
      return sim::runOnePort(model, opt, sim::makeCpuEngine);
    };
    // the accepted power from the strips, as a fraction of the available power |Vs|^2 / (4 z0), at the
    // sweep's frequencies
    auto acceptedFraction = [](const sim::RunResult& r) {
      const auto acc =
          fdtd::acceptedPower(r.stripVoltage, r.portSource, r.dt, r.stripResistance, r.s.frequenciesHz);
      const auto vs = fdtd::sourceSpectrum(r.portSource, r.dt, r.s.frequenciesHz);
      std::vector<double> f;
      for (std::size_t i = 0; i < acc.size(); ++i)
        f.push_back(acc[i] / (std::norm(vs[i]) / (4 * 50.0)));
      return f;
    };
    WHEN("the port is 1 mm wide") {
      const auto r = run(1e-3);
      const auto& s11 = r.s.s11;
      THEN(
          "|S11| is 1 within 1e-3 at 1, 2 and 3 GHz (measured 1.0000, 0.9999, 0.9996): nothing can be lost") {
        for (const auto& v : s11)
          REQUIRE(std::abs(v) == Approx(1.0).margin(1e-3));
      }
    }
    WHEN("the port is as wide as the pad, 3 mm") {
      const auto r = run(3e-3);
      const auto& s11 = r.s.s11;
      THEN("|S11| falls below 0.95 at 3 GHz (measured 0.886) although the structure is lossless: the strips "
           "see different voltages, so their mean is not a terminal voltage (a known limit of the wide port, "
           "flagged in the run notes)") {
        REQUIRE(std::abs(s11.back()) < 0.95);
        REQUIRE(std::abs(s11.back()) > 0.80);
        REQUIRE(std::abs(s11.front()) > std::abs(s11.back())); // worse with frequency
      }
      AND_THEN("the power measured strip by strip says what is true: the accepted power is under 2 % of the "
               "available power at every frequency (measured 0.05, 0.30 and 0.84 % at 1, 2 and 3 GHz), where "
               "(1 - |S11|^2) would claim 3.9, 12.6 and 21.5 %") {
        const auto f = acceptedFraction(r);
        for (std::size_t i = 0; i < f.size(); ++i) {
          INFO("accepted fraction " << f[i] << ", S11-implied " << 1 - std::norm(s11[i]));
          REQUIRE(std::abs(f[i]) < 0.02);
        }
        REQUIRE(1 - std::norm(s11.back()) > 0.15);
      }
    }
  }
}
