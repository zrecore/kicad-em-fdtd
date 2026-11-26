// BDD scenarios for the machine-readable output of the command line (docs/cli-json.md).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "app/cli_json.hpp"
#include "app/settings.hpp"
#include "core/resources.hpp"
#include "fdtd/sparam.hpp"

namespace fs = std::filesystem;
using Json = nlohmann::json;
using Catch::Approx;
namespace cj = app::clijson;

namespace {

fs::path scratch(const std::string& name) {
  const char* base = std::getenv("FDTD_TEST_TMP");
  const fs::path dir = (base ? fs::path(base) : fs::temp_directory_path()) / "cli_json" / name;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

void requireHeader(const Json& j) {
  REQUIRE(j.at("schema") == 1);
  REQUIRE(j.at("tool") == "emfdtd");
  REQUIRE(j.at("version") == "9.9.9");
}

// A saved run with a known S11: a Lorentzian dip of -20 dB at 2.44 GHz on a flat -3 dB floor, 101 points.
fs::path makeRun(const std::string& name, bool legacySummary) {
  const fs::path dir = scratch(name);
  fdtd::SParam1 s;
  for (int i = 0; i <= 100; ++i) {
    const double f = 2.0e9 + 0.01e9 * i;
    const double x = (f - 2.44e9) / 0.05e9;
    const double w = 1.0 / (1.0 + x * x); // 1 at the dip, falling to 0
    const double mag = std::pow(10.0, -3.0 / 20.0) * (1.0 - w) + std::pow(10.0, -20.0 / 20.0) * w;
    s.frequenciesHz.push_back(f);
    s.s11.push_back(std::complex<double>(-mag, 0.0));
  }
  std::ofstream out(dir / "s11.s1p");
  fdtd::writeTouchstone(out, s, "test");
  out.close();
  std::ofstream sum(dir / "summary.txt");
  sum << "kicad-em-fdtd run summary\nboard: testboard\nprofile: bluetooth / Bluetooth Low Energy [verified]\n"
      << "source of the band: a source\n";
  if (!legacySummary)
    sum << "band of interest: 2.4 to 2.4835 GHz\nS11 reported: 2 to 3 GHz\n";
  sum << "grid: 100 x 80 x 40 cells, smallest cell 0.035 mm\n"
      << (legacySummary ? "steps: 12345, dt 0.0775 ps, backend GPU\n"
                        : "steps: 12345, time step 0.0775 ps, backend GPU\n")
      << "changed from default: mesh_preset\nchanged from default: decay_threshold\nnote: a note\n";
  std::ofstream(dir / "settings.json") << R"({"format":"kicad-em-fdtd.settings","version":1,"values":{}})";
  return dir;
}

} // namespace

SCENARIO("The settings list is typed and complete") {
  GIVEN("the registry of a fresh Settings") {
    const app::Catalog catalog =
        app::Catalog::load(core::resourceDir("data", FDTD_DATA_DIR) + "/radio_profiles.json");
    const app::Settings settings(catalog);
    WHEN("it is listed as JSON") {
      const Json j = Json::parse(cj::settingsList("9.9.9", settings));
      THEN("the header is there and every registry key appears once, in registry order") {
        requireHeader(j);
        const auto& list = j.at("settings");
        REQUIRE(list.size() == settings.defs().size());
        for (std::size_t i = 0; i < list.size(); ++i)
          REQUIRE(list[i].at("key") == settings.defs()[i].key);
      }
      THEN("numbers are JSON numbers with their range, choices are strings with their choices") {
        bool sawNumber = false, sawChoice = false;
        for (const auto& s : j.at("settings")) {
          REQUIRE(s.contains("tier"));
          REQUIRE(s.contains("group"));
          REQUIRE(s.contains("help"));
          REQUIRE(s.at("default") == s.at("value"));
          if (s.at("kind") == "number") {
            sawNumber = true;
            REQUIRE(s.at("value").is_number());
            REQUIRE(s.at("min").get<double>() <= s.at("max").get<double>());
            REQUIRE(s.at("value").get<double>() == Approx(settings.number(s.at("key"))));
          } else if (s.at("kind") == "choice") {
            sawChoice = true;
            REQUIRE(s.at("value").is_string());
            REQUIRE(s.at("choices").is_array());
          }
        }
        REQUIRE(sawNumber);
        REQUIRE(sawChoice);
      }
    }
  }
}

SCENARIO("The device list matches what --device accepts") {
  GIVEN("a usable discrete GPU, an integrated one and a device below Vulkan 1.2") {
    std::vector<cj::DeviceRow> rows(3);
    rows[0] = {0, "Big GPU", "aaaa", "Driver A", 16ull << 30, 1, 4, true, true};
    rows[1] = {1, "Small GPU", "bbbb", "Driver B", 4ull << 30, 1, 2, false, true};
    rows[2] = {std::nullopt, "Old GPU", "cccc", "Driver C", 1ull << 30, 1, 1, true, false};
    WHEN("they are listed") {
      const Json j = Json::parse(cj::devicesList("9.9.9", rows));
      THEN("each row has its index, memory, Vulkan version and usable flag; the old one has no index") {
        requireHeader(j);
        const auto& d = j.at("devices");
        REQUIRE(d.size() == 3);
        REQUIRE(d[0].at("index") == 0);
        REQUIRE(d[0].at("memory_bytes") == (16ull << 30));
        REQUIRE(d[0].at("vulkan") == Json::array({1, 4}));
        REQUIRE(d[0].at("discrete") == true);
        REQUIRE(d[1].at("index") == 1);
        REQUIRE(d[2].at("index").is_null());
        REQUIRE(d[2].at("usable") == false);
      }
    }
  }
  GIVEN("no devices") {
    THEN("the list is an empty array, not missing") {
      const Json j = Json::parse(cj::devicesList("9.9.9", {}));
      REQUIRE(j.at("devices").is_array());
      REQUIRE(j.at("devices").empty());
    }
  }
}

SCENARIO("A dry run and a run share one set of facts") {
  GIVEN("facts of a 20 M cell model on a GPU") {
    cj::RunFacts f;
    f.profile = "bluetooth";
    f.variant = "Bluetooth Low Energy";
    f.variantStatus = "verified";
    f.bandHz = {2.4e9, 2.4835e9};
    f.sweepHz = {2.0e9, 3.0e9};
    f.excitationHz = {1.9e9, 3.1e9};
    f.cells = {474, 327, 127};
    f.cellCount = 474ull * 327 * 127;
    f.smallestCellM = 3.5e-5;
    f.timeStepS = 7.75e-14;
    f.estimatedSteps = 120000;
    f.estimatedMemoryBytes = 2.0e9;
    f.estimatedSeconds = 900;
    f.backend = "gpu";
    f.device = "Apple M1 Pro";
    f.changedSettings = {"mesh_preset"};
    f.notes = {"a note"};
    WHEN("it is written as a dry run") {
      const Json j = Json::parse(cj::dryRun("9.9.9", f));
      THEN("it is one document with the plan, the estimate, the changed settings and the notes") {
        requireHeader(j);
        REQUIRE(j.at("mode") == "dry_run");
        REQUIRE(j.at("grid").at("cells") == Json::array({474, 327, 127}));
        REQUIRE(j.at("grid").at("cell_count") == 474ull * 327 * 127);
        REQUIRE(j.at("grid").at("smallest_cell_m").get<double>() == Approx(3.5e-5));
        REQUIRE(j.at("estimate").at("steps") == 120000);
        REQUIRE(j.at("estimate").at("exceeds_memory") == false);
        REQUIRE(j.at("backend") == "gpu");
        REQUIRE(j.at("device") == "Apple M1 Pro");
        REQUIRE(j.at("changed_settings") == Json::array({"mesh_preset"}));
        REQUIRE(j.at("notes") == Json::array({"a note"}));
        REQUIRE_FALSE(j.contains("results_dir"));
      }
    }
    WHEN("it is written for a finished run") {
      cj::RunOutcome o;
      o.runDir = "/x/run-1";
      o.steps = 118000;
      o.timeStepS = 7.75e-14;
      o.wallSeconds = 900.5;
      o.mcellsPerSecond = 1250;
      o.bestHz = 2.3664e9;
      o.bestDb = -33.2;
      const Json j = Json::parse(cj::run("9.9.9", f, o));
      THEN("it adds the results folder, the measured speed and the best match") {
        requireHeader(j);
        REQUIRE(j.at("mode") == "run");
        REQUIRE(j.at("results_dir") == "/x/run-1");
        REQUIRE(j.at("steps") == 118000);
        REQUIRE(j.at("mcells_per_second").get<double>() == Approx(1250));
        REQUIRE(j.at("best_match").at("db").get<double>() == Approx(-33.2));
        REQUIRE(j.at("best_match").at("freq_hz").get<double>() == Approx(2.3664e9));
        REQUIRE(j.at("exit_status") == 0);
        REQUIRE(j.at("grid").at("cells") == Json::array({474, 327, 127}));
      }
    }
  }
}

SCENARIO("A saved run folder is summarized without solving") {
  GIVEN("a run folder with a summary, settings and an S11 file with a -20 dB dip at 2.44 GHz") {
    const fs::path dir = makeRun("modern", false);
    WHEN("it is read") {
      const Json j = Json::parse(cj::runInfo("9.9.9", dir, false));
      THEN("the board, profile, grid, steps, settings and notes come from the files") {
        requireHeader(j);
        REQUIRE(j.at("mode") == "run_info");
        REQUIRE(j.at("board") == "testboard");
        REQUIRE(j.at("profile") == "bluetooth");
        REQUIRE(j.at("variant") == "Bluetooth Low Energy");
        REQUIRE(j.at("variant_status") == "verified");
        REQUIRE(j.at("band_hz") == Json::array({2.4e9, 2.4835e9}));
        REQUIRE(j.at("grid").at("cells") == Json::array({100, 80, 40}));
        REQUIRE(j.at("grid").at("smallest_cell_m").get<double>() == Approx(3.5e-5));
        REQUIRE(j.at("steps") == 12345);
        REQUIRE(j.at("time_step_s").get<double>() == Approx(7.75e-14));
        REQUIRE(j.at("backend") == "gpu");
        REQUIRE(j.at("changed_settings") == Json::array({"mesh_preset", "decay_threshold"}));
        REQUIRE(j.at("notes") == Json::array({"a note"}));
        REQUIRE(j.at("settings").at("format") == "kicad-em-fdtd.settings");
        REQUIRE(j.at("files").at("board") == false);
      }
      THEN("the S11 summary finds the deepest point and the -10 dB band around it, but no table") {
        const auto& s = j.at("s11");
        REQUIRE(s.at("points") == 101);
        REQUIRE(s.at("f_min_hz").get<double>() == Approx(2.0e9));
        REQUIRE(s.at("f_max_hz").get<double>() == Approx(3.0e9));
        REQUIRE(s.at("best").at("freq_hz").get<double>() == Approx(2.44e9).margin(1e3));
        REQUIRE(s.at("best").at("db").get<double>() == Approx(-20.0).margin(0.01));
        const double lo = s.at("band_10db").at("low_hz"), hi = s.at("band_10db").at("high_hz");
        REQUIRE(lo < 2.44e9);
        REQUIRE(hi > 2.44e9);
        REQUIRE(s.at("band_10db").at("width_hz").get<double>() == Approx(hi - lo));
        REQUIRE_FALSE(s.contains("table"));
      }
    }
    WHEN("the table is asked for") {
      const Json j = Json::parse(cj::runInfo("9.9.9", dir, true));
      THEN("every point is there with its frequency and real and imaginary parts") {
        const auto& t = j.at("s11").at("table");
        REQUIRE(t.size() == 101);
        REQUIRE(t[0].at("f_hz").get<double>() == Approx(2.0e9));
        REQUIRE(t[0].contains("re"));
        REQUIRE(t[0].contains("im"));
      }
    }
  }
  GIVEN("a summary in the older wording written by the headless run") {
    const fs::path dir = makeRun("legacy", true);
    THEN("the steps and time step are still read, and the absent band is null") {
      const Json j = Json::parse(cj::runInfo("9.9.9", dir, false));
      REQUIRE(j.at("steps") == 12345);
      REQUIRE(j.at("time_step_s").get<double>() == Approx(7.75e-14));
      REQUIRE(j.at("band_hz").is_null());
    }
  }
  GIVEN("a folder that is not a run, and one that does not exist") {
    const fs::path empty = scratch("empty");
    THEN("the call throws with a message that names the folder") {
      REQUIRE_THROWS_WITH(cj::runInfo("9.9.9", empty, false), Catch::Matchers::ContainsSubstring("empty"));
      REQUIRE_THROWS(cj::runInfo("9.9.9", empty / "nope", false));
    }
  }
  GIVEN("a failure") {
    THEN("it is a document with an error and the header") {
      const Json j = Json::parse(cj::error("9.9.9", "bad thing"));
      requireHeader(j);
      REQUIRE(j.at("error") == "bad thing");
    }
  }
}
