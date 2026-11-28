// BDD scenarios for the refinement suggestions (docs/PLAN.md, "Plugin updated with the findings").
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <string>

#include "board/refine_suggest.hpp"

using Catch::Approx;

namespace {
board::Ring rect(double x0, double y0, double x1, double y1) {
  return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
}

// 30 x 20 mm board, 1.6 mm of er 4.4. F.Cu: a feed track (net FEED, 0.8 mm wide, running along x at y = 10
// mm) with ground pours (net GND) beside it at `gapMm`; a vertical port on the track at x = 10 mm.
board::Snapshot feedBoard(double gapMm) {
  board::Snapshot s;
  s.outline = rect(0, 0, 0.03, 0.02);
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
  const double w = 0.8e-3, g = gapMm * 1e-3;
  s.copper.push_back({"F.Cu", "", "FEED", rect(0.005, 0.01 - w / 2, 0.025, 0.01 + w / 2), {}});
  s.copper.push_back({"F.Cu", "", "GND", rect(0.001, 0.001, 0.029, 0.01 - w / 2 - g), {}});
  s.copper.push_back({"F.Cu", "", "GND", rect(0.001, 0.01 + w / 2 + g, 0.029, 0.019), {}});
  s.copper.push_back({"B.Cu", "", "GND", rect(0.0, 0.0, 0.03, 0.02), {}});
  board::PortDef p;
  p.name = "P1";
  p.layer = "F.Cu";
  p.layerB = "B.Cu";
  p.a = p.b = {0.01, 0.01};
  p.width = 0.4e-3;
  s.ports.push_back(p);
  return s;
}
} // namespace

SCENARIO("A narrow gap between two nets near a port is turned into a suggested refinement box") {
  GIVEN("a feed with ground 0.3 mm beside it and a port on it") {
    const auto s = feedBoard(0.3);
    const auto list = board::suggestRefinement(s, board::Selection{});
    THEN("one suggestion names the nets, the layer and the gap, and puts the box around the port") {
      REQUIRE(list.size() == 1);
      const auto& g = list[0];
      REQUIRE(g.layer == "F.Cu");
      REQUIRE(((g.netA == "FEED" && g.netB == "GND") || (g.netA == "GND" && g.netB == "FEED")));
      REQUIRE(g.gapMm == Approx(0.3).margin(0.02));
      REQUIRE(g.box.x0 < 0.01);
      REQUIRE(g.box.x1 > 0.01);
      REQUIRE(g.box.y0 < 0.01 - 0.0004);
      REQUIRE(g.box.y1 > 0.01 + 0.0004);
      REQUIRE_FALSE(g.id.empty());
      REQUIRE(g.reason.find("0.3") != std::string::npos);
    }
    AND_THEN("the suggested cell is the gap over six cells") {
      REQUIRE_FALSE(list.empty());
      REQUIRE(list[0].cellMm == Approx(0.05).margin(0.004));
      REQUIRE(list[0].box.cell == Approx(list[0].cellMm * 1e-3).epsilon(1e-6));
    }
    AND_THEN("asking for ten cells across gives a finer cell, and the floor holds") {
      REQUIRE_FALSE(board::suggestRefinement(s, board::Selection{}, 10.0).empty());
      REQUIRE(board::suggestRefinement(s, board::Selection{}, 10.0)[0].cellMm == Approx(0.03).margin(0.003));
      REQUIRE(board::suggestRefinement(s, board::Selection{}, 100.0)[0].cellMm == Approx(0.02).margin(1e-9));
    }
  }
  GIVEN("the same feed with ground 3 mm away") {
    THEN("there is nothing to suggest") {
      REQUIRE(board::suggestRefinement(feedBoard(3.0), board::Selection{}).empty());
    }
  }
  GIVEN("a feed with ground beside it, but the ground net is left out of the model") {
    board::Selection sel;
    sel.netFilter = true;
    sel.nets = {"FEED"};
    THEN("no gap to a net that is not in the model is suggested") {
      REQUIRE(board::suggestRefinement(feedBoard(0.3), sel).empty());
    }
  }
  GIVEN("a board without ports") {
    auto s = feedBoard(0.3);
    s.ports.clear();
    THEN("there is nothing to suggest: only the neighbourhood of a feed matters") {
      REQUIRE(board::suggestRefinement(s, board::Selection{}).empty());
    }
  }
}

namespace {
// 60 x 60 mm board, 1.6 mm of er 4.4. F.Cu: a 20 x 14 mm patch (net RF, x 20 to 40 mm, y 23 to 37 mm) with a
// probe port inside it; B.Cu: a full ground plane (net GND). `feed` adds a 0.5 mm track from the patch's
// right edge out to the board edge (copper that leaves the neighbourhood of the port).
board::Snapshot patchBoard(bool feed) {
  board::Snapshot s;
  s.outline = rect(0, 0, 0.06, 0.06);
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
  s.copper.push_back({"F.Cu", "", "RF", rect(0.02, 0.023, 0.04, 0.037), {}});
  if (feed)
    s.copper.push_back({"F.Cu", "", "RF", rect(0.039, 0.02975, 0.06, 0.03025), {}});
  s.copper.push_back({"B.Cu", "", "GND", rect(0.0, 0.0, 0.06, 0.06), {}});
  board::PortDef p;
  p.name = "probe";
  p.layer = "F.Cu";
  p.layerB = "B.Cu";
  p.a = p.b = {0.03, 0.026};
  p.width = 0.0;
  s.ports.push_back(p);
  return s;
}
} // namespace

SCENARIO("The open ends of the copper a port feeds are turned into suggested refinement boxes") {
  GIVEN("a patch with a probe port and a ground plane under it") {
    const auto s = patchBoard(false);
    const auto list = board::suggestCopperEnds(s, board::Selection{}, 0.12, 3.0);
    THEN("each of the four edges gets a box of the asked cell, centered on the edge and spanning it") {
      REQUIRE(list.size() == 4);
      int left = 0, right = 0, bottom = 0, top = 0;
      for (const auto& g : list) {
        REQUIRE(g.layer == "F.Cu");
        REQUIRE(g.cellMm == Approx(0.12).margin(1e-9));
        REQUIRE(g.box.cell == Approx(0.12e-3).epsilon(1e-6));
        REQUIRE_FALSE(g.id.empty());
        const bool xEnd = g.box.x1 - g.box.x0 < g.box.y1 - g.box.y0 - 1e-6; // narrow in x: an x end
        if (xEnd) {
          REQUIRE(g.box.x1 - g.box.x0 == Approx(6e-3).margin(0.4e-3));
          REQUIRE(g.box.y0 <= 0.023);
          REQUIRE(g.box.y1 >= 0.037);
          const double c = 0.5 * (g.box.x0 + g.box.x1);
          (c < 0.03 ? left : right) += (std::abs(c - (c < 0.03 ? 0.02 : 0.04)) < 0.3e-3);
        } else {
          REQUIRE(g.box.y1 - g.box.y0 == Approx(6e-3).margin(0.4e-3));
          REQUIRE(g.box.x0 <= 0.02);
          REQUIRE(g.box.x1 >= 0.04);
          const double c = 0.5 * (g.box.y0 + g.box.y1);
          (c < 0.03 ? bottom : top) += (std::abs(c - (c < 0.03 ? 0.023 : 0.037)) < 0.3e-3);
        }
      }
      REQUIRE(left == 1);
      REQUIRE(right == 1);
      REQUIRE(bottom == 1);
      REQUIRE(top == 1);
    }
  }
  GIVEN("the same patch with a feed track that leaves the neighbourhood of the port") {
    const auto list = board::suggestCopperEnds(patchBoard(true), board::Selection{}, 0.12, 3.0, 12.0);
    THEN("no box is put at the far side, where the copper only continues past the reach") {
      for (const auto& g : list)
        REQUIRE(g.box.x1 < 0.04 + 3.5e-3);
      REQUIRE(list.size() == 3);
    }
  }
  GIVEN("a board whose only copper is a ground plane, and a board without ports") {
    auto ground = patchBoard(false);
    ground.copper.erase(ground.copper.begin());
    auto noPort = patchBoard(false);
    noPort.ports.clear();
    THEN("there is nothing to suggest: a plane has no ends, and only the copper a port feeds matters") {
      REQUIRE(board::suggestCopperEnds(ground, board::Selection{}).empty());
      REQUIRE(board::suggestCopperEnds(noPort, board::Selection{}).empty());
    }
  }
}
