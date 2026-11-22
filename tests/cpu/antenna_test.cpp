// BDD scenarios for the static antenna checks (docs/PLAN.md, "Reference-guided antenna tuning", Tier 1).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <sstream>

#include "board/antenna.hpp"

using Catch::Approx;

namespace {
board::Ring rect(double x0, double y0, double x1, double y1) {
  return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
}

// A 40 x 30 mm four-layer board (0.1 mm dielectrics), the antenna on F.Cu at the middle of the bottom edge.
board::Snapshot fixture() {
  board::Snapshot s;
  s.outline = rect(0, 0, 0.04, 0.03);
  for (const char* n : {"F.Cu", "In1.Cu", "In2.Cu", "B.Cu"}) {
    if (!s.stackup.empty()) {
      board::Layer d;
      d.name = "core";
      d.thickness = 1e-4;
      s.stackup.push_back(d);
    }
    board::Layer c;
    c.name = n;
    c.copper = true;
    s.stackup.push_back(c);
  }
  board::Component ant;
  ant.ref = "ANT1";
  ant.value = "W3008";
  ant.layer = "F.Cu";
  ant.x = 0.02;
  ant.y = 0.001;
  ant.pads = {{"1", "RF", 0.0192, 0.001}, {"2", "GND", 0.0208, 0.001}};
  s.components.push_back(ant);
  s.copper.push_back({"F.Cu", "ANT1", "RF", rect(0.0189, 0.0005, 0.0195, 0.0015), {}});
  return s;
}
} // namespace

SCENARIO("Copper inside the antenna keepout is reported per layer, the part's own pads are not") {
  GIVEN("a board whose inner ground pour covers the whole board, including the keepout") {
    board::Snapshot s = fixture();
    s.copper.push_back({"In1.Cu", "", "GND", rect(0, 0, 0.04, 0.03), {}});
    WHEN("the 4.00 x 4.25 mm keepout is checked") {
      const auto r = board::checkAntenna(s, s.components[0], 4.0, 4.25, {"GND"});
      THEN("In1.Cu has exactly the keepout's area of copper and no other layer is reported") {
        REQUIRE(r.ok);
        REQUIRE(r.keepout.size() == 1);
        REQUIRE(r.keepout[0].layer == "In1.Cu");
        REQUIRE(r.keepout[0].areaMm2 == Approx(4.0 * 4.25).epsilon(2e-3));
        REQUIRE(r.keepout[0].nets.count("GND") == 1);
        REQUIRE(r.edgeDistanceMm == Approx(1.0).epsilon(2e-3));
      }
    }
  }
  GIVEN("the same pour with a notch that clears the keepout and a hole in its corner") {
    board::Snapshot s = fixture();
    board::Ring notched{{0, 0},     {0.018, 0}, {0.018, 0.0045}, {0.022, 0.0045},
                        {0.022, 0}, {0.04, 0},  {0.04, 0.03},    {0, 0.03}};
    s.copper.push_back({"In1.Cu", "", "GND", notched, {}});
    THEN("nothing is reported") {
      const auto r = board::checkAntenna(s, s.components[0], 4.0, 4.25, {"GND"});
      REQUIRE(r.ok);
      REQUIRE(r.keepout.empty());
    }
  }
  GIVEN("a pour with a 1 x 1 mm hole inside the keepout") {
    board::Snapshot s = fixture();
    s.copper.push_back({"In2.Cu", "", "GND", rect(0, 0, 0.04, 0.03), {rect(0.0195, 0.001, 0.0205, 0.002)}});
    THEN("the hole's area is not counted") {
      const auto r = board::checkAntenna(s, s.components[0], 4.0, 4.25, {"GND"});
      REQUIRE(r.keepout.size() == 1);
      REQUIRE(r.keepout[0].areaMm2 == Approx(4.0 * 4.25 - 1.0).epsilon(2e-3));
    }
  }
}

SCENARIO("Vias, ground size and stackup are summarised") {
  GIVEN("the board with a 40 x 30 mm ground on In1.Cu and one via in the keepout") {
    board::Snapshot s = fixture();
    s.copper.push_back({"In1.Cu", "", "GND", rect(0.0, 0.006, 0.04, 0.03), {}});
    s.vias.push_back({0.02, 0.003, 4e-4, 2e-4, "GND", "", "", ""});
    s.vias.push_back({0.035, 0.003, 4e-4, 2e-4, "GND", "", "", ""});
    const auto r = board::checkAntenna(s, s.components[0], 4.0, 4.25, {"GND"}, 80, 37);
    THEN("one via is inside the keepout") {
      REQUIRE(r.viasInKeepout == 1);
    }
    AND_THEN("the ground layer's area, reach and width come from the geometry") {
      REQUIRE(r.ground.size() == 1);
      REQUIRE(r.ground[0].areaMm2 == Approx(40.0 * 24.0).epsilon(2e-3));
      REQUIRE(r.ground[0].reachMm == Approx(30.0).epsilon(2e-3));
      REQUIRE(r.ground[0].widthMm == Approx(40.0).epsilon(2e-3));
    }
    AND_THEN("a ground under half the vendor's board is called out") {
      REQUIRE(r.findings.size() >= 1); // 960 mm2 of 2960 mm2: about 32 %
      REQUIRE(r.findings.back().find("Ground:") == 0);
    }
    AND_THEN("the stackup under the part is 0.1 mm to the next copper layer, 0.3 mm in all") {
      REQUIRE(r.toNextPlaneMm == Approx(0.1).epsilon(2e-3));
      REQUIRE(r.totalThicknessMm == Approx(0.3).epsilon(2e-3));
    }
  }
}

SCENARIO("The antenna rule is found by part number in the shipped data") {
  GIVEN("data/antenna_rules.json") {
    std::ifstream in(std::string(FDTD_DATA_DIR) + "/antenna_rules.json");
    std::stringstream ss;
    ss << in.rdbuf();
    const auto rules = board::parseAntennaRules(ss.str());
    board::Component c;
    c.value = "w3008";
    THEN("a part whose value holds the number gets its keepout, another gets none") {
      const board::AntennaRule* r = board::ruleFor(rules, c);
      REQUIRE(r);
      REQUIRE(r->keepoutAlongMm == Approx(4.0));
      REQUIRE(r->keepoutDepthMm == Approx(4.25));
      c.value = "10k";
      REQUIRE(board::ruleFor(rules, c) == nullptr);
    }
  }
}

SCENARIO("A corner-mounted keepout starts at the nearest corner, not at the part's centre") {
  GIVEN(
      "the 40 x 30 mm board with the part 6 mm from the bottom-left corner, and copper at 2 mm and at 8 mm") {
    board::Snapshot s = fixture();
    s.components[0].pads = {{"1", "RF", 0.0058, 0.001}, {"2", "GND", 0.0062, 0.001}};
    s.copper.clear();
    s.copper.push_back(
        {"In1.Cu", "", "GND", rect(0.0015, 0.0005, 0.0025, 0.0015), {}}); // 1 mm2 at x 1.5 to 2.5
    s.copper.push_back(
        {"In2.Cu", "", "GND", rect(0.0075, 0.0005, 0.0085, 0.0015), {}}); // 1 mm2 at x 7.5 to 8.5
    WHEN("the 4.00 x 4.25 mm keepout is anchored at the corner") {
      const auto r =
          board::checkAntenna(s, s.components[0], 4.0, 4.25, {"GND"}, 0, 0, board::Placement::Corner);
      THEN("only the copper within 4 mm of the corner is reported") {
        REQUIRE(r.ok);
        REQUIRE(r.corner);
        REQUIRE(r.keepout.size() == 1);
        REQUIRE(r.keepout[0].layer == "In1.Cu");
        REQUIRE(r.keepout[0].areaMm2 == Approx(1.0).epsilon(2e-3));
      }
    }
    AND_WHEN("it is centred on the part instead") {
      const auto r = board::checkAntenna(s, s.components[0], 4.0, 4.25, {"GND"});
      THEN("the box spans x 4 to 8 mm, so the other copper is the one reported (the first sits outside it)") {
        REQUIRE(r.keepout.size() == 1);
        REQUIRE(r.keepout[0].layer == "In2.Cu");
      }
    }
  }
}

SCENARIO("A project's own antenna rules win over the shipped ones") {
  GIVEN("a shipped rule for one part and a project rule for the same part number and another part") {
    const auto shipped = board::parseAntennaRules(
        R"({"rules":[{"id":"shipped-a","parts":["AAA1"],"keepout_along_mm":4,"keepout_depth_mm":4},
                     {"id":"shipped-b","parts":["BBB2"],"keepout_along_mm":5,"keepout_depth_mm":5}]})");
    const auto project = board::parseAntennaRules(
        R"({"rules":[{"id":"mine","parts":["AAA1"],"keepout_along_mm":7,"keepout_depth_mm":6,"placement":"corner"},
                     {"id":"shipped-b","parts":["BBB2"],"keepout_along_mm":9,"keepout_depth_mm":9}]})",
        true);
    const auto merged = board::mergeRules(shipped, project);
    THEN("the project's rule is used for the shared part number, and a redefined id replaces the shipped "
         "one") {
      board::Component c;
      c.value = "aaa1";
      const board::AntennaRule* r = board::ruleFor(merged, c);
      REQUIRE(r);
      REQUIRE(r->id == "mine");
      REQUIRE(r->fromProject);
      REQUIRE(r->placement == "corner");
      REQUIRE(merged.size() == 3); // mine, the redefined shipped-b, and the untouched shipped-a
      c.value = "BBB2";
      REQUIRE(board::ruleFor(merged, c)->keepoutAlongMm == Approx(9.0));
    }
  }
  GIVEN("a rules file that is not valid JSON") {
    THEN("it is refused with a message naming the problem") {
      REQUIRE_THROWS_AS(board::parseAntennaRules("{ not json", true), board::SnapshotError);
    }
  }
}
