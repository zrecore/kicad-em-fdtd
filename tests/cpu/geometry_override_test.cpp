// BDD scenarios for solver-side geometry overrides (docs/PLAN.md, Next steps 11g).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "board/geometry_override.hpp"

using Catch::Approx;

namespace {
board::Snapshot patchBoard() {
  board::Snapshot s;
  s.outline = {{0, 0}, {0.05, 0}, {0.05, 0.05}, {0, 0.05}};
  // the patch: 10 mm along x (0.010 to 0.020) by 8 mm along y (0.010 to 0.018), a via at its middle
  s.copper.push_back(
      {"F.Cu", "", "RF", {{0.010, 0.010}, {0.020, 0.010}, {0.020, 0.018}, {0.010, 0.018}}, {}});
  s.copper.push_back({"B.Cu", "", "GND", {{0, 0}, {0.05, 0}, {0.05, 0.05}, {0, 0.05}}, {}});
  s.vias.push_back({0.015, 0.014, 0.0004, 0.0002, "RF", ""});
  return s;
}
} // namespace

SCENARIO("An override stretches one net along one axis and leaves everything else alone") {
  GIVEN("a 10 mm x 8 mm patch on net RF with a via at its middle, and a ground plane") {
    WHEN("RF is stretched along x by +1 mm") {
      auto s = patchBoard();
      const auto notes = board::applyGeometryOverrides(s, {board::parseGeometryOverride("RF.x=1")});
      THEN("the patch is 11 mm long, starts where it did, and keeps its width, within 1e-12 m") {
        const auto& r = s.copper[0].outer;
        REQUIRE(r[0].x == Approx(0.010).margin(1e-12));
        REQUIRE(r[1].x == Approx(0.021).margin(1e-12));
        REQUIRE(r[0].y == Approx(0.010).margin(1e-12));
        REQUIRE(r[2].y == Approx(0.018).margin(1e-12));
      }
      AND_THEN("the via moves with the scaling (its middle position 0.015 becomes 0.0155)") {
        REQUIRE(s.vias[0].x == Approx(0.0155).margin(1e-12));
        REQUIRE(s.vias[0].y == Approx(0.014).margin(1e-12));
      }
      AND_THEN("the ground plane is untouched and the override is described for the run summary") {
        REQUIRE(s.copper[1].outer[2].x == 0.05);
        REQUIRE(notes.size() == 1);
        REQUIRE(notes[0].find("RF") != std::string::npos);
        REQUIRE(notes[0].find("+1 mm") != std::string::npos);
      }
    }
    WHEN("RF is shortened along y by 2 mm") {
      auto s = patchBoard();
      board::applyGeometryOverrides(s, {board::parseGeometryOverride("RF.y=-2")});
      THEN("the patch is 6 mm wide and keeps its length") {
        const auto& r = s.copper[0].outer;
        REQUIRE(r[2].y - r[0].y == Approx(0.006).margin(1e-12));
        REQUIRE(r[1].x - r[0].x == Approx(0.010).margin(1e-12));
      }
    }
    WHEN("two overrides are given") {
      auto s = patchBoard();
      board::applyGeometryOverrides(
          s, {board::parseGeometryOverride("RF.x=1"), board::parseGeometryOverride("RF.x=-1")});
      THEN("they compose in order and return the patch to its size, within 1e-12 m") {
        REQUIRE(s.copper[0].outer[1].x == Approx(0.020).margin(1e-12));
      }
    }
    WHEN("an override names a net with no copper or removes the whole extent") {
      auto s = patchBoard();
      THEN("it is refused with a message") {
        REQUIRE_THROWS_AS(board::applyGeometryOverrides(s, {board::parseGeometryOverride("NOPE.x=1")}),
                          board::SnapshotError);
        REQUIRE_THROWS_AS(board::applyGeometryOverrides(s, {board::parseGeometryOverride("RF.x=-10")}),
                          board::SnapshotError);
      }
    }
  }
}

SCENARIO("A geometry override is read from NET.axis=millimetres") {
  GIVEN("the accepted and refused forms") {
    THEN("a plain net and one with dots in its name both parse") {
      const auto a = board::parseGeometryOverride("RF.y=-0.25");
      REQUIRE(a.net == "RF");
      REQUIRE(a.axis == 'y');
      REQUIRE(a.deltaMm == Approx(-0.25));
      const auto b = board::parseGeometryOverride("Net-(AE1.1).x=2");
      REQUIRE(b.net == "Net-(AE1.1)");
      REQUIRE(b.axis == 'x');
    }
    THEN("a missing axis, a bad axis and a bad number are refused") {
      REQUIRE_THROWS_AS(board::parseGeometryOverride("RF=1"), board::SnapshotError);
      REQUIRE_THROWS_AS(board::parseGeometryOverride("RF.z=1"), board::SnapshotError);
      REQUIRE_THROWS_AS(board::parseGeometryOverride("RF.x=abc"), board::SnapshotError);
      REQUIRE_THROWS_AS(board::parseGeometryOverride("RF.x="), board::SnapshotError);
    }
  }
}
