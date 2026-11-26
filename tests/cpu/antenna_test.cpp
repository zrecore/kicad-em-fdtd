// BDD scenarios for the static antenna checks (docs/PLAN.md, "Reference-guided antenna tuning", Tier 1).
#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <sstream>

#include "board/antenna.hpp"
#include "board/feed_solve.hpp"

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

namespace {
board::Component part(const std::string& ref, const std::string& value) {
  board::Component c;
  c.ref = ref;
  c.value = value;
  return c;
}
} // namespace

SCENARIO("The antenna part is found and listed first even when no rule knows it") {
  // the parts in the order KiCAD's export gave them on the nRF52840 IFA board: the antenna is the ninth
  const std::vector<board::Component> parts = {
      part("X2", "32.768kHz"),     part("C11", "100n"), part("C12", "100n"),
      part("U1", "nRF52840-QIAA"), part("C2", "12pF"),  part("C23", "NF"),
      part("J1", "U.FL-R-SMT-1"),  part("C18", "1u"),   part("ANT1", "IFA"),
      part("C10", "100n"),         part("R2", "0R")};
  GIVEN("a part named ANT1 whose value (IFA) no stored rule matches") {
    THEN("it looks like an antenna, and a connector, a resistor and a capacitor do not") {
      REQUIRE(board::looksLikeAntenna(parts[8]));
      REQUIRE(board::looksLikeAntenna(part("AE1", "W3008")));
      REQUIRE(board::looksLikeAntenna(part("U7", "2.4 GHz Chip Antenna")));
      REQUIRE(board::looksLikeAntenna(part("E1", "PIFA")));
      for (const char* ref : {"J1", "R2", "C2", "U1", "X2"})
        REQUIRE_FALSE(board::looksLikeAntenna(part(ref, "")));
      REQUIRE_FALSE(board::looksLikeAntenna(parts[6])); // a U.FL connector is not the antenna
    }
    AND_THEN("the list puts it first and the rest in natural order (C2 before C10)") {
      const auto order = board::antennaPickOrder(parts);
      REQUIRE(order.size() == parts.size());
      REQUIRE(order.front()->ref == "ANT1");
      std::vector<std::string> rest;
      for (std::size_t i = 1; i < order.size(); ++i)
        rest.push_back(order[i]->ref);
      REQUIRE(rest ==
              std::vector<std::string>{"C2", "C10", "C11", "C12", "C18", "C23", "J1", "R2", "U1", "X2"});
    }
    AND_THEN("with no rule it is the default pick, because it is the only one that looks like an antenna") {
      const board::Component* d = board::defaultAntennaPart(parts, {});
      REQUIRE(d);
      REQUIRE(d->ref == "ANT1");
    }
  }
  GIVEN("two parts that look like antennas and no rule") {
    auto two = parts;
    two.push_back(part("ANT2", "chip antenna"));
    THEN("nothing is picked for the user, but both are listed first") {
      REQUIRE(board::defaultAntennaPart(two, {}) == nullptr);
      const auto order = board::antennaPickOrder(two);
      REQUIRE(order[0]->ref == "ANT1");
      REQUIRE(order[1]->ref == "ANT2");
    }
  }
  GIVEN("a part a stored rule recognises beside an ANT1") {
    std::ifstream in(std::string(FDTD_DATA_DIR) + "/antenna_rules.json");
    std::stringstream ss;
    ss << in.rdbuf();
    const auto rules = board::parseAntennaRules(ss.str());
    auto withRule = parts;
    withRule.push_back(part("U9", "W3008"));
    THEN("the rule's part wins, as before") {
      const board::Component* d = board::defaultAntennaPart(withRule, rules);
      REQUIRE(d);
      REQUIRE(d->ref == "U9");
    }
  }
  GIVEN("a board with no part that looks like an antenna") {
    THEN("nothing is picked") {
      REQUIRE(board::defaultAntennaPart({part("R1", "10k"), part("C1", "1u")}, {}) == nullptr);
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

namespace {
// A 40 x 40 mm two-layer board, 1.6 mm of er 4.4, ground plane on B.Cu, a part on F.Cu whose RF pad feeds a
// track of the given width running up the board; optional ground copper beside the track on F.Cu.
board::Snapshot feedBoard(double trackWidthMm, double sideGapMm, bool groundBelow = true) {
  board::Snapshot s;
  s.outline = rect(0, 0, 0.04, 0.04);
  board::Layer top;
  top.name = "F.Cu";
  top.copper = true;
  board::Layer core;
  core.name = "core";
  core.thickness = 1.6e-3;
  core.epsilonR = 4.4;
  board::Layer bottom;
  bottom.name = "B.Cu";
  bottom.copper = true;
  s.stackup = {top, core, bottom};
  board::Component ant;
  ant.ref = "ANT1";
  ant.value = "chip";
  ant.layer = "F.Cu";
  ant.x = 0.02;
  ant.y = 0.004;
  ant.pads = {{"1", "RF", 0.02, 0.004}, {"2", "GND", 0.0215, 0.004}};
  s.components.push_back(ant);
  const double w = trackWidthMm * 1e-3;
  s.copper.push_back({"F.Cu", "ANT1", "RF", rect(0.0197, 0.0035, 0.0203, 0.0045), {}});
  s.copper.push_back({"F.Cu", "ANT1", "GND", rect(0.0212, 0.0035, 0.0218, 0.0045), {}});
  s.copper.push_back({"F.Cu", "", "RF", rect(0.02 - w / 2, 0.004, 0.02 + w / 2, 0.034), {}});
  if (groundBelow)
    s.copper.push_back({"B.Cu", "", "GND", rect(0, 0, 0.04, 0.04), {}});
  if (sideGapMm > 0) {
    const double g = sideGapMm * 1e-3;
    s.copper.push_back({"F.Cu", "", "GND", rect(0.0, 0.007, 0.02 - w / 2 - g, 0.038), {}});
    s.copper.push_back({"F.Cu", "", "GND", rect(0.02 + w / 2 + g, 0.007, 0.04, 0.038), {}});
  }
  return s;
}
} // namespace

namespace {
// feedBoard's track replaced by pieces (x0, y0, x1, y1 in metres, all on net RF), to give the feed a step, a
// corner or another part's pad.
board::Snapshot feedBoardWith(const std::vector<std::array<double, 4>>& pieces,
                              const std::vector<std::pair<std::string, std::array<double, 4>>>& pads = {}) {
  board::Snapshot s = feedBoard(0.8, 0);
  s.copper.erase(std::remove_if(s.copper.begin(), s.copper.end(),
                                [](const board::CopperShape& c) { return c.owner.empty() && c.net == "RF"; }),
                 s.copper.end());
  for (const auto& p : pieces)
    s.copper.push_back({"F.Cu", "", "RF", rect(p[0], p[1], p[2], p[3]), {}});
  for (const auto& [ref, p] : pads)
    s.copper.push_back({"F.Cu", ref, "RF", rect(p[0], p[1], p[2], p[3]), {}});
  return s;
}
bool says(const board::FeedReport& r, const std::string& text) {
  for (const auto& f : r.findings)
    if (f.find(text) != std::string::npos)
      return true;
  return false;
}
} // namespace

SCENARIO("A line with ground copper along one side only is not read as plain microstrip") {
  const std::set<std::string> gnd{"GND"};
  GIVEN("a 0.8 mm track with ground copper 0.3 mm beside it on one side and a plane below") {
    auto s = feedBoard(0.8, 0.3);
    s.copper.pop_back(); // the right-hand pour: only the left one stays
    const auto r = board::checkFeed(s, s.components[0], gnd);
    THEN("the report says one-sided, with the gap, and the sentence names it") {
      REQUIRE(r.ok);
      REQUIRE(r.oneSided);
      REQUIRE_FALSE(r.coplanar);
      REQUIRE(r.gapMm == Approx(0.3).margin(0.05));
      REQUIRE(says(r, "one side only"));
    }
    AND_THEN(
        "the field solve includes that ground, so it reads lower than microstrip and higher than two-sided") {
      const auto one = board::solveFeed(r, {}, 0.1);
      auto both = r;
      both.oneSided = false;
      both.coplanar = true;
      const auto two = board::solveFeed(both, {}, 0.1);
      auto plain = r;
      plain.oneSided = false;
      plain.gapMm = 0;
      const auto micro = board::solveFeed(plain, {}, 0.1);
      INFO("microstrip " << micro.z0Ohm << ", one side " << one.z0Ohm << ", both sides " << two.z0Ohm);
      REQUIRE(one.ok);
      REQUIRE(one.z0Ohm < micro.z0Ohm);
      REQUIRE(one.z0Ohm > two.z0Ohm);
    }
  }
}

SCENARIO("The feed report says how uniform the line is, not just one number") {
  const std::set<std::string> gnd{"GND"};
  GIVEN("a uniform 0.8 mm track") {
    const auto s = feedBoardWith({{0.0196, 0.004, 0.0204, 0.034}});
    const auto r = board::checkFeed(s, s.components[0], gnd);
    THEN("there is one segment of 0.8 mm, no other parts and no turns, and the report says so") {
      REQUIRE(r.ok);
      REQUIRE(r.segments.size() == 1);
      REQUIRE(r.segments[0].widthMm == Approx(0.8).margin(0.05));
      REQUIRE(r.otherParts.empty());
      REQUIRE(r.turnsAtMm.empty());
      REQUIRE(says(r, "uniform"));
    }
  }
  GIVEN("a track that is 0.8 mm wide for 4.5 mm from the pad and 0.4 mm wide after that") {
    const auto s = feedBoardWith({{0.0196, 0.004, 0.0204, 0.0085}, {0.0198, 0.0085, 0.0202, 0.034}});
    const auto r = board::checkFeed(s, s.components[0], gnd);
    THEN("two segments are found, the step is named with where it is, and it is not called uniform") {
      REQUIRE(r.ok);
      REQUIRE(r.segments.size() == 2);
      REQUIRE(r.segments[0].widthMm == Approx(0.8).margin(0.05));
      REQUIRE(r.segments[1].widthMm == Approx(0.4).margin(0.05));
      REQUIRE(r.segments[1].fromMm == Approx(4.5).margin(0.6));
      REQUIRE(r.segments[0].z0Ohm > 0);
      REQUIRE(r.segments[0].z0Ohm < r.segments[1].z0Ohm); // the narrower stretch has the higher impedance
      REQUIRE(says(r, "width changes"));
      REQUIRE_FALSE(says(r, "uniform"));
    }
  }
  GIVEN("another part's pad on the feed net 2 mm from the part, wider than the track") {
    const auto s =
        feedBoardWith({{0.0196, 0.004, 0.0204, 0.034}}, {{"L1", {0.0192, 0.0055, 0.0208, 0.0065}}});
    const auto r = board::checkFeed(s, s.components[0], gnd);
    THEN("it is named, and its copper does not count as line width") {
      REQUIRE(r.ok);
      REQUIRE(r.otherParts == std::vector<std::string>{"L1"});
      REQUIRE(r.widthMm == Approx(0.8).margin(0.05));
      REQUIRE(says(r, "L1"));
    }
  }
  GIVEN("a track that runs 3 mm from the pad and then turns right") {
    const auto s = feedBoardWith({{0.0196, 0.004, 0.0204, 0.0074}, {0.0196, 0.0066, 0.0300, 0.0074}});
    const auto r = board::checkFeed(s, s.components[0], gnd);
    THEN("the turn is reported near 3 mm from the pad") {
      REQUIRE(r.ok);
      REQUIRE_FALSE(r.turnsAtMm.empty());
      REQUIRE(r.turnsAtMm.front() == Approx(3.0).margin(1.0));
      REQUIRE(says(r, "turns or branches"));
    }
  }
}

SCENARIO("The feed line of an antenna part is read from the board and its impedance estimated") {
  const std::set<std::string> gnd{"GND"};
  GIVEN("a 3.06 mm track over a ground plane 1.6 mm below, no ground beside it") {
    const auto s = feedBoard(3.06, 0);
    const auto r = board::checkFeed(s, s.components[0], gnd);
    THEN("it is a microstrip of about 50 ohm and the report says where it measured") {
      REQUIRE(r.ok);
      REQUIRE_FALSE(r.coplanar);
      REQUIRE(r.layer == "F.Cu");
      REQUIRE(r.referenceLayer == "B.Cu");
      REQUIRE(r.net == "RF");
      REQUIRE(r.widthMm == Approx(3.06).margin(0.05));
      REQUIRE(r.heightMm == Approx(1.6).margin(1e-6));
      REQUIRE(r.z0Ohm == Approx(50.0).epsilon(0.05));
    }
    AND_THEN(
        "a line within 10 percent of the port impedance is called fine, with the method's limits stated") {
      bool within = false, limits = false;
      for (const auto& f : r.findings) {
        within = within || f.find("within 10 %") != std::string::npos;
        limits = limits || f.find("closed form") != std::string::npos;
      }
      REQUIRE(within);
      REQUIRE(limits);
    }
  }
  GIVEN("a 0.8 mm track with ground copper 0.3 mm beside it on the same layer and a plane below") {
    const auto s = feedBoard(0.8, 0.3);
    const auto r = board::checkFeed(s, s.components[0], gnd);
    THEN("it is a grounded coplanar line with the gap measured, near 65 ohm") {
      REQUIRE(r.ok);
      REQUIRE(r.coplanar);
      REQUIRE(r.widthMm == Approx(0.8).margin(0.05));
      REQUIRE(r.gapMm == Approx(0.3).margin(0.05));
      REQUIRE(r.z0Ohm == Approx(64.7).epsilon(0.05));
    }
    AND_THEN("it is flagged as more than 10 percent from a 50 ohm port") {
      bool off = false;
      for (const auto& f : r.findings)
        off = off || f.find("more than 10 %") != std::string::npos;
      REQUIRE(off);
    }
  }
  GIVEN("the same coplanar feed on a board whose top copper is 35 um thick") {
    auto s = feedBoard(0.8, 0.3);
    s.stackup[0].thickness = 35e-6;
    const auto r = board::checkFeed(s, s.components[0], gnd);
    THEN("the report carries the line's copper thickness, 0 when the board does not say") {
      REQUIRE(r.ok);
      REQUIRE(r.copperMm == Approx(0.035));
      const auto none = feedBoard(0.8, 0.3);
      REQUIRE(board::checkFeed(none, none.components[0], gnd).copperMm == 0);
    }
  }
  GIVEN("a part whose signal pad has no copper leaving it") {
    auto s = feedBoard(3.06, 0);
    s.copper.erase(s.copper.begin() + 2); // the track
    const auto r = board::checkFeed(s, s.components[0], gnd);
    THEN("the report says there is no feed line to read") {
      REQUIRE_FALSE(r.ok);
      REQUIRE_FALSE(r.why.empty());
    }
  }
  GIVEN("a track with no ground plane under it") {
    const auto s = feedBoard(3.06, 0, false);
    const auto r = board::checkFeed(s, s.components[0], gnd);
    THEN("the report says no reference plane was found") {
      REQUIRE_FALSE(r.ok);
      REQUIRE(r.why.find("ground") != std::string::npos);
    }
  }
}
