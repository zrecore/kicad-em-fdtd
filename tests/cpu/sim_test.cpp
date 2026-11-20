// End-to-end scenarios: snapshot -> voxelizer -> solver -> S11. Analytic references in each THEN.
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <string>

#include "board/snapshot.hpp"
#include "sim/simulation.hpp"

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
      const auto r = sim::runOnePort(model, opt, sim::makeCpuEngine);
      THEN("|S11 - 1/3| is below 0.02 across the band") {
        for (const auto& s : r.s.s11)
          REQUIRE(std::abs(s - 1.0 / 3.0) < 0.02);
      }
      AND_THEN("the run stopped on decay, not on the step limit") {
        REQUIRE(r.steps < opt.maxSteps);
      }
    }
  }
}
