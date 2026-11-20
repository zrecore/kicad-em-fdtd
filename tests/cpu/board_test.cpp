// BDD scenarios for the board snapshot reader and the voxelizer. Lengths in the fixture are metres.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <string>

#include "board/snapshot.hpp"
#include "board/voxelize.hpp"

using Catch::Approx;
using Catch::Matchers::ContainsSubstring;

namespace {

// A 10 x 10 mm two-layer board: 0.8 mm dielectric (er 4.4, tan d 0.02), a ground plane inset 0.2 mm from the
// edge on the bottom, and a 2.8 mm square pad (owned by footprint AE1) on top.
std::string fixture(int version = 1, const std::string& ports = "") {
  return R"({"format":"kicad-em-fdtd.snapshot","version":)" + std::to_string(version) + R"(,
    "board":{"name":"fixture","kicad_version":"10.0.7"},
    "outline":[[0,0],[0.01,0],[0.01,0.01],[0,0.01]],
    "stackup":[{"name":"F.Cu","kind":"copper"},
               {"name":"core","kind":"dielectric","thickness":0.0008,"epsilon_r":4.4,"loss_tangent":0.02},
               {"name":"B.Cu","kind":"copper"}],
    "copper":[{"layer":"B.Cu","owner":"","net":"GND","polygon":[[0.0002,0.0002],[0.0098,0.0002],[0.0098,0.0098],[0.0002,0.0098]]},
              {"layer":"F.Cu","owner":"AE1","net":"FEED","polygon":[[0.0036,0.0036],[0.0064,0.0036],[0.0064,0.0064],[0.0036,0.0064]]}],
    "vias":[],"ports":[)" +
         ports + R"(],
    "components":[{"ref":"AE1","value":"antenna","x":0.005,"y":0.005,"layer":"F.Cu"}]})";
}

const board::GridPlan kPlan{
    .lateralCell = 1e-3, .maxDielectricCell = 0.4e-3, .airMargin = 2e-3, .airCell = 1e-3};
constexpr double kF0 = 2.44e9;

std::size_t edgesInPlane(const board::Model& m, std::size_t zNode) {
  std::size_t n = 0;
  const auto& g = m.grid;
  for (const auto& e : m.pecEdges)
    n += (e.node / (g.nodes(fdtd::X) * g.nodes(fdtd::Y))) == zNode;
  return n;
}

} // namespace

SCENARIO("A board snapshot is read and validated") {
  GIVEN("a well-formed version 1 snapshot") {
    const auto s = board::parseSnapshot(fixture());
    THEN("the board, stackup, copper and components are all present") {
      REQUIRE(s.boardName == "fixture");
      REQUIRE(s.stackup.size() == 3);
      REQUIRE(s.stackup[1].thickness == Approx(0.0008));
      REQUIRE(s.copper.size() == 2);
      REQUIRE(s.copper[1].owner == "AE1");
      REQUIRE(s.components.size() == 1);
    }
  }
  GIVEN("a snapshot of an unsupported version") {
    THEN("reading fails and the message names the version") {
      REQUIRE_THROWS_WITH(board::parseSnapshot(fixture(2)), ContainsSubstring("version"));
    }
  }
  GIVEN("a snapshot without an outline") {
    std::string text = fixture();
    text.replace(text.find("\"outline\""), 9, "\"outlyne\"");
    THEN("reading fails and the message names the missing field") {
      REQUIRE_THROWS_WITH(board::parseSnapshot(text), ContainsSubstring("outline"));
    }
  }
}

SCENARIO("A snapshot is voxelized into a grid, materials and PEC sheets") {
  GIVEN("the fixture board with every component selected") {
    const auto snap = board::parseSnapshot(fixture());
    const auto model = board::voxelize(snap, board::Selection{}, kPlan, kF0);
    const auto& g = model.grid;
    auto epsAt = [&](std::size_t i, std::size_t j, std::size_t k) {
      return model.materials[model.cellMaterial[g.cellIndex(i, j, k)]].epsR;
    };
    THEN("the grid covers the outline plus the air margin: 14 x 14 cells laterally, 6 in z") {
      REQUIRE(g.cells(fdtd::X) == 14);
      REQUIRE(g.cells(fdtd::Y) == 14);
      REQUIRE(g.cells(fdtd::Z) == 6); // 2 air + 2 dielectric (0.8 mm / 0.4 mm) + 2 air
    }
    THEN("cells inside the outline and the dielectric have its permittivity; air and off-board cells are 1") {
      REQUIRE(epsAt(7, 7, 2) == Approx(4.4));
      REQUIRE(epsAt(7, 7, 3) == Approx(4.4));
      REQUIRE(epsAt(7, 7, 1) == Approx(1.0)); // below the board
      REQUIRE(epsAt(7, 7, 4) == Approx(1.0)); // above the board
      REQUIRE(epsAt(0, 7, 2) == Approx(1.0)); // beside the board, at dielectric height
    }
    THEN("the dielectric loss is a conductivity set at the centre frequency (2 pi f eps0 er tan d)") {
      const auto id = model.cellMaterial[g.cellIndex(7, 7, 2)];
      REQUIRE(model.materials[id].sigma == Approx(0.011946).epsilon(1e-3));
    }
    THEN("copper becomes PEC sheets on the layer interfaces: 180 edges in the ground plane, 12 in the pad") {
      REQUIRE(edgesInPlane(model, 2) == 180); // bottom of the dielectric
      REQUIRE(edgesInPlane(model, 4) == 12);  // top of the dielectric
      REQUIRE(model.pecEdges.size() == 192);
    }
  }
  GIVEN("the same board with the antenna footprint left out of the model") {
    const auto snap = board::parseSnapshot(fixture());
    board::Selection sel;
    sel.all = false; // nothing selected: board-level copper only
    const auto model = board::voxelize(snap, sel, kPlan, kF0);
    THEN("the pad is gone and the ground plane stays") {
      REQUIRE(edgesInPlane(model, 4) == 0);
      REQUIRE(edgesInPlane(model, 2) == 180);
    }
    THEN("the run notes say what was left out") {
      REQUIRE(!model.notes.empty());
    }
  }
}

SCENARIO("A port is expanded into series cells and parallel strips of lumped edges") {
  GIVEN("a 50 ohm port from x = 2.0 mm to the pad edge at 3.6 mm, 2 mm wide, on the top layer") {
    const auto snap = board::parseSnapshot(fixture(
        1,
        R"({"name":"P1","layer":"F.Cu","a":[0.002,0.005],"b":[0.0036,0.005],"width":0.002,"impedance":50})"));
    const auto model = board::voxelize(snap, board::Selection{}, kPlan, kF0);
    REQUIRE(model.ports.size() == 1);
    const auto& port = model.ports[0];
    THEN("it has 2 cells in series and 3 strips (nodes y = 4, 5, 6 mm), all on the top copper plane") {
      REQUIRE(port.series == 2);
      REQUIRE(port.parallel == 3);
      REQUIRE(port.edges.size() == 6);
      for (const auto& e : port.edges)
        REQUIRE(e.edge.k == 4);
    }
    THEN("each strip presents R * parallel = 150 ohm, so the strips together present 50 ohm") {
      double conductance = 0;
      for (std::size_t strip = 0; strip < 3; ++strip) {
        double r = 0;
        for (const auto& e : port.edges)
          if (e.strip == strip)
            r += e.edge.resistance;
        REQUIRE(r == Approx(150.0));
        conductance += 1.0 / r;
      }
      REQUIRE(1.0 / conductance == Approx(50.0));
    }
    THEN("the positive terminal is at the lower coordinate, so the sign is +1") {
      REQUIRE(port.edges[0].sign == 1.0);
    }
  }
}

SCENARIO("A vertical port spans the dielectric between two copper layers") {
  GIVEN("a 50 ohm port from the top pad layer down to the ground plane at (5, 5) mm, 2.2 mm wide") {
    const auto snap = board::parseSnapshot(fixture(
        1,
        R"({"name":"P1","layer":"F.Cu","layer_b":"B.Cu","a":[0.005,0.005],"width":0.0022,"impedance":50})"));
    const auto model = board::voxelize(snap, board::Selection{}, kPlan, kF0);
    REQUIRE(model.ports.size() == 1);
    const auto& port = model.ports[0];
    THEN("it has 2 cells in series (the two dielectric cells) and 3 strips (x nodes 4, 5, 6 mm)") {
      REQUIRE(port.series == 2);
      REQUIRE(port.parallel == 3);
      for (const auto& e : port.edges)
        REQUIRE(e.edge.component == fdtd::Z);
    }
    THEN("the positive terminal is the upper layer, so the sign is -1, and each strip presents 150 ohm") {
      REQUIRE(port.edges[0].sign == -1.0);
      double r = 0;
      for (const auto& e : port.edges)
        if (e.strip == 0)
          r += e.edge.resistance;
      REQUIRE(r == Approx(150.0));
    }
  }
}

SCENARIO("Refinement boxes grade the lateral grid") {
  GIVEN("the fixture board with a 0.25 mm refinement box around a port and 1 mm coarse cells") {
    const auto snap = board::parseSnapshot(fixture(
        1,
        R"({"name":"P1","layer":"F.Cu","a":[0.002,0.005],"b":[0.0036,0.005],"width":0.002,"impedance":50})"));
    auto plan = kPlan;
    plan.refine = board::refineAroundPorts(snap, 0.25e-3, 1e-3);
    const auto model = board::voxelize(snap, board::Selection{}, plan, kF0);
    const auto& g = model.grid;
    THEN("the cells next to the port are fine and the far cells are coarse") {
      double minX = 1, maxX = 0;
      for (std::size_t i = 0; i < g.cells(fdtd::X); ++i) {
        minX = std::min(minX, g.cellSize(fdtd::X, i));
        maxX = std::max(maxX, g.cellSize(fdtd::X, i));
      }
      REQUIRE(minX <= 0.25e-3 * (1 + 1e-9));
      REQUIRE(maxX > 0.9e-3);
    }
    THEN("the port still spans the gap, now with several cells in series") {
      REQUIRE(model.ports.size() == 1);
      REQUIRE(model.ports[0].series >= 4);
    }
  }
}
