// BDD scenarios for parameter sweeps (docs/PLAN.md, Next steps 11g).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>
#include <fstream>

#include "app/paths.hpp"
#include "app/sweep.hpp"

using Catch::Approx;
using Catch::Matchers::ContainsSubstring;

namespace {
const app::Catalog& catalog() {
  static const app::Catalog c = app::Catalog::load(std::string(FDTD_DATA_DIR) + "/radio_profiles.json");
  return c;
}
} // namespace

SCENARIO("Sweep values are read as a list or as a range") {
  GIVEN("the two forms") {
    THEN("a list keeps its values in order, trimmed") {
      const auto v = app::parseSweepValues("coarse, medium ,fine");
      REQUIRE(v == std::vector<std::string>{"coarse", "medium", "fine"});
    }
    THEN("a range gives evenly spaced values including both ends, within 1e-9") {
      const auto v = app::parseSweepValues("2:4:5");
      REQUIRE(v.size() == 5);
      const double expect[5] = {2, 2.5, 3, 3.5, 4};
      for (std::size_t i = 0; i < 5; ++i)
        REQUIRE(std::stod(v[i]) == Approx(expect[i]).margin(1e-9));
    }
    THEN("an empty list, a bad range and too many points are refused with a message") {
      REQUIRE_THROWS_AS(app::parseSweepValues(""), app::SettingsError);
      REQUIRE_THROWS_AS(app::parseSweepValues("1:2"), app::SettingsError);
      REQUIRE_THROWS_AS(app::parseSweepValues("1:2:1"), app::SettingsError);
      REQUIRE_THROWS_AS(app::parseSweepValues("a:b:3"), app::SettingsError);
      REQUIRE_THROWS_WITH(app::parseSweepValues("1:2:100"), ContainsSubstring("at most 25"));
    }
  }
}

SCENARIO("A sweep runs one point per value, keeps each S11 and each results folder", "[slow][sweep]") {
  GIVEN("a 100 ohm load across a 50 ohm port, swept over the mesh quality, plus one value that does not "
        "exist") {
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
    const auto dir = std::filesystem::path(FDTD_TEST_TMP) / "sweep-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "board");
    std::ofstream(dir / "board" / "board.json") << "{}";
    const app::ProjectPaths paths(dir / "board");
    board::Selection selection;
    app::Settings base(catalog());
    base.set("backend", std::string("cpu"));
    base.set("boundary_distance_wavelengths",
             0.05); // a small domain: the board is 10 mm and the band 2.4 GHz
    base.set("max_steps",
             150.0); // structure only: the physics is checked on the GPU (tests/gpu/sweep_gpu_test.cpp)
    app::SweepEnv env{snap,           (dir / "board" / "board.json").string(),
                      catalog(),      paths,
                      selection,      sim::makeCpuEngine,
                      "CPU reference"};
    std::size_t started = 0, finished = 0;
    env.progress = [&](std::size_t, std::size_t, const app::SweepPoint* done) {
      (done ? finished : started)++;
    };
    env.stamp = "test";
    WHEN("the sweep runs") {
      const auto pts = app::runSweep(env, base, {"sweep_points", {"11", "21", "1"}});
      THEN("there is one point per value, in order, and the bad one carries its error and does not stop the "
           "sweep") {
        REQUIRE(pts.size() == 3);
        REQUIRE(pts[0].value == "11");
        REQUIRE(pts[0].error.empty());
        REQUIRE(pts[1].error.empty());
        REQUIRE(!pts[2].error.empty());
        REQUIRE(pts[2].s.s11.empty());
        REQUIRE(started == 3);
        REQUIRE(finished == 3);
        REQUIRE(pts[0].s.s11.size() == 11);
        REQUIRE(pts[1].s.s11.size() == 21);
      }
      AND_THEN("the good points' S11 values are finite and passive (|S11| below 1.2 even for this cut-short "
               "run)") {
        REQUIRE(!pts[0].s.s11.empty());
        for (const auto& p : {pts[0], pts[1]})
          for (const auto& v : p.s.s11) {
            REQUIRE(std::isfinite(v.real()));
            REQUIRE(std::abs(v) < 1.2);
          }
      }
      AND_THEN("each point has its own folder with a Touchstone file and a summary, and the sweep has a "
               "combined file naming both points") {
        REQUIRE(std::filesystem::exists(pts[0].dir / "s11.s1p"));
        REQUIRE(std::filesystem::exists(pts[0].dir / "summary.txt"));
        const auto root = paths.resultsRoot / "sweep-test";
        std::ifstream f(root / "sweep.json");
        const std::string text((std::istreambuf_iterator<char>(f)), {});
        REQUIRE_THAT(text, ContainsSubstring("\"value\": \"11\""));
        REQUIRE_THAT(text, ContainsSubstring("\"error\""));
      }
      AND_THEN("the point's summary says which setting changed") {
        std::ifstream f(pts[0].dir / "summary.txt");
        const std::string text((std::istreambuf_iterator<char>(f)), {});
        REQUIRE_THAT(text, ContainsSubstring("changed from default: sweep_points"));
      }
    }
  }
}
