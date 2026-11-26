// BDD scenarios for radio profiles, the settings registry (defaults, overrides, reset, persistence, search)
// and the debouncer behind the search box. Profile numbers come from data/radio_profiles.json.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <fstream>
#include <sstream>

#include "app/paths.hpp"
#include "app/prepare.hpp"
#include "app/selection_store.hpp"
#include "app/settings.hpp"
#include "board/snapshot.hpp"

using Catch::Approx;
using Catch::Matchers::ContainsSubstring;

namespace {
const app::Catalog& catalog() {
  static const app::Catalog c = app::Catalog::load(std::string(FDTD_DATA_DIR) + "/radio_profiles.json");
  return c;
}
bool contains(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}
} // namespace

SCENARIO("Radio profiles are loaded from the data file") {
  GIVEN("the shipped radio_profiles.json") {
    THEN("it has the six profiles, with Wi-Fi 2.4 GHz as the default") {
      for (const char* id : {"wifi", "bluetooth", "zigbee", "lwmesh", "uwb", "custom"})
        REQUIRE(contains(catalog().profileIds(), id));
      REQUIRE(catalog().defaultProfile() == "wifi");
      const auto& v = catalog().variant("wifi", "");
      REQUIRE(v.bandHz[0] == Approx(2.4e9));
      REQUIRE(v.bandHz[1] == Approx(2.4835e9));
    }
    THEN("every variant states its source and whether it is verified") {
      for (const auto& id : catalog().profileIds())
        for (const auto& vid : catalog().variantIds(id)) {
          REQUIRE(!catalog().variant(id, vid).source.empty());
          REQUIRE(!catalog().variant(id, vid).status.empty());
        }
    }
    THEN("UWB covers channels 5 and 9 and Custom starts at the 2.4 GHz ISM band") {
      const auto& uwb = catalog().variant("uwb", "");
      REQUIRE(uwb.bandHz[0] <= 6.24e9);
      REQUIRE(uwb.bandHz[1] >= 8.2368e9);
      REQUIRE(catalog().variant("custom", "").bandHz[0] == Approx(2.4e9));
    }
  }
  GIVEN("a data file that is not valid") {
    THEN("loading names the problem") {
      REQUIRE_THROWS_WITH(app::Catalog::fromJson("{\"profiles\":{}}"), ContainsSubstring("default_profile"));
    }
  }
}

SCENARIO("The run plan follows from the band and the mesh preset, never from constants in code") {
  GIVEN("default settings (Wi-Fi 2.4 GHz, medium mesh) on a board whose highest permittivity is 4.5") {
    app::Settings s(catalog());
    const auto plan = app::makePlan(s, 4.5);
    THEN("the reported sweep is the band widened by 15 % of the frequency at each end") {
      REQUIRE(plan.sweepHz[0] == Approx(2.4e9 * 0.85));
      REQUIRE(plan.sweepHz[1] == Approx(2.4835e9 * 1.15));
    }
    THEN("the excitation band is the sweep widened by 25 % of its width at each end") {
      const double w = plan.sweepHz[1] - plan.sweepHz[0];
      REQUIRE(plan.excitationHz[0] == Approx(plan.sweepHz[0] - 0.25 * w));
      REQUIRE(plan.excitationHz[1] == Approx(plan.sweepHz[1] + 0.25 * w));
    }
    THEN("the coarse cell is lambda_min in the dielectric over 20 cells per wavelength") {
      const double lambdaMin = 299792458.0 / (plan.sweepHz[1] * std::sqrt(4.5));
      REQUIRE(plan.coarseCell == Approx(lambdaMin / 20).epsilon(1e-6));
    }
  }
  GIVEN("the fine preset") {
    app::Settings s(catalog());
    s.set("mesh_preset", std::string("fine"));
    THEN("the coarse cell shrinks to lambda_min over 30") {
      const auto plan = app::makePlan(s, 4.5);
      REQUIRE(plan.coarseCell == Approx(299792458.0 / (plan.sweepHz[1] * std::sqrt(4.5)) / 30).epsilon(1e-6));
    }
  }
}

SCENARIO("A user can adjust settings, see where each value comes from, and reset to the defaults") {
  GIVEN("default settings") {
    app::Settings s(catalog());
    REQUIRE(s.source("band_min_ghz") == app::Source::Profile);
    WHEN("the user switches to Bluetooth") {
      s.selectProfile("bluetooth");
      THEN("the band follows the profile") {
        REQUIRE(s.number("band_min_ghz") == Approx(2.4));
        REQUIRE(s.text("profile") == "bluetooth");
      }
    }
    WHEN("the user edits the band and the port impedance") {
      s.set("band_min_ghz", 2.3);
      s.set("port_impedance_ohm", 75.0);
      THEN("the values change and are marked as the user's") {
        REQUIRE(s.number("band_min_ghz") == Approx(2.3));
        REQUIRE(s.source("band_min_ghz") == app::Source::User);
        REQUIRE(s.source("port_impedance_ohm") == app::Source::User);
      }
      AND_WHEN("one setting is reset") {
        s.reset("port_impedance_ohm");
        THEN("only that one returns to its default") {
          REQUIRE(s.number("port_impedance_ohm") == Approx(50.0));
          REQUIRE(s.source("port_impedance_ohm") == app::Source::Default);
          REQUIRE(s.number("band_min_ghz") == Approx(2.3));
        }
      }
      AND_WHEN("everything is reset") {
        s.resetAll();
        THEN("all values are back to the profile defaults") {
          REQUIRE(s.number("band_min_ghz") == Approx(2.4));
          REQUIRE(s.source("band_min_ghz") == app::Source::Profile);
          REQUIRE(s.modifiedKeys().empty());
        }
      }
    }
    WHEN("a value is out of range or not one of the choices") {
      THEN("the change is refused and the message names the setting") {
        REQUIRE_THROWS_WITH(s.set("port_impedance_ohm", -5.0), ContainsSubstring("port_impedance_ohm"));
        REQUIRE_THROWS_WITH(s.set("mesh_preset", std::string("ultra")), ContainsSubstring("mesh_preset"));
        REQUIRE_THROWS_WITH(s.set("no_such_setting", 1.0), ContainsSubstring("no_such_setting"));
      }
    }
    WHEN("the user sets a band whose minimum is above its maximum") {
      s.set("band_min_ghz", 3.0);
      THEN("planning refuses with a clear message") {
        REQUIRE_THROWS_WITH(app::makePlan(s, 4.5), ContainsSubstring("band"));
      }
    }
  }
}

SCENARIO("Settings are saved as the user's deviations only and restored without losing anything") {
  GIVEN("settings with two changes") {
    app::Settings s(catalog());
    s.selectProfile("uwb");
    s.set("mesh_preset", std::string("coarse"));
    s.set("sweep_points", 201.0);
    const std::string saved = s.toJson();
    THEN("the saved text holds the profile and the changed values, not the defaults") {
      REQUIRE_THAT(saved, ContainsSubstring("uwb"));
      REQUIRE_THAT(saved, ContainsSubstring("sweep_points"));
      REQUIRE_THAT(saved, !ContainsSubstring("port_impedance_ohm"));
    }
    WHEN("it is loaded into fresh settings") {
      app::Settings t(catalog());
      std::vector<std::string> problems;
      t.fromJson(saved, problems);
      THEN("profile and changes are restored and nothing is reported") {
        REQUIRE(t.text("profile") == "uwb");
        REQUIRE(t.text("mesh_preset") == "coarse");
        REQUIRE(t.number("sweep_points") == Approx(201.0));
        REQUIRE(problems.empty());
      }
    }
    WHEN("a saved file holds a setting this version does not know") {
      app::Settings t(catalog());
      std::vector<std::string> problems;
      t.fromJson(R"({"profile":"wifi","values":{"future_setting":1,"sweep_points":99}})", problems);
      THEN("it is reported, not silently dropped, and the known values still load") {
        REQUIRE(problems.size() == 1);
        REQUIRE_THAT(problems[0], ContainsSubstring("future_setting"));
        REQUIRE(t.number("sweep_points") == Approx(99.0));
      }
    }
  }
}

SCENARIO("Advanced settings are revealed in tiers and can be found by search") {
  GIVEN("default settings") {
    app::Settings s(catalog());
    THEN("only the essential settings are shown at first") {
      const auto shown = s.visible({}, "");
      REQUIRE(contains(shown, "profile"));
      REQUIRE(contains(shown, "band_min_ghz"));
      REQUIRE(contains(shown, "mesh_preset"));
      REQUIRE(!contains(shown, "pml_cells"));
    }
    THEN("expanding a group reveals the settings of the next tier in that group only") {
      const auto shown = s.visible({"Numerics"}, "");
      REQUIRE(contains(shown, "pml_cells"));
      REQUIRE(!contains(shown, "port_impedance_ohm"));
    }
    THEN("searching finds a hidden setting by a word in its label, help or keywords, whatever its tier") {
      REQUIRE(contains(s.visible({}, "absorbing"), "pml_cells"));
      REQUIRE(contains(s.visible({}, "IMPEDANCE"), "port_impedance_ohm"));
      REQUIRE(contains(s.visible({}, "resolution"), "mesh_preset"));
    }
    THEN("a search with no match shows nothing, and the groups of matches are reported for expansion") {
      REQUIRE(s.visible({}, "zzzzqq").empty());
      REQUIRE(s.groupOf("pml_cells") == "Numerics");
    }
  }
}

SCENARIO("The search box waits until the user stops typing") {
  GIVEN("a debouncer with a 200 ms delay") {
    app::Debouncer d(0.2);
    WHEN("the user types three letters 50 ms apart") {
      d.update("a", 0.00);
      d.update("ab", 0.05);
      d.update("abc", 0.10);
      THEN("nothing is released while typing continues") {
        REQUIRE(!d.poll(0.20).has_value());
        REQUIRE(!d.poll(0.29).has_value());
      }
      THEN("the final text is released once, 200 ms after the last keystroke") {
        const auto text = d.poll(0.31);
        REQUIRE(text.has_value());
        REQUIRE(*text == "abc");
        REQUIRE(!d.poll(0.50).has_value());
      }
    }
    WHEN("the text is cleared") {
      d.update("abc", 0.0);
      (void)d.poll(0.3);
      d.update("", 1.0);
      THEN("the empty search is released too, so the list returns to normal") {
        const auto text = d.poll(1.3);
        REQUIRE(text.has_value());
        REQUIRE(text->empty());
      }
    }
  }
}

SCENARIO("Results are organised in a folder at the base of the KiCAD project") {
  GIVEN("a project folder") {
    const auto dir = std::filesystem::path(FDTD_TEST_TMP) / "emfdtd_paths_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const app::ProjectPaths paths(dir);
    THEN("settings, selection and results all live under <project>/em-fdtd") {
      REQUIRE(paths.settingsFile == dir / "em-fdtd" / "settings.json");
      REQUIRE(paths.selectionFile == dir / "em-fdtd" / "selection.json");
      REQUIRE(paths.resultsRoot == dir / "em-fdtd" / "results");
    }
    WHEN("two runs start in the same second") {
      const auto a = paths.newRunDir("20260101-120000");
      const auto b = paths.newRunDir("20260101-120000");
      THEN("each gets its own folder and the first is not touched") {
        REQUIRE(a != b);
        REQUIRE(std::filesystem::is_directory(a));
        REQUIRE(std::filesystem::is_directory(b));
        REQUIRE(a.filename() == "run-20260101-120000");
      }
    }
    std::filesystem::remove_all(dir);
  }
  GIVEN("a folder that does not exist, or a KiCAD backups folder") {
    THEN("creating the paths refuses with a message") {
      REQUIRE_THROWS_WITH(app::ProjectPaths("/no/such/project/folder"), ContainsSubstring("does not exist"));
      const auto backups = std::filesystem::path(FDTD_TEST_TMP) / "board-backups";
      std::filesystem::create_directories(backups);
      REQUIRE_THROWS_WITH(app::ProjectPaths(backups), ContainsSubstring("backups"));
      std::filesystem::remove_all(backups);
    }
  }
}

SCENARIO("A board is prepared and estimated from the settings alone") {
  GIVEN("a small two-layer board with a vertical port and the default settings") {
    const auto snap =
        board::parseSnapshot(R"({"format":"kicad-em-fdtd.snapshot","version":1,"board":{"name":"t"},
      "outline":[[0,0],[0.03,0],[0.03,0.03],[0,0.03]],
      "stackup":[{"name":"F.Cu","kind":"copper"},
                 {"name":"core","kind":"dielectric","thickness":0.0016,"epsilon_r":4.5,"loss_tangent":0.02},
                 {"name":"B.Cu","kind":"copper"}],
      "copper":[{"layer":"B.Cu","owner":"","net":"G","polygon":[[-0.001,-0.001],[0.031,-0.001],[0.031,0.031],[-0.001,0.031]]},
                {"layer":"F.Cu","owner":"","net":"S","polygon":[[0.010,0.010],[0.020,0.010],[0.020,0.020],[0.010,0.020]]}],
      "vias":[],"components":[],
      "ports":[{"name":"P1","layer":"F.Cu","layer_b":"B.Cu","a":[0.015,0.012],"width":0,"impedance":50}]})");
    app::Settings s(catalog());
    const auto p = app::prepare(snap, s, board::Selection{});
    THEN("the grid size, band and cells all follow from the profile and the board") {
      REQUIRE(p.options.fMinHz == Approx(2.4e9 * 0.85));
      REQUIRE(p.model.grid.cells(fdtd::X) > 10);
      REQUIRE(p.plan.dielectricCell <= 1.6e-3 / 3 * (1 + 1e-9)); // at least 3 cells through the 1.6 mm layer
    }
    THEN("the estimate reports cells, memory and a positive time, and flags a memory budget that is too "
         "small") {
      const auto e = app::estimate(p, true, 16e9);
      REQUIRE(e.cells == p.model.grid.cellCount());
      REQUIRE(e.seconds > 0);
      REQUIRE(e.memoryBytes > 28.0 * static_cast<double>(e.cells));
      REQUIRE(!e.exceedsMemory);
      REQUIRE(app::estimate(p, true, 1e5).exceedsMemory);
    }
  }
}

SCENARIO("Field colours are settings with per-theme defaults the user can change and reset") {
  GIVEN("default settings") {
    app::Settings s(catalog());
    THEN("dark defaults are a blue (negative) and yellow (positive) pair, light defaults blue and red") {
      const auto dn = app::parseColour(s.text("colour_dark_negative")),
                 dp = app::parseColour(s.text("colour_dark_positive"));
      const auto ln = app::parseColour(s.text("colour_light_negative")),
                 lp = app::parseColour(s.text("colour_light_positive"));
      REQUIRE(dn[2] > dn[0] + 80);  // sky blue: blue well above red
      REQUIRE(dp[0] > dp[2] + 100); // yellow: red and green well above blue
      REQUIRE(dp[1] > dp[2] + 100);
      REQUIRE(ln[2] > ln[0] + 60); // blue
      REQUIRE(lp[0] > lp[2] + 60); // red
    }
    WHEN("the user picks their own colour and saves and reloads the settings") {
      s.set("colour_dark_positive", std::string("#FFA500"));
      const std::string saved = s.toJson();
      app::Settings t(catalog());
      std::vector<std::string> problems;
      t.fromJson(saved, problems);
      THEN("it is kept, marked as the user's, and the other colours keep their defaults") {
        REQUIRE(t.text("colour_dark_positive") == "#FFA500");
        REQUIRE(t.source("colour_dark_positive") == app::Source::User);
        REQUIRE(t.source("colour_dark_negative") == app::Source::Default);
        REQUIRE(problems.empty());
      }
      AND_WHEN("it is reset") {
        t.reset("colour_dark_positive");
        THEN("the theme default returns") {
          REQUIRE(t.text("colour_dark_positive") == s.catalog().uiColours().at("dark_positive"));
        }
      }
    }
    WHEN("a colour is not in #RRGGBB form") {
      THEN("the change is refused and the message names the setting") {
        REQUIRE_THROWS_WITH(s.set("colour_light_negative", std::string("blue")),
                            ContainsSubstring("colour_light_negative"));
        REQUIRE_THROWS_WITH(s.set("colour_light_negative", std::string("#12")), ContainsSubstring("#RRGGBB"));
        REQUIRE_THROWS_WITH(s.set("colour_light_negative", 5.0), ContainsSubstring("colour_light_negative"));
      }
    }
  }
}

SCENARIO("The slice and 3D view panels are settings that follow the theme by default") {
  app::Settings s(catalog());
  THEN("both default to following the theme and accept light or dark, nothing else") {
    REQUIRE(s.text("slice_panel") == "follow");
    REQUIRE(s.text("volume_panel") == "follow");
    s.set("volume_panel", std::string("light"));
    REQUIRE(s.text("volume_panel") == "light");
    REQUIRE_THROWS(s.set("slice_panel", std::string("purple")));
  }
}

namespace {
board::Snapshot stackupBoard(int copperLayers, const std::string& copperExtra = "") {
  std::string st = R"({"name":"F.Cu","kind":"copper")" + copperExtra + "}";
  for (int i = 1; i < copperLayers; ++i)
    st += R"(,{"name":"d)" + std::to_string(i) +
          R"(","kind":"dielectric","thickness":0.0005,"epsilon_r":4.2,"loss_tangent":0.015},{"name":"c)" +
          std::to_string(i) + R"(","kind":"copper")" + copperExtra + "}";
  return board::parseSnapshot(R"({"format":"kicad-em-fdtd.snapshot","version":1,"board":{"name":"t"},
      "outline":[[0,0],[0.03,0],[0.03,0.03],[0,0.03]],"stackup":[)" +
                              st + R"(],"copper":[],"vias":[],"components":[],"ports":[]})");
}
} // namespace

SCENARIO("PCB properties are editable settings with fab profiles as defaults") {
  GIVEN("the shipped fab profiles") {
    app::Settings s(catalog());
    THEN("OSH Park and PCBWay profiles exist for the layer counts their pages publish") {
      const auto ids = s.choices("pcb_profile");
      for (const char* id :
           {"board", "oshpark_2layer", "oshpark_4layer", "oshpark_6layer", "pcbway_2layer", "pcbway_4layer"})
        REQUIRE(std::find(ids.begin(), ids.end(), id) != ids.end());
      REQUIRE(catalog().fabProfile("oshpark_6layer").dielectrics.size() == 5);
      REQUIRE(catalog().fabProfile("pcbway_4layer").dielectrics[1].thickness == Approx(1.03e-3));
    }
    THEN("the default is the board's own stackup and every PCB value is unset") {
      REQUIRE(s.text("pcb_profile") == "board");
      REQUIRE(s.number("diel1_thickness_mm") == 0);
      auto b = stackupBoard(4);
      REQUIRE(app::applyPcbSettings(s, b).empty());
      REQUIRE(b.stackup[1].epsilonR == Approx(4.2));
    }
  }
  GIVEN("a 4-layer board and the OSH Park 4-layer profile") {
    app::Settings s(catalog());
    s.set("pcb_profile", std::string("oshpark_4layer"));
    THEN("the setting defaults come from the profile and say so") {
      REQUIRE(s.number("diel2_thickness_mm") == Approx(0.9906));
      REQUIRE(s.source("diel2_thickness_mm") == app::Source::Profile);
      REQUIRE(s.number("diel4_thickness_mm") == 0);
    }
    THEN("the board's dielectrics take the profile's values, with assumptions named") {
      auto b = stackupBoard(4);
      const auto notes = app::applyPcbSettings(s, b);
      REQUIRE(b.stackup[1].thickness == Approx(0.1999e-3));
      REQUIRE(b.stackup[3].thickness == Approx(0.9906e-3));
      REQUIRE(b.stackup[3].epsilonR == Approx(3.87)); // FR408HR core, from OSH Park's own stackup diagram
      const std::string all = [&] {
        std::string t;
        for (const auto& n : notes)
          t += n + "\n";
        return t;
      }();
      REQUIRE_THAT(all, ContainsSubstring("OSH Park 4-layer"));
      REQUIRE_THAT(all, ContainsSubstring("assumed"));
    }
    THEN("a value the user types wins over the profile") {
      s.set("diel2_thickness_mm", 0.8);
      REQUIRE(s.source("diel2_thickness_mm") == app::Source::User);
      auto b = stackupBoard(4);
      app::applyPcbSettings(s, b);
      REQUIRE(b.stackup[3].thickness == Approx(0.8e-3));
    }
    THEN("on a board with another layer count the profile is refused but typed values still apply") {
      s.set("diel1_epsilon_r", 3.9);
      auto b = stackupBoard(2);
      const auto notes = app::applyPcbSettings(s, b);
      REQUIRE(b.stackup[1].thickness == Approx(0.5e-3));
      REQUIRE(b.stackup[1].epsilonR == Approx(3.9));
      REQUIRE_THAT(notes.front(), ContainsSubstring("not applied"));
    }
    THEN("resetting returns to the board's values") {
      s.reset("pcb_profile");
      REQUIRE(s.number("diel2_thickness_mm") == 0);
    }
  }
  GIVEN("an edit with no profile") {
    app::Settings s(catalog());
    s.set("diel1_loss_tangent", 0.004);
    THEN("only that value changes, and a layer the board lacks is reported") {
      auto b = stackupBoard(2);
      app::applyPcbSettings(s, b);
      REQUIRE(b.stackup[1].lossTangent == Approx(0.004));
      REQUIRE(b.stackup[1].epsilonR == Approx(4.2));
      s.set("diel5_epsilon_r", 3.0);
      REQUIRE_THAT(app::applyPcbSettings(s, b).back(), ContainsSubstring("does not exist"));
    }
    THEN("out-of-range values are refused") {
      REQUIRE_THROWS(s.set("diel1_thickness_mm", -1.0));
      REQUIRE_THROWS(s.set("pcb_profile", std::string("nobody")));
    }
  }
}

SCENARIO("Component bodies are an option that defaults to off and follows the settings") {
  GIVEN("a board with a 6 mm square part, 3 mm tall, on the top side") {
    const auto snap =
        board::parseSnapshot(R"({"format":"kicad-em-fdtd.snapshot","version":1,"board":{"name":"t"},
      "outline":[[0,0],[0.03,0],[0.03,0.03],[0,0.03]],
      "stackup":[{"name":"F.Cu","kind":"copper"},
                 {"name":"core","kind":"dielectric","thickness":0.0016,"epsilon_r":4.5,"loss_tangent":0.02},
                 {"name":"B.Cu","kind":"copper"}],
      "copper":[{"layer":"B.Cu","owner":"","net":"G","polygon":[[-0.001,-0.001],[0.031,-0.001],[0.031,0.031],[-0.001,0.031]]},
                {"layer":"F.Cu","owner":"","net":"S","polygon":[[0.010,0.010],[0.020,0.010],[0.020,0.020],[0.010,0.020]]}],
      "vias":[],
      "components":[{"ref":"U1","value":"can","x":0.022,"y":0.022,"layer":"F.Cu",
                     "body":{"polygon":[[0.019,0.019],[0.025,0.019],[0.025,0.025],[0.019,0.025]],"height":0.003}}],
      "ports":[{"name":"P1","layer":"F.Cu","layer_b":"B.Cu","a":[0.015,0.012],"width":0,"impedance":50}]})");
    auto bodyCells = [](const app::Prepared& p) {
      std::size_t n = 0;
      for (const auto id : p.model.cellMaterial)
        n += id >= 2; // air, the board dielectric, then bodies
      return n;
    };
    WHEN("the settings are left at their defaults") {
      app::Settings s(catalog());
      const auto p = app::prepare(snap, s, board::Selection{});
      THEN("the part's body is not modelled") {
        REQUIRE(s.text("component_bodies") == "ignore");
        REQUIRE(bodyCells(p) == 0);
      }
    }
    WHEN("component bodies are set to dielectric with permittivity 9") {
      app::Settings s(catalog());
      s.set("component_bodies", std::string("dielectric"));
      s.set("component_body_eps", 9.0);
      const auto p = app::prepare(snap, s, board::Selection{});
      THEN("the part's footprint is filled with permittivity 9 above the board, and nothing else changes") {
        REQUIRE(bodyCells(p) > 0);
        bool nine = false;
        for (const auto& m : p.model.materials)
          nine = nine || m.epsR == Approx(9.0);
        REQUIRE(nine);
      }
    }
    WHEN("component bodies are set to metal") {
      app::Settings s(catalog());
      s.set("component_bodies", std::string("metal"));
      const auto p = app::prepare(snap, s, board::Selection{});
      THEN("the body cells are perfect conductor") {
        REQUIRE(p.model.materials.back().pec);
        REQUIRE(bodyCells(p) > 0);
      }
    }
  }
}

SCENARIO("The model selection is saved next to the board and restored exactly") {
  GIVEN("a selection with picked nets, a bead's values, an open part and a metal body") {
    app::StoredSelection sel;
    sel.pickedNets = {"RF_FEED", "ANT"};
    sel.extraGroundNets = {"SHIELD"};
    sel.parts["FB1"] = {board::PartTreatment::State::Values, false, 600.0, 100e-9, 0.2e-12};
    sel.parts["R9"].state = board::PartTreatment::State::Open;
    sel.parts["R5"].fitDnp = true;
    sel.bodies["U2"] = {board::BodyKind::Metal, 1.0};
    sel.defaultBody = {board::BodyKind::Dielectric, 6.5};
    sel.refinement["gap|F.Cu|FEED|GND|10.0|10.0"] = {true, 0.04};
    sel.refinement["gap|F.Cu|A|B|3.5|4.5"] = {false, 0.0};
    const auto dir = std::filesystem::path(FDTD_TEST_TMP) / "selection-store";
    std::filesystem::create_directories(dir);
    const auto file = dir / "em-fdtd" / "selection.json";
    WHEN("it is saved and read back") {
      app::saveSelection(file, sel);
      const auto back = app::loadSelection(file);
      THEN("every value comes back exactly") {
        REQUIRE(back.pickedNets == sel.pickedNets);
        REQUIRE(back.extraGroundNets == sel.extraGroundNets);
        REQUIRE(back.parts.at("FB1").state == board::PartTreatment::State::Values);
        REQUIRE(*back.parts.at("FB1").r == 600.0);
        REQUIRE(*back.parts.at("FB1").l == 100e-9);
        REQUIRE(*back.parts.at("FB1").c == 0.2e-12);
        REQUIRE(back.parts.at("R9").state == board::PartTreatment::State::Open);
        REQUIRE(back.parts.at("R5").fitDnp);
        REQUIRE(back.bodies.at("U2").kind == board::BodyKind::Metal);
        REQUIRE(back.defaultBody.kind == board::BodyKind::Dielectric);
        REQUIRE(back.defaultBody.epsilonR == 6.5);
        REQUIRE(back.refinement.at("gap|F.Cu|FEED|GND|10.0|10.0").enabled);
        REQUIRE(back.refinement.at("gap|F.Cu|FEED|GND|10.0|10.0").cellMm == 0.04);
        REQUIRE_FALSE(back.refinement.at("gap|F.Cu|A|B|3.5|4.5").enabled);
      }
    }
    WHEN("the file does not exist") {
      THEN("the selection is empty") {
        REQUIRE(app::loadSelection(dir / "none" / "selection.json").pickedNets.empty());
      }
    }
    WHEN("the file is not a selection file") {
      std::filesystem::create_directories(dir / "bad");
      std::ofstream(dir / "bad" / "selection.json") << "{\"format\":\"something else\"}";
      THEN("it is refused, not silently replaced") {
        REQUIRE_THROWS_AS(app::loadSelection(dir / "bad" / "selection.json"), app::SettingsError);
      }
    }
  }
}

SCENARIO("The sequential colour maps are data with a source and a licence, and match their known ends") {
  GIVEN("the catalogue loaded from data/") {
    const auto& maps = catalog().colourMaps();
    THEN("viridis, magma, inferno and plasma are there with 256 entries each") {
      for (const char* id : {"viridis", "magma", "inferno", "plasma"}) {
        REQUIRE(maps.count(id) == 1);
        REQUIRE(maps.at(id).colours.size() == 256);
      }
    }
    AND_THEN("their end colours are the published ones within 1 level per channel (viridis 440154 to FDE725, "
             "magma 000004 to FCFDBF, inferno 000004 to FCFFA4, plasma 0D0887 to F0F921)") {
      auto near = [](const std::array<std::uint8_t, 3>& c, int r, int g, int b) {
        return std::abs(int(c[0]) - r) <= 1 && std::abs(int(c[1]) - g) <= 1 && std::abs(int(c[2]) - b) <= 1;
      };
      REQUIRE(near(maps.at("viridis").colours.front(), 0x44, 0x01, 0x54));
      REQUIRE(near(maps.at("viridis").colours.back(), 0xFD, 0xE7, 0x25));
      REQUIRE(near(maps.at("magma").colours.front(), 0x00, 0x00, 0x04));
      REQUIRE(near(maps.at("magma").colours.back(), 0xFC, 0xFD, 0xBF));
      REQUIRE(near(maps.at("inferno").colours.front(), 0x00, 0x00, 0x04));
      REQUIRE(near(maps.at("inferno").colours.back(), 0xFC, 0xFF, 0xA4));
      REQUIRE(near(maps.at("plasma").colours.front(), 0x0D, 0x08, 0x87));
      REQUIRE(near(maps.at("plasma").colours.back(), 0xF0, 0xF9, 0x21));
    }
    AND_THEN("each map gets lighter from end to end: luma 0.2126 R + 0.7152 G + 0.0722 B never falls by more "
             "than 2 of 255 between neighbours, and ends at least 150 above where it starts (viridis, "
             "magma, inferno are designed this way; plasma is checked the same way)") {
      for (const auto& [id, def] : maps) {
        auto luma = [](const std::array<std::uint8_t, 3>& c) {
          return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
        };
        double worstFall = 0;
        for (std::size_t i = 1; i < def.colours.size(); ++i)
          worstFall = std::max(worstFall, luma(def.colours[i - 1]) - luma(def.colours[i]));
        INFO(id << " worst fall " << worstFall);
        REQUIRE(worstFall <= 2.0);
        REQUIRE(luma(def.colours.back()) - luma(def.colours.front()) >= 150);
      }
    }
  }
  GIVEN("the setting that picks the 3D colours") {
    app::Settings s(catalog());
    THEN("the default is the two-colour signed view, and the maps are the other choices") {
      REQUIRE(s.text("volume_colour_map") == "signed");
      const auto choices = s.choices("volume_colour_map");
      REQUIRE(choices.front() == "signed");
      REQUIRE(std::find(choices.begin(), choices.end(), "viridis") != choices.end());
    }
  }
}

SCENARIO("A board drawn in a panel is modelled without the frame unless the user keeps it") {
  GIVEN("a snapshot whose outline is a 30 mm panel and whose board proper is the 10 mm square inside it") {
    board::Snapshot b = board::parseSnapshot(R"({"format":"kicad-em-fdtd.snapshot","version":1,
      "board":{"name":"t"},"outline":[[0,0],[0.03,0],[0.03,0.03],[0,0.03]],
      "board_proper":{"outline":[[0.01,0.01],[0.02,0.01],[0.02,0.02],[0.01,0.02]],"cutouts":[]},
      "stackup":[{"name":"F.Cu","kind":"copper"}],
      "copper":[{"layer":"F.Cu","owner":"","net":"A","polygon":[[0.012,0.012],[0.014,0.012],[0.014,0.014]]},
                {"layer":"F.Cu","owner":"","net":"B","polygon":[[0.002,0.002],[0.004,0.002],[0.004,0.004]]}],
      "vias":[],"components":[],"ports":[]})");
    app::Settings s(catalog());
    WHEN("the default setting is used") {
      board::Snapshot copy = b;
      const auto notes = app::applyPcbSettings(s, copy);
      THEN("the outline is the board proper and the frame's copper is dropped") {
        REQUIRE(copy.outline.size() == 4);
        REQUIRE(copy.outline[0].x == Catch::Approx(0.01));
        REQUIRE(copy.copper.size() == 1);
        REQUIRE(copy.copper[0].net == "A");
        REQUIRE(!notes.empty());
      }
    }
    WHEN("the user keeps the whole outline") {
      s.set("board_outline", std::string("whole"));
      app::applyPcbSettings(s, b);
      THEN("nothing is dropped") {
        REQUIRE(b.outline[1].x == Catch::Approx(0.03));
        REQUIRE(b.copper.size() == 2);
      }
    }
  }
}

SCENARIO("Copper thickness comes from the board, the profile or the user, in that order of trust") {
  GIVEN("a 4-layer board whose snapshot gives 17.5 um on every copper layer") {
    app::Settings s(catalog());
    THEN("the snapshot's value is read and kept when nothing overrides it") {
      auto b = stackupBoard(4, R"(,"thickness":0.0000175)");
      REQUIRE(b.stackup[0].thickness == Approx(17.5e-6));
      app::applyPcbSettings(s, b);
      REQUIRE(b.stackup[0].thickness == Approx(17.5e-6));
      REQUIRE(b.stackup[2].thickness == Approx(17.5e-6));
    }
    THEN("a value the user types replaces it on the outer layers only") {
      s.set("copper_outer_um", 70.0);
      auto b = stackupBoard(4, R"(,"thickness":0.0000175)");
      const auto notes = app::applyPcbSettings(s, b);
      REQUIRE(b.stackup[0].thickness == Approx(70e-6));
      REQUIRE(b.stackup[6].thickness == Approx(70e-6)); // the last copper layer is outer too
      REQUIRE(b.stackup[2].thickness == Approx(17.5e-6));
      std::string all;
      for (const auto& n : notes)
        all += n + "\n";
      REQUIRE_THAT(all, ContainsSubstring("feed-line"));
    }
  }
  GIVEN("a snapshot with no copper thickness and the OSH Park 4-layer profile") {
    app::Settings s(catalog());
    s.set("pcb_profile", std::string("oshpark_4layer"));
    THEN("the profile's thickness fills the unknown ones") {
      auto b = stackupBoard(4);
      REQUIRE(b.stackup[0].thickness == 0);
      app::applyPcbSettings(s, b);
      REQUIRE(b.stackup[0].thickness > 0);
      REQUIRE(b.stackup[0].thickness == Approx(s.number("copper_outer_um") * 1e-6));
      REQUIRE(b.stackup[2].thickness == Approx(s.number("copper_inner_um") * 1e-6));
    }
  }
  GIVEN("a snapshot whose copper thickness is negative") {
    THEN("reading fails naming the layer") {
      REQUIRE_THROWS_WITH(stackupBoard(2, R"(,"thickness":-1e-5)"), ContainsSubstring("F.Cu"));
    }
  }
}

SCENARIO("The copper model is a setting that keeps sheets unless the user asks for slabs") {
  GIVEN("the default settings") {
    app::Settings s(catalog());
    THEN("copper is a sheet, and the plan says so") {
      REQUIRE(s.text("copper_model") == "sheet");
      const auto choices = s.choices("copper_model");
      REQUIRE(std::find(choices.begin(), choices.end(), "slab") != choices.end());
      REQUIRE_FALSE(app::makePlan(s, 4.4, 0.8e-3).copperSlab);
    }
    AND_THEN("a typo is refused, and slab reaches the plan") {
      REQUIRE_THROWS(s.set("copper_model", std::string("thick")));
      s.set("copper_model", std::string("slab"));
      REQUIRE(app::makePlan(s, 4.4, 0.8e-3).copperSlab);
    }
    AND_THEN("it is found by searching for 'copper thickness'") {
      REQUIRE(contains(s.visible({}, "slab"), "copper_model"));
    }
  }
  GIVEN("a board whose copper has a thickness and the slab model chosen") {
    app::Settings s(catalog());
    s.set("copper_model", std::string("slab"));
    auto b = stackupBoard(2, R"(,"thickness":0.000035)");
    THEN("the prepared model is built with slabs") {
      const app::Prepared p = app::prepare(b, s, board::Selection{});
      bool slab = false;
      for (const auto& n : p.model.notes)
        slab = slab || n.find("slabs of its finished thickness") != std::string::npos;
      REQUIRE(slab);
    }
  }
}
