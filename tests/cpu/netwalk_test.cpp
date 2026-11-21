// BDD scenarios for net-driven selection (docs/PLAN.md, Next steps 12b).
#include <catch2/catch_test_macros.hpp>

#include "board/netwalk.hpp"
#include "board/voxelize.hpp"

namespace {

board::Component part(const std::string& ref, const std::string& value,
                      std::vector<std::pair<std::string, std::string>> padNets, bool dnp = false) {
  board::Component c;
  c.ref = ref;
  c.value = value;
  c.layer = "F.Cu";
  c.dnp = dnp;
  int n = 1;
  for (const auto& [net, unused] : padNets)
    c.pads.push_back({std::to_string(n++), net, 0.0, 0.0});
  return c;
}

// FEED -R1(0R)- MID -L1(2.2nH)- ANT, a shunt C9 from MID to GND, an IC U1 on FEED with a ground pad and a
// third net, a bead FB1 (part number only) from ANT to FAR, and R9 between two unrelated nets.
board::Snapshot matchingNetwork() {
  board::Snapshot s;
  s.outline = {{0, 0}, {0.1, 0}, {0.1, 0.1}, {0, 0.1}};
  s.components = {part("R1", "0R", {{"FEED", ""}, {"MID", ""}}),
                  part("L1", "2.2nH", {{"MID", ""}, {"ANT", ""}}),
                  part("C9", "1pF", {{"MID", ""}, {"GND", ""}}),
                  part("U1", "PA", {{"FEED", ""}, {"GND", ""}, {"CTRL", ""}}),
                  part("FB1", "BLM18", {{"ANT", ""}, {"FAR", ""}}),
                  part("R9", "10k", {{"X", ""}, {"Y", ""}})};
  // a ground pour covering most of the board, and signal copper
  s.copper.push_back({"B.Cu", "", "GND", {{0, 0}, {0.1, 0}, {0.1, 0.1}, {0, 0.1}}, {}});
  s.copper.push_back({"F.Cu", "", "FEED", {{0.01, 0.01}, {0.02, 0.01}, {0.02, 0.012}, {0.01, 0.012}}, {}});
  s.copper.push_back({"F.Cu", "", "X", {{0.05, 0.05}, {0.06, 0.05}, {0.06, 0.052}, {0.05, 0.052}}, {}});
  return s;
}

} // namespace

SCENARIO("Ground and plane nets are recognised by name and by the area they cover") {
  GIVEN("a board with a GND pour, a signal net and a large net with an unusual name") {
    auto s = matchingNetwork();
    s.copper.push_back({"In1.Cu", "", "PLANE5", {{0, 0}, {0.09, 0}, {0.09, 0.09}, {0, 0.09}}, {}});
    const auto ground = board::guessGroundNets(s);
    THEN("GND (by name and area) and PLANE5 (by area alone) are ground, FEED and X are not") {
      REQUIRE(ground.count("GND"));
      REQUIRE(ground.count("PLANE5"));
      REQUIRE(!ground.count("FEED"));
      REQUIRE(!ground.count("X"));
    }
  }
}

SCENARIO("Picking the feed net brings in the matching network and stops at ICs and unmodelled parts") {
  GIVEN("a feed line, a 0R link, a 2.2 nH inductor, a shunt capacitor, an IC, a bead and an unrelated "
        "resistor") {
    const auto s = matchingNetwork();
    const std::set<std::string> ground{"GND"};
    const auto w = board::walkNets(s, {"FEED"}, ground);
    THEN("the walk crosses R1 and L1 and reaches MID and ANT, but never GND") {
      REQUIRE(w.nets == std::set<std::string>{"FEED", "MID", "ANT"});
    }
    AND_THEN("every part with a pad on a reached net is included, with its reason") {
      REQUIRE(w.parts == std::set<std::string>{"R1", "L1", "C9", "U1", "FB1"});
      REQUIRE(w.reasons.at("R1").find("pad on net FEED") != std::string::npos);
      REQUIRE(w.reasons.at("R1").find("joins net MID") != std::string::npos);
      REQUIRE(w.reasons.at("C9").find("pad on net MID") != std::string::npos);
    }
    AND_THEN("the IC and the bead are included but not crossed, and the notes say why") {
      REQUIRE(!w.nets.count("CTRL"));
      REQUIRE(!w.nets.count("FAR"));
      bool ic = false, bead = false;
      for (const auto& n : w.notes) {
        ic = ic || (n.find("U1") != std::string::npos && n.find("3 pad") != std::string::npos);
        bead = bead || (n.find("FB1") != std::string::npos && n.find("not modelled") != std::string::npos);
      }
      REQUIRE(ic);
      REQUIRE(bead);
    }
    AND_THEN("the unrelated resistor is not included") {
      REQUIRE(!w.parts.count("R9"));
    }
  }
  GIVEN("the same board with the user's values for the bead, and R1 set open") {
    const auto s = matchingNetwork();
    std::map<std::string, board::PartTreatment> t;
    t["FB1"] = {board::PartTreatment::State::Values, false, 600.0, 100e-9, 0.2e-12};
    WHEN("the walk starts again from FEED") {
      const auto w = board::walkNets(s, {"FEED"}, {"GND"}, t);
      THEN("it still reaches ANT and now also FAR through the bead") {
        REQUIRE(w.nets.count("FAR"));
      }
    }
    t["R1"].state = board::PartTreatment::State::Open;
    WHEN("R1 is open") {
      const auto w = board::walkNets(s, {"FEED"}, {"GND"}, t);
      THEN("the walk stops at R1: MID and ANT are not reached") {
        REQUIRE(w.nets == std::set<std::string>{"FEED"});
      }
    }
  }
  GIVEN("a do-not-populate 0R link") {
    auto s = matchingNetwork();
    s.components[0].dnp = true;
    const auto w = board::walkNets(s, {"FEED"}, {"GND"});
    THEN("it is not crossed: a DNP part is open") {
      REQUIRE(!w.nets.count("MID"));
    }
  }
}

SCENARIO(
    "A net the user picked is never treated as ground, even a big patch the area guess would call a plane") {
  GIVEN("a patch antenna net covering a third of the board, listed among the guessed ground nets") {
    auto s = matchingNetwork();
    s.copper.push_back({"F.Cu", "", "PATCH", {{0.01, 0.01}, {0.07, 0.01}, {0.07, 0.07}, {0.01, 0.07}}, {}});
    const auto ground = board::guessGroundNets(s);
    THEN("the guess does call it a plane") {
      REQUIRE(ground.count("PATCH"));
    }
    AND_THEN("picking it still walks from it: it stays in the nets") {
      const auto w = board::walkNets(s, {"PATCH"}, ground);
      REQUIRE(w.nets.count("PATCH"));
    }
  }
}

SCENARIO("A net selection keeps only the copper on its nets and the ground reference") {
  GIVEN("a snapshot with copper on GND and FEED, and a selection of FEED alone with GND as ground") {
    board::Snapshot s = matchingNetwork();
    const auto w = board::walkNets(s, {"FEED"}, {"GND"});
    board::Selection sel;
    board::applyNetSelection(sel, w, {"GND"});
    THEN("copper on X is dropped, copper on FEED and GND is kept") {
      REQUIRE(sel.netIncluded("FEED"));
      REQUIRE(sel.netIncluded("MID"));
      REQUIRE(sel.netIncluded("GND"));
      REQUIRE(!sel.netIncluded("X"));
      REQUIRE(!sel.netIncluded(""));
    }
    AND_THEN("the parts are the walk's, and the notes carry each reason") {
      REQUIRE(sel.includes("R1"));
      REQUIRE(!sel.includes("R9"));
      bool reason = false;
      for (const auto& n : sel.netNotes)
        reason = reason || n.find("R1 included") != std::string::npos;
      REQUIRE(reason);
    }
  }
}
