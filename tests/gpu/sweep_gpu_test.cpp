// A parameter sweep on the GPU against a closed-form value (docs/PLAN.md, Next steps 11g). Hidden from the
// default GPU suite because the debug build with the validation layer needs many minutes for 50 000 steps;
// run it from the release build under the lock:
//   with_gpu_lock.sh nice -n 10 build/release/tests/tests_gpu "[.sweepgpu]"
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>

#include "app/paths.hpp"
#include "app/sweep.hpp"
#include "fdtd_vk/gpu_engine.hpp"

namespace {
const app::Catalog& catalog() {
  static const app::Catalog c = app::Catalog::load(std::string(FDTD_DATA_DIR) + "/radio_profiles.json");
  return c;
}
} // namespace

SCENARIO("A sweep of the fine cell size on the GPU gives the load's reflection at every point",
         "[.sweepgpu]") {
  GIVEN("a 100 ohm load across a 50 ohm port, band 0.3 to 0.6 GHz, fine cells of 1.0 and 0.5 mm") {
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
    const auto dir = std::filesystem::path(FDTD_TEST_TMP) / "sweep-gpu-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "board");
    std::ofstream(dir / "board" / "board.json") << "{}";
    const app::ProjectPaths paths(dir / "board");
    vkcore::Context ctx({});
    board::Selection selection;
    app::Settings base(catalog());
    base.set("band_min_ghz", 0.3);
    base.set("band_max_ghz", 0.6);
    base.set("boundary_distance_wavelengths", 0.05);
    base.set("decay_threshold", 0.01);
    base.set("mesh_preset", std::string("coarse"));
    base.set("backend", std::string("gpu"));
    app::SweepEnv env{snap,
                      (dir / "board" / "board.json").string(),
                      catalog(),
                      paths,
                      selection,
                      [&](fdtd::Solver<float>& s) { return std::make_unique<fdtd_vk::GpuEngine>(ctx, s); },
                      "GPU",
                      true,
                      static_cast<double>(ctx.deviceLocalBytes())};
    env.stamp = "gpu";
    WHEN("the sweep runs") {
      const auto pts = app::runSweep(env, base, {"fine_cell_mm", {"1.0", "0.5"}});
      THEN("both points ran, and |S11| is 1/3 within 0.06 across the band 0.3 to 0.6 GHz (measured 0.345 to "
           "0.377 on the coarse mesh; the analytic value for a 100 ohm load on a 50 ohm port)") {
        REQUIRE(pts.size() == 2);
        for (const auto& p : pts) {
          REQUIRE(p.error.empty());
          std::size_t inBand = 0;
          for (std::size_t i = 0; i < p.s.s11.size(); ++i)
            if (p.s.frequenciesHz[i] >= 0.3e9 && p.s.frequenciesHz[i] <= 0.6e9) {
              REQUIRE(std::abs(std::abs(p.s.s11[i]) - 1.0 / 3.0) < 0.06);
              ++inBand;
            }
          REQUIRE(inBand > 10);
        }
      }
      AND_THEN("the two cell sizes agree on |S11| to within 0.01 at every frequency (the sweep's own "
               "convergence check)") {
        REQUIRE(pts[0].s.s11.size() == pts[1].s.s11.size());
        for (std::size_t i = 0; i < pts[0].s.s11.size(); ++i)
          REQUIRE(std::abs(std::abs(pts[0].s.s11[i]) - std::abs(pts[1].s.s11[i])) < 0.01);
      }
    }
  }
}
