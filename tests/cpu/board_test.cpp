// BDD scenarios for the board snapshot reader and the voxelizer. Lengths in the fixture are metres.
#include <algorithm>
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
std::string fixture(int version = 1, const std::string& ports = "", const std::string& body = "") {
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
    "components":[{"ref":"AE1","value":"antenna","x":0.005,"y":0.005,"layer":"F.Cu"})" +
         body + R"(]})";
}

// airGrowth 1: most scenarios below count nodes in an even air layout; the graded default has its own
// scenario
const board::GridPlan kPlan{
    .lateralCell = 1e-3, .maxDielectricCell = 0.4e-3, .airMargin = 2e-3, .airCell = 1e-3, .airGrowth = 1.0};
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

namespace {
// the antenna footprint's body: a 2.8 mm square over the pad, 1.8 mm tall (two 1 mm air cells above the
// board)
std::string withBody(const std::string& layer = "F.Cu", double height = 0.0018) {
  return R"(,{"ref":"U1","value":"shield","x":0.005,"y":0.005,"layer":")" + layer +
         R"(","body":{"polygon":[[0.0036,0.0036],[0.0064,0.0036],[0.0064,0.0064],[0.0036,0.0064]],"height":)" +
         std::to_string(height) + "}}";
}
std::size_t cellsWith(const board::Model& m, std::uint8_t id) {
  return static_cast<std::size_t>(std::count(m.cellMaterial.begin(), m.cellMaterial.end(), id));
}
} // namespace

SCENARIO("A component body can be left out, made a dielectric block, or made metal") {
  GIVEN("a board whose part U1 has a 2.8 mm square body 1.8 mm tall on the top side") {
    const auto snap = board::parseSnapshot(fixture(1, "", withBody()));
    auto epsAt = [](const board::Model& m, std::size_t i, std::size_t j, std::size_t k) {
      return m.materials[m.cellMaterial[m.grid.cellIndex(i, j, k)]].epsR;
    };
    WHEN("the default treatment is used") {
      const auto model = board::voxelize(snap, board::Selection{}, kPlan, kF0);
      THEN("the body is not modelled: the cells above the board are all air") {
        REQUIRE(epsAt(model, 6, 6, 4) == Approx(1.0));
        REQUIRE(model.materials.size() == 2); // air and the one dielectric
      }
    }
    WHEN("U1's body is a dielectric of permittivity 6") {
      board::Selection sel;
      sel.bodies["U1"] = {board::BodyKind::Dielectric, 6.0};
      const auto model = board::voxelize(snap, sel, kPlan, kF0);
      THEN("exactly the 2 x 2 x 2 cells under the outline (cell centres inside it, up to 1.8 mm) are eps 6") {
        std::size_t n = 0;
        for (std::size_t k = 0; k < model.grid.cells(fdtd::Z); ++k)
          for (std::size_t j = 0; j < model.grid.cells(fdtd::Y); ++j)
            for (std::size_t i = 0; i < model.grid.cells(fdtd::X); ++i)
              n += epsAt(model, i, j, k) == Approx(6.0);
        REQUIRE(n == 8);
        REQUIRE(epsAt(model, 6, 6, 4) == Approx(6.0));
        REQUIRE(epsAt(model, 7, 7, 5) == Approx(6.0));
        REQUIRE(epsAt(model, 5, 6, 4) == Approx(1.0)); // beside it
        REQUIRE(epsAt(model, 6, 6, 3) == Approx(4.4)); // the board itself is untouched
      }
      AND_THEN("the run notes name the part, the treatment and the cell count") {
        bool found = false;
        for (const auto& n : model.notes)
          found = found || (n.find("U1") != std::string::npos && n.find("8 cells") != std::string::npos);
        REQUIRE(found);
      }
    }
    WHEN("U1's body is metal") {
      board::Selection sel;
      sel.bodies["U1"] = {board::BodyKind::Metal, 1.0};
      const auto model = board::voxelize(snap, sel, kPlan, kF0);
      THEN("the same 8 cells are perfect conductor") {
        REQUIRE(model.materials.back().pec);
        REQUIRE(cellsWith(model, static_cast<std::uint8_t>(model.materials.size() - 1)) == 8);
      }
    }
    WHEN("U1 is a dielectric body but is not part of the selection") {
      board::Selection sel;
      sel.all = false;
      sel.defaultBody = {board::BodyKind::Dielectric, 6.0};
      const auto model = board::voxelize(snap, sel, kPlan, kF0);
      THEN("it leaves no trace in the model") {
        REQUIRE(epsAt(model, 6, 6, 4) == Approx(1.0));
      }
    }
    WHEN("the body is taller than the air above the board") {
      const auto tall = board::parseSnapshot(fixture(1, "", withBody("F.Cu", 0.02)));
      board::Selection sel;
      sel.defaultBody = {board::BodyKind::Dielectric, 6.0};
      const auto model = board::voxelize(tall, sel, kPlan, kF0);
      THEN("it is cut off at the grid edge and the notes say so") {
        bool found = false;
        for (const auto& n : model.notes)
          found = found || n.find("cut off") != std::string::npos;
        REQUIRE(found);
      }
    }
    WHEN("a body has no outline in the snapshot") {
      const auto bare = board::parseSnapshot(fixture());
      board::Selection sel;
      sel.defaultBody = {board::BodyKind::Dielectric, 6.0};
      const auto model = board::voxelize(bare, sel, kPlan, kF0);
      THEN("the part is left out with a note, not silently") {
        bool found = false;
        for (const auto& n : model.notes)
          found = found || n.find("no body outline") != std::string::npos;
        REQUIRE(found);
      }
    }
  }
}

SCENARIO("A fine layer of air next to the board resolves a body thinner than the coarse air cells") {
  GIVEN("a metal body 0.4 mm tall on a board whose air cells are 1 mm") {
    const auto snap = board::parseSnapshot(fixture(1, "", withBody("F.Cu", 0.0004)));
    board::Selection sel;
    sel.bodies["U1"] = {board::BodyKind::Metal, 1.0};
    WHEN("the air has no fine layer") {
      const auto model = board::voxelize(snap, sel, kPlan, kF0);
      THEN("no cell centre lies inside the body, and the run notes say it was left out") {
        bool found = false;
        for (const auto& n : model.notes)
          found =
              found || (n.find("U1") != std::string::npos && n.find("covers no cell") != std::string::npos);
        REQUIRE(found);
      }
    }
    WHEN("the first 1 mm of air uses 0.25 mm cells") {
      board::GridPlan plan = kPlan;
      plan.airNearCell = 0.25e-3;
      plan.airNearHeight = 1e-3;
      const auto model = board::voxelize(snap, sel, plan, kF0);
      THEN("the body fills 2 z cells under each of its 2 x 2 lateral cells: 8 cells") {
        REQUIRE(cellsWith(model, static_cast<std::uint8_t>(model.materials.size() - 1)) == 8);
      }
      AND_THEN("the air still spans the same margin and no cell is larger than the coarse air cell") {
        const auto& g = model.grid;
        const std::size_t nz = g.cells(fdtd::Z);
        REQUIRE(g.edge(fdtd::Z, nz) - g.edge(fdtd::Z, 0) ==
                Approx(0.0008 + 2 * kPlan.airMargin).margin(1e-9)); // 0.8 mm board plus both margins
        for (std::size_t k = 0; k < nz; ++k)
          REQUIRE(g.cellSize(fdtd::Z, k) <= kPlan.airCell + 1e-12);
      }
    }
  }
}

SCENARIO("The air next to the board grows from the board's own cell, by no more than the growth ratio") {
  GIVEN("a board with 0.4 mm cells and a coarse air cell of 1 mm") {
    const auto snap = board::parseSnapshot(fixture(1, "", ""));
    auto largestStep = [](const board::Model& m) {
      const auto& g = m.grid;
      double worst = 1.0;
      for (std::size_t k = 0; k + 1 < g.cells(fdtd::Z); ++k) {
        const double a = g.cellSize(fdtd::Z, k), b = g.cellSize(fdtd::Z, k + 1);
        worst = std::max(worst, std::max(a / b, b / a));
      }
      return worst;
    };
    WHEN("the plan uses the default growth ratio") {
      board::GridPlan plan = kPlan;
      plan.airGrowth = 1.3;
      const auto model = board::voxelize(snap, board::Selection{}, plan, kF0);
      THEN("no two neighbouring z cells differ by more than a factor 2 (the smooth-mesh rule)") {
        REQUIRE(largestStep(model) <= 2.0);
      }
      AND_THEN("the air still spans the same margin and no cell exceeds the coarse air cell") {
        const auto& g = model.grid;
        const std::size_t nz = g.cells(fdtd::Z);
        REQUIRE(g.edge(fdtd::Z, nz) - g.edge(fdtd::Z, 0) ==
                Approx(0.0008 + 2 * kPlan.airMargin).margin(1e-9));
        for (std::size_t k = 0; k < nz; ++k)
          REQUIRE(g.cellSize(fdtd::Z, k) <= kPlan.airCell + 1e-12);
      }
    }
    WHEN("the growth ratio is 1, the earlier even split of the air") {
      board::GridPlan plan = kPlan;
      plan.airGrowth = 1.0;
      const auto model = board::voxelize(snap, board::Selection{}, plan, kF0);
      THEN("the first air cell is the coarse cell, 2.5 times the board's cell") {
        REQUIRE(largestStep(model) == Approx(2.5).margin(1e-9));
      }
    }
  }
}

SCENARIO("A body can float off the board, like a metal strip on top of a part") {
  GIVEN("a metal body 0.25 mm tall whose near face is 0.5 mm above the board, in 0.25 mm air cells") {
    std::string comp = withBody("F.Cu", 0.00025);
    comp.insert(comp.find("\"height\""), "\"offset\":0.0005,");
    const auto snap = board::parseSnapshot(fixture(1, "", comp));
    board::Selection sel;
    sel.bodies["U1"] = {board::BodyKind::Metal, 1.0};
    board::GridPlan plan = kPlan;
    plan.airNearCell = 0.25e-3;
    plan.airNearHeight = 1e-3;
    const auto model = board::voxelize(snap, sel, plan, kF0);
    THEN("it fills one z cell, 0.5 to 0.75 mm above the board, and the air below it stays empty") {
      const std::uint8_t metal = static_cast<std::uint8_t>(model.materials.size() - 1);
      REQUIRE(model.materials.back().pec);
      REQUIRE(cellsWith(model, metal) == 4); // 2 x 2 lateral cells, one z cell
      const auto& g = model.grid;
      std::size_t k = 0; // the metal cell above the lateral cell (6, 6)
      while (k < g.cells(fdtd::Z) && model.cellMaterial[g.cellIndex(6, 6, k)] != metal)
        ++k;
      REQUIRE(k < g.cells(fdtd::Z));
      REQUIRE(model.cellMaterial[g.cellIndex(6, 6, k - 1)] == 0); // 0.25 to 0.5 mm above: air
      REQUIRE(model.cellMaterial[g.cellIndex(6, 6, k - 2)] == 0); // 0 to 0.25 mm above: air
      REQUIRE(model.cellMaterial[g.cellIndex(6, 6, k - 3)] != 0); // the board itself
      REQUIRE(g.cellSize(fdtd::Z, k) == Approx(0.25e-3).margin(1e-9));
    }
  }
}

SCENARIO("A component's pads and do-not-populate flag are read from the snapshot") {
  GIVEN("a part R5 with two pads on two nets, marked DNP and surface-mounted") {
    const auto snap =
        board::parseSnapshot(fixture(1, "",
                                     R"(,{"ref":"R5","value":"0R","x":0.005,"y":0.005,"layer":"F.Cu",
             "attributes":{"dnp":true,"mounting":"smd"},
             "pads":[{"number":"1","net":"FEED","x":0.004,"y":0.005},{"number":"2","net":"ANT","x":0.006,"y":0.005}]})"));
    THEN("the pads, their nets and positions, and the flags are available, within 1e-12 m") {
      const auto& c = snap.components.at(1);
      REQUIRE(c.dnp);
      REQUIRE(c.mounting == "smd");
      REQUIRE(c.pads.size() == 2);
      REQUIRE(c.pads[0].net == "FEED");
      REQUIRE(c.pads[1].x == Approx(0.006).margin(1e-12));
    }
    AND_THEN("the run notes name a fitted two-terminal part as not modelled and a DNP one as left open") {
      const auto model = board::voxelize(snap, board::Selection{}, kPlan, kF0);
      bool open = false, dnpNote = false;
      for (const auto& n : model.notes) {
        open = open || (n.find("not modelled yet") != std::string::npos);
        dnpNote =
            dnpNote || (n.find("do not populate") != std::string::npos && n.find("R5") != std::string::npos);
      }
      REQUIRE(!open); // R5 is DNP, so there is no fitted two-terminal part
      REQUIRE(dnpNote);
    }
    AND_THEN("a part without them reads as fitted, with no pads") {
      REQUIRE(!snap.components.at(0).dnp);
      REQUIRE(snap.components.at(0).pads.empty());
    }
  }
}

SCENARIO("A fitted two-terminal part whose value is not R, L or C is reported as not modelled") {
  GIVEN("a ferrite bead, known only by its part number, between two nets") {
    const auto snap =
        board::parseSnapshot(fixture(1, "",
                                     R"(,{"ref":"FB6","value":"BLM18","x":0.005,"y":0.005,"layer":"F.Cu",
             "pads":[{"number":"1","net":"FEED","x":0.004,"y":0.005},{"number":"2","net":"ANT","x":0.006,"y":0.005}]})"));
    THEN("the run notes say the gap is open and name the part") {
      const auto model = board::voxelize(snap, board::Selection{}, kPlan, kF0);
      bool found = false;
      for (const auto& n : model.notes)
        found =
            found || (n.find("not modelled yet") != std::string::npos && n.find("FB6") != std::string::npos);
      REQUIRE(found);
    }
  }
  GIVEN("a 0 ohm link whose pads have no copper under them in this snapshot") {
    const auto snap =
        board::parseSnapshot(fixture(1, "",
                                     R"(,{"ref":"R6","value":"0R","x":0.005,"y":0.005,"layer":"F.Cu",
             "pads":[{"number":"1","net":"FEED","x":0.004,"y":0.005},{"number":"2","net":"ANT","x":0.006,"y":0.005}]})"));
    THEN("the notes say its pads are not found, once, and do not also call it unmodelled") {
      const auto model = board::voxelize(snap, board::Selection{}, kPlan, kF0);
      int mentions = 0;
      for (const auto& n : model.notes)
        mentions += n.find("R6") != std::string::npos;
      REQUIRE(mentions == 1);
    }
  }
}

namespace {
// A part R6 on the top layer with pads 1 mm apart: pad 1 spans x 3-4 mm, pad 2 spans x 5-6 mm, both
// y 7.5-8.5.
board::Snapshot snapshotWithPart(const std::string& ref, const std::string& value, bool dnp = false) {
  std::string json =
      fixture(1, "",
              R"(,{"ref":")" + ref + R"(","value":")" + value + R"(","x":0.0045,"y":0.008,"layer":"F.Cu",)" +
                  (dnp ? R"("attributes":{"dnp":true},)" : "") +
                  R"("pads":[{"number":"1","net":"A","x":0.0035,"y":0.008},)" +
                  R"({"number":"2","net":"B","x":0.0055,"y":0.008}]})");
  const std::string pads =
      R"({"layer":"F.Cu","owner":")" + ref +
      R"(","net":"A","polygon":[[0.003,0.0075],[0.004,0.0075],[0.004,0.0085],[0.003,0.0085]]},)" +
      R"({"layer":"F.Cu","owner":")" + ref +
      R"(","net":"B","polygon":[[0.005,0.0075],[0.006,0.0075],[0.006,0.0085],[0.005,0.0085]]},)";
  json.replace(json.find(R"("copper":[)") + 10, 0, pads);
  return board::parseSnapshot(json);
}
// the x edge across the gap: cell 4-5 mm (index 6 from the -2 mm origin), node y = 8 mm (index 10), top
// copper
bool gapIsShort(const board::Model& m) {
  const auto& g = m.grid;
  const std::size_t node = g.nodeIndex(6, 10, m.copperPlaneNode[0]);
  return std::any_of(m.pecEdges.begin(), m.pecEdges.end(),
                     [&](const fdtd::PecEdge& e) { return e.axis == fdtd::X && e.node == node; });
}
bool noteContains(const board::Model& m, const std::string& a, const std::string& b) {
  return std::any_of(m.notes.begin(), m.notes.end(), [&](const std::string& n) {
    return n.find(a) != std::string::npos && n.find(b) != std::string::npos;
  });
}
} // namespace

SCENARIO("A fitted 0 ohm link or resistor between two pads is modelled across the gap") {
  GIVEN("a part R6 whose pads leave a one-cell gap") {
    WHEN("it is a 0R link") {
      const auto model = board::voxelize(snapshotWithPart("R6", "0R"), board::Selection{}, kPlan, kF0);
      THEN("the gap edge is a short (a forced edge) and no extra load is added") {
        REQUIRE(gapIsShort(model));
        REQUIRE(model.ports.empty()); // the fixture has no port, and none is added
        REQUIRE(noteContains(model, "R6", "short"));
      }
      AND_THEN("it is no longer reported as not modelled") {
        REQUIRE(!noteContains(model, "not modelled yet", "R6"));
      }
    }
    WHEN("it is a 100R resistor") {
      const auto model = board::voxelize(snapshotWithPart("R6", "100R"), board::Selection{}, kPlan, kF0);
      THEN("the gap edge is a lumped load of 100 ohm, within 1e-9, and not a short") {
        REQUIRE(!gapIsShort(model));
        REQUIRE(model.ports.size() == 1);
        REQUIRE(model.ports[0].name == "R6");
        REQUIRE(model.ports[0].impedance == Approx(100.0));
        REQUIRE(model.ports[0].edges.size() == 1);
        REQUIRE(model.ports[0].edges[0].edge.resistance == Approx(100.0).epsilon(1e-9));
      }
    }
    WHEN("it is marked do not populate") {
      const auto model = board::voxelize(snapshotWithPart("R6", "0R", true), board::Selection{}, kPlan, kF0);
      THEN("the gap stays open") {
        REQUIRE(!gapIsShort(model));
        REQUIRE(model.ports.empty());
      }
    }
    WHEN("it is not part of the selection") {
      board::Selection sel;
      sel.all = false;
      const auto model = board::voxelize(snapshotWithPart("R6", "0R"), sel, kPlan, kF0);
      THEN("it leaves no trace") {
        REQUIRE(!gapIsShort(model));
        REQUIRE(model.ports.empty());
      }
    }
    WHEN("it is an inductor footprint (L3) whose value is 0R, so a 0 ohm link was asked for") {
      const auto model = board::voxelize(snapshotWithPart("L3", "0R"), board::Selection{}, kPlan, kF0);
      THEN("the value wins: the gap is a short, and the notes say the reference disagreed") {
        REQUIRE(gapIsShort(model));
        REQUIRE(noteContains(model, "L3", "reference says L"));
      }
    }
    WHEN("it is a part with a bare number value on a non-resistor reference") {
      const auto model = board::voxelize(snapshotWithPart("C4", "47"), board::Selection{}, kPlan, kF0);
      THEN("it is not guessed to be ohms: the gap stays open and the notes say so") {
        REQUIRE(!gapIsShort(model));
        REQUIRE(noteContains(model, "not modelled yet", "C4"));
      }
    }
    WHEN("it is a 2 pF capacitor") {
      const auto model = board::voxelize(snapshotWithPart("C2", "2pF"), board::Selection{}, kPlan, kF0);
      THEN("the gap edge is a lumped edge with 2 pF, no conductance, and is not a short") {
        REQUIRE(!gapIsShort(model));
        REQUIRE(model.ports.size() == 1);
        REQUIRE(model.ports[0].name == "C2");
        REQUIRE(model.ports[0].edges.size() == 1);
        REQUIRE(model.ports[0].edges[0].edge.capacitance == Approx(2e-12).epsilon(1e-9));
        REQUIRE(std::isinf(model.ports[0].edges[0].edge.resistance));
        REQUIRE(!noteContains(model, "not modelled yet", "C2"));
      }
    }
    WHEN("it is a capacitor footprint whose value is 4p7 on reference R9, an odd reference") {
      const auto model = board::voxelize(snapshotWithPart("R9", "4p7"), board::Selection{}, kPlan, kF0);
      THEN("the value wins and the notes say the reference disagreed") {
        REQUIRE(model.ports.size() == 1);
        REQUIRE(model.ports[0].edges[0].edge.capacitance == Approx(4.7e-12).epsilon(1e-9));
        REQUIRE(noteContains(model, "R9", "reference says R"));
      }
    }
    WHEN("it is a 10 nH inductor, 4.7 nH on a reference that says R") {
      const auto model = board::voxelize(snapshotWithPart("L1", "10nH"), board::Selection{}, kPlan, kF0);
      THEN("the gap edge carries 10 nH, no conductance and no capacitance, and is not a short") {
        REQUIRE(!gapIsShort(model));
        REQUIRE(model.ports.size() == 1);
        const auto& e = model.ports[0].edges[0].edge;
        REQUIRE(e.inductance == Approx(10e-9).epsilon(1e-9));
        REQUIRE(std::isinf(e.resistance));
        REQUIRE(e.capacitance == 0);
        REQUIRE(!noteContains(model, "not modelled yet", "L1"));
      }
    }
    WHEN("it is a ferrite bead with a part number for a value") {
      const auto model = board::voxelize(snapshotWithPart("FB1", "BLM18"), board::Selection{}, kPlan, kF0);
      THEN("it is not guessed: the gap stays open and the notes say so") {
        REQUIRE(!gapIsShort(model));
        REQUIRE(model.ports.empty());
        REQUIRE(noteContains(model, "not modelled yet", "FB1"));
      }
    }
    WHEN("the user gives the bead's values: 600 ohm, 100 nH and 0.2 pF in parallel") {
      board::Selection sel;
      sel.parts["FB1"] = {board::PartTreatment::State::Values, false, 600.0, 100e-9, 0.2e-12};
      const auto model = board::voxelize(snapshotWithPart("FB1", "BLM18"), sel, kPlan, kF0);
      THEN("one edge carries all three, within 1e-9 relative") {
        REQUIRE(model.ports.size() == 1);
        const auto& e = model.ports[0].edges[0].edge;
        REQUIRE(e.resistance == Approx(600.0).epsilon(1e-9));
        REQUIRE(e.inductance == Approx(100e-9).epsilon(1e-9));
        REQUIRE(e.capacitance == Approx(0.2e-12).epsilon(1e-9));
        REQUIRE(!noteContains(model, "not modelled yet", "FB1"));
      }
    }
    WHEN("the user sets a DNP-marked part open, short, or fitted by value") {
      board::Selection open, shortIt, fit;
      open.parts["R6"].state = board::PartTreatment::State::Open;
      shortIt.parts["R6"].state = board::PartTreatment::State::Short;
      fit.parts["R6"].fitDnp = true;
      THEN("Open leaves the gap open even for a fitted 0R, Short closes a DNP part, and fitDnp fits a DNP "
           "0R by its value") {
        REQUIRE(!gapIsShort(board::voxelize(snapshotWithPart("R6", "0R"), open, kPlan, kF0)));
        REQUIRE(gapIsShort(board::voxelize(snapshotWithPart("R6", "100R", true), shortIt, kPlan, kF0)));
        REQUIRE(gapIsShort(board::voxelize(snapshotWithPart("R6", "0R", true), fit, kPlan, kF0)));
      }
    }
  }
}

SCENARIO("The voxelizer drops copper on nets that the net selection leaves out") {
  GIVEN("the fixture board: ground plane on GND, the antenna pad on FEED") {
    const auto snap = board::parseSnapshot(fixture());
    WHEN("only GND is selected") {
      board::Selection sel;
      sel.netFilter = true;
      sel.nets = {"GND"};
      const auto model = board::voxelize(snap, sel, kPlan, kF0);
      THEN("the pad is gone, the ground plane stays, and the notes name the net left out") {
        REQUIRE(edgesInPlane(model, 4) == 0);
        REQUIRE(edgesInPlane(model, 2) == 180);
        REQUIRE(noteContains(model, "net selection", "FEED"));
      }
    }
    WHEN("GND and FEED are selected") {
      board::Selection sel;
      sel.netFilter = true;
      sel.nets = {"GND", "FEED"};
      const auto model = board::voxelize(snap, sel, kPlan, kF0);
      THEN("nothing is dropped") {
        REQUIRE(edgesInPlane(model, 4) == 12);
        REQUIRE(edgesInPlane(model, 2) == 180);
      }
    }
  }
}

SCENARIO("A port as wide as the copper it feeds is flagged in the run notes") {
  GIVEN("the fixture's 2.8 mm pad with a vertical port 2.8 mm wide, and with one 0.8 mm wide") {
    auto make = [](double width) {
      return board::parseSnapshot(
          fixture(1, R"({"name":"P1","layer":"F.Cu","layer_b":"B.Cu","a":[0.005,0.005],"width":)" +
                         std::to_string(width) + R"(,"impedance":50})"));
    };
    THEN("the wide port is flagged, with both widths, and the narrow one is not") {
      const auto wide = board::voxelize(make(0.0028), board::Selection{}, kPlan, kF0);
      const auto narrow = board::voxelize(make(0.0008), board::Selection{}, kPlan, kF0);
      REQUIRE(noteContains(wide, "port 'P1'", "apparent loss"));
      REQUIRE(noteContains(wide, "2.8", "wide"));
      REQUIRE(!noteContains(narrow, "port 'P1'", "apparent loss"));
    }
  }
}

SCENARIO("A via narrower than the grid cell still joins the layers") {
  GIVEN("a 0.65 mm via placed midway between grid nodes on a 1 mm lateral grid") {
    std::string text = fixture();
    text.replace(
        text.find("\"vias\":[]"), 9,
        R"("vias":[{"x":0.0055,"y":0.0055,"diameter":0.00065,"drill":0.0003,"net":"GND","owner":""}])");
    const auto snap = board::parseSnapshot(text);
    const auto model = board::voxelize(snap, board::Selection{}, kPlan, kF0);
    THEN("a column of conducting edges runs through the whole dielectric") {
      std::size_t columns = 0;
      for (const auto& e : model.pecEdges)
        columns += e.axis == fdtd::Z;
      REQUIRE(columns > 0);
    }
  }
}

SCENARIO("Grid lines snap to the corners of a modelled body, so a 0.04 mm strip is resolved") {
  GIVEN("a metal body 0.04 mm wide, placed off a 0.05 mm grid inside a fine refinement box") {
    const std::string strip =
        R"(,{"ref":"U1","value":"strip","x":0.005,"y":0.005,"layer":"F.Cu","body":{"polygon":[[0.005013,0.0036],[0.005053,0.0036],[0.005053,0.0064],[0.005013,0.0064]],"height":0.0018}})";
    const auto snap = board::parseSnapshot(fixture(1, "", strip));
    board::Selection sel;
    sel.bodies["U1"] = {board::BodyKind::Metal, 1.0};
    board::GridPlan plan = kPlan;
    plan.refine = {{0.004, 0.003, 0.006, 0.007, 0.05e-3}};
    auto strong = [&](bool snapped) {
      plan.snapToGeometry = snapped;
      const auto model = board::voxelize(snap, sel, plan, kF0);
      return cellsWith(model, static_cast<std::uint8_t>(model.materials.size() - 1));
    };
    THEN("with snapping the strip is exactly one cell wide, so it is modelled") {
      const std::size_t withSnap = strong(true);
      REQUIRE(withSnap > 0);
      AND_THEN("it holds more cells than without snapping, where a cell centre may miss it") {
        REQUIRE(withSnap >= strong(false));
      }
    }
  }
}

SCENARIO("Grid lines snap to the long edges of a traced shape inside a refinement box") {
  GIVEN("a 0.25 mm trace, off the 0.1 mm grid, drawn with more than eight vertices as a vendor Gerber trace "
        "is") {
    std::string text = fixture();
    const std::string patch = "[[0.0036,0.0036],[0.0064,0.0036],[0.0064,0.0064],[0.0036,0.0064]]";
    const std::string trace =
        "[[0.0050137,0.003],[0.0052637,0.003],[0.0052637,0.0045],[0.0052637,0.005],[0.0052637,0.0055],"
        "[0.0052637,0.007],[0.0050137,0.007],[0.0050137,0.0055],[0.0050137,0.005],[0.0050137,0.0045]]";
    text.replace(text.find(patch), patch.size(), trace);
    const auto snap = board::parseSnapshot(text);
    board::GridPlan plan = kPlan;
    plan.refine = {{0.004, 0.002, 0.006, 0.008, 0.1e-3}};
    auto widthInCells = [&](bool snapped) {
      plan.snapToGeometry = snapped;
      const auto model = board::voxelize(snap, board::Selection{}, plan, kF0);
      double lo = -1, hi = -1;
      for (std::size_t i = 0; i < model.grid.nodes(fdtd::X); ++i) {
        const double x = model.grid.edge(fdtd::X, i) + model.originX;
        if (std::abs(x - 0.0050137) < 1e-9)
          lo = x;
        if (std::abs(x - 0.0052637) < 1e-9)
          hi = x;
      }
      return std::pair{lo, hi};
    };
    THEN("with snapping both long edges are grid lines, so the trace is exactly 0.25 mm wide") {
      const auto [lo, hi] = widthInCells(true);
      REQUIRE(lo > 0);
      REQUIRE(hi > 0);
    }
    AND_THEN("without snapping they are not (the cell edges fall where the uniform grid puts them)") {
      const auto [lo, hi] = widthInCells(false);
      REQUIRE((lo < 0 || hi < 0));
    }
  }
}

SCENARIO("A via can span only some of the copper layers, as a blind or micro via does") {
  GIVEN("a three-layer board with a via drilled from the top layer to the first inner layer only") {
    auto text = [](const std::string& via) {
      return R"({"format":"kicad-em-fdtd.snapshot","version":1,"board":{"name":"hdi","kicad_version":"10"},
        "outline":[[0,0],[0.01,0],[0.01,0.01],[0,0.01]],
        "stackup":[{"name":"F.Cu","kind":"copper"},
                   {"name":"d1","kind":"dielectric","thickness":0.0004,"epsilon_r":4.4,"loss_tangent":0.02},
                   {"name":"In1.Cu","kind":"copper"},
                   {"name":"d2","kind":"dielectric","thickness":0.0004,"epsilon_r":4.4,"loss_tangent":0.02},
                   {"name":"B.Cu","kind":"copper"}],
        "copper":[],"vias":[)" +
             via + R"(],"components":[]})";
    };
    auto columnEdges = [&](const std::string& via) {
      const auto model = board::voxelize(board::parseSnapshot(text(via)), board::Selection{}, kPlan, kF0);
      std::size_t n = 0;
      for (const auto& e : model.pecEdges)
        n += e.axis == fdtd::Z;
      return n;
    };
    const std::string base = R"("x":0.005,"y":0.005,"diameter":0.0006,"drill":0.0003,"net":"GND","owner":"")";
    const std::size_t full = columnEdges("{" + base + "}");
    const std::size_t blind = columnEdges("{" + base + R"(,"from":"F.Cu","to":"In1.Cu"})");
    THEN("it has fewer conducting edges than a through via, and about half as many here") {
      REQUIRE(full > 0);
      REQUIRE(blind > 0);
      REQUIRE(blind < full);
      REQUIRE(columnEdges("{" + base + R"(,"from":"In1.Cu","to":"F.Cu"})") == blind); // order does not matter
    }
    AND_THEN("naming a layer that is not copper is an error") {
      REQUIRE_THROWS(columnEdges("{" + base + R"(,"from":"F.Cu","to":"nowhere"})"));
    }
  }
}

SCENARIO("A cutout in the board is air, not dielectric") {
  GIVEN("the fixture board with a 2 mm square hole through its middle") {
    std::string text = fixture();
    const auto solid = board::parseSnapshot(text);
    text.replace(text.find("\"stackup\""), 9,
                 R"("cutouts":[[[0.004,0.004],[0.006,0.004],[0.006,0.006],[0.004,0.006]]],"stackup")");
    const auto holed = board::parseSnapshot(text);
    REQUIRE(holed.cutouts.size() == 1);
    const auto a = board::voxelize(solid, board::Selection{}, kPlan, kF0);
    const auto b = board::voxelize(holed, board::Selection{}, kPlan, kF0);
    auto boardCells = [](const board::Model& m) {
      return static_cast<std::size_t>(std::count_if(m.cellMaterial.begin(), m.cellMaterial.end(),
                                                    [&](std::uint8_t id) { return id == 1; }));
    };
    THEN("the four 1 mm columns inside the hole, two cells deep, are air") {
      REQUIRE(boardCells(a) - boardCells(b) == 8);
    }
  }
}

SCENARIO("A dielectric body can carry a loss tangent") {
  GIVEN("a body of permittivity 6 and loss tangent 0.01") {
    const auto snap = board::parseSnapshot(fixture(1, "", withBody()));
    board::Selection sel;
    sel.bodies["U1"] = {board::BodyKind::Dielectric, 6.0, 0.01};
    const auto model = board::voxelize(snap, sel, kPlan, kF0);
    THEN("its material has the equivalent conductivity at the centre frequency") {
      const auto& m = model.materials.back();
      REQUIRE(m.epsR == Approx(6.0));
      REQUIRE(m.sigma == Approx(2 * 3.14159265358979 * kF0 * 8.8541878128e-12 * 6.0 * 0.01).epsilon(1e-6));
    }
  }
  GIVEN("two bodies with the same permittivity but different loss") {
    THEN("they are different materials") {
      const auto snap = board::parseSnapshot(fixture(1, "", withBody()));
      board::Selection a;
      a.bodies["U1"] = {board::BodyKind::Dielectric, 6.0, 0.0};
      board::Selection b;
      b.bodies["U1"] = {board::BodyKind::Dielectric, 6.0, 0.02};
      const auto ma = board::voxelize(snap, a, kPlan, kF0), mb = board::voxelize(snap, b, kPlan, kF0);
      REQUIRE(ma.materials.back().sigma == Approx(0.0));
      REQUIRE(mb.materials.back().sigma > 0.0);
    }
  }
}

namespace {
bool anyNote(const board::Model& m, const std::string& a, const std::string& b = "") {
  return std::any_of(m.notes.begin(), m.notes.end(), [&](const std::string& n) {
    return n.find(a) != std::string::npos && (b.empty() || n.find(b) != std::string::npos);
  });
}
std::string twoShapes(const std::string& netA, const std::string& netB, double bx0) {
  return R"({"format":"kicad-em-fdtd.snapshot","version":1,"board":{"name":"t","kicad_version":"10"},
    "outline":[[0,0],[0.01,0],[0.01,0.01],[0,0.01]],
    "stackup":[{"name":"F.Cu","kind":"copper"},
               {"name":"core","kind":"dielectric","thickness":0.0008,"epsilon_r":4.4,"loss_tangent":0.02},
               {"name":"B.Cu","kind":"copper"}],
    "copper":[{"layer":"F.Cu","owner":"","net":")" +
         netA + R"(","polygon":[[0.002,0.002],[0.005,0.002],[0.005,0.008],[0.002,0.008]]},
              {"layer":"F.Cu","owner":"","net":")" +
         netB + R"(","polygon":[[)" + std::to_string(bx0) + R"(,0.002],[0.008,0.002],[0.008,0.008],[)" +
         std::to_string(bx0) + R"(,0.008]]}],
    "vias":[],"components":[]})";
}
} // namespace

SCENARIO("Copper of two nets that touch is reported, copper of one net is not") {
  GIVEN("a feed track and a ground pour whose edges overlap by 0.5 mm") {
    const auto m =
        board::voxelize(board::parseSnapshot(twoShapes("RF", "GND", 0.0045)), board::Selection{}, kPlan, kF0);
    THEN("the notes name both nets and the layer") {
      REQUIRE(anyNote(m, "RF", "GND"));
      REQUIRE(anyNote(m, "F.Cu"));
    }
  }
  GIVEN("the same two shapes 1 mm apart") {
    const auto m =
        board::voxelize(board::parseSnapshot(twoShapes("RF", "GND", 0.006)), board::Selection{}, kPlan, kF0);
    THEN("nothing is reported") {
      REQUIRE_FALSE(anyNote(m, "touch"));
    }
  }
  GIVEN("a feed track and a ground pour with a gap of 0.3 mm, under the 1 mm cell") {
    const auto m =
        board::voxelize(board::parseSnapshot(twoShapes("RF", "GND", 0.0053)), board::Selection{}, kPlan, kF0);
    THEN("the notes say the nets touch in the grid, though no edge is claimed twice") {
      REQUIRE(anyNote(m, "RF", "GND"));
    }
  }
  GIVEN("overlapping shapes of the same net") {
    const auto m = board::voxelize(board::parseSnapshot(twoShapes("GND", "GND", 0.0045)), board::Selection{},
                                   kPlan, kF0);
    THEN("nothing is reported") {
      REQUIRE_FALSE(anyNote(m, "touch"));
    }
  }
}

SCENARIO("A gap between two nets that is only a few cells wide near a port is reported") {
  // a feed pad (net FEED) and a ground pour (net GND) with a gap between them, an in-plane port across the
  // gap
  auto gapBoard = [](double gapMm) {
    const double bx0 = 5.0 + gapMm;
    return R"({"format":"kicad-em-fdtd.snapshot","version":1,"board":{"name":"t","kicad_version":"10"},
      "outline":[[0,0],[0.02,0],[0.02,0.01],[0,0.01]],
      "stackup":[{"name":"F.Cu","kind":"copper"},
                 {"name":"core","kind":"dielectric","thickness":0.0008,"epsilon_r":4.4,"loss_tangent":0.02},
                 {"name":"B.Cu","kind":"copper"}],
      "copper":[{"layer":"F.Cu","owner":"","net":"FEED","polygon":[[0.002,0.002],[0.005,0.002],[0.005,0.008],[0.002,0.008]]},
                {"layer":"F.Cu","owner":"","net":"GND","polygon":[[)" +
           std::to_string(bx0 * 1e-3) + R"(,0.002],[0.018,0.002],[0.018,0.008],[)" +
           std::to_string(bx0 * 1e-3) +
           R"(,0.008]]}],
      "vias":[],"components":[],
      "ports":[{"name":"P1","layer":"F.Cu","a":[0.0045,0.005],"b":[)" +
           std::to_string((bx0 + 0.5) * 1e-3) + R"(,0.005],"width":0.0005,"impedance":50}]})";
  };
  GIVEN("copper of two nets 2 mm apart on a 1 mm grid, with a port across the gap") {
    const auto m = board::voxelize(board::parseSnapshot(gapBoard(2.0)), board::Selection{}, kPlan, kF0);
    THEN("the notes say the gap is a few cells wide, name both nets and say what to do") {
      REQUIRE(anyNote(m, "FEED", "GND"));
      REQUIRE(anyNote(m, "cells wide"));
      REQUIRE(anyNote(m, "refine"));
    }
  }
  GIVEN("the same two nets 7 mm apart") {
    const auto m = board::voxelize(board::parseSnapshot(gapBoard(7.0)), board::Selection{}, kPlan, kF0);
    THEN("nothing is reported about the gap") {
      REQUIRE_FALSE(anyNote(m, "cells wide"));
    }
  }
}

SCENARIO("A port whose terminals are joined by copper, or that floats, is reported") {
  auto withPort = [](const std::string& copper, const std::string& vias) {
    return R"({"format":"kicad-em-fdtd.snapshot","version":1,"board":{"name":"t","kicad_version":"10"},
      "outline":[[0,0],[0.01,0],[0.01,0.01],[0,0.01]],
      "stackup":[{"name":"F.Cu","kind":"copper"},
                 {"name":"core","kind":"dielectric","thickness":0.0008,"epsilon_r":4.4,"loss_tangent":0.02},
                 {"name":"B.Cu","kind":"copper"}],
      "copper":[{"layer":"B.Cu","owner":"","net":"GND","polygon":[[0.0002,0.0002],[0.0098,0.0002],[0.0098,0.0098],[0.0002,0.0098]]},)" +
           copper + R"(],
      "vias":[)" +
           vias + R"(],"components":[],
      "ports":[{"name":"J1","layer":"F.Cu","layer_b":"B.Cu","a":[0.003,0.005],"width":0.0005,"impedance":50}]})";
  };
  const std::string strip =
      R"({"layer":"F.Cu","owner":"","net":"RF","polygon":[[0.002,0.0045],[0.008,0.0045],[0.008,0.0055],[0.002,0.0055]]})";
  GIVEN("a strip over a ground plane, with no via") {
    const auto m = board::voxelize(board::parseSnapshot(withPort(strip, "")), board::Selection{}, kPlan, kF0);
    THEN("the port is not reported as shorted") {
      REQUIRE_FALSE(anyNote(m, "joined by copper"));
    }
  }
  GIVEN("the same strip with a via from its far end to the ground plane") {
    const auto m = board::voxelize(
        board::parseSnapshot(withPort(
            strip, R"({"x":0.0075,"y":0.005,"diameter":0.0004,"drill":0.0002,"net":"GND","owner":""})")),
        board::Selection{}, kPlan, kF0);
    THEN("the notes say the port's terminals are joined by copper") {
      REQUIRE(anyNote(m, "port 'J1'", "joined by copper"));
    }
  }
  GIVEN("a port placed where there is no copper on its driven layer") {
    const auto m = board::voxelize(
        board::parseSnapshot(withPort(
            R"({"layer":"F.Cu","owner":"","net":"RF","polygon":[[0.007,0.0045],[0.008,0.0045],[0.008,0.0055],[0.007,0.0055]]})",
            "")),
        board::Selection{}, kPlan, kF0);
    THEN("the notes say a terminal floats") {
      REQUIRE(anyNote(m, "port 'J1'", "floats"));
    }
  }
}

SCENARIO("Thin dielectric bodies, touching metal bodies and model labels are reported in the notes") {
  const std::string two =
      R"(,{"ref":"M1","value":"a","x":0.005,"y":0.005,"layer":"F.Cu","body":{"polygon":[[0.0036,0.0036],[0.0050,0.0036],[0.0050,0.0064],[0.0036,0.0064]],"height":0.0018}},
          {"ref":"M2","value":"b","x":0.005,"y":0.005,"layer":"F.Cu","body":{"polygon":[[0.0050,0.0036],[0.0064,0.0036],[0.0064,0.0064],[0.0050,0.0064]],"height":0.0018}})";
  const auto snap = board::parseSnapshot(fixture(1, "", two));
  GIVEN("two metal bodies that share an edge") {
    board::Selection sel;
    sel.bodies["M1"] = {board::BodyKind::Metal, 1.0};
    sel.bodies["M2"] = {board::BodyKind::Metal, 1.0};
    const auto m = board::voxelize(snap, sel, kPlan, kF0);
    THEN("the notes say they touch in the grid") {
      REQUIRE(anyNote(m, "M1", "touch in the grid"));
    }
  }
  GIVEN("a dielectric body 0.9 mm tall on 1 mm air cells, so one cell thick") {
    const auto thin = board::parseSnapshot(fixture(1, "", withBody("F.Cu", 0.0009)));
    board::Selection sel;
    sel.bodies["U1"] = {board::BodyKind::Dielectric, 6.0};
    const auto m = board::voxelize(thin, sel, kPlan, kF0);
    THEN("the notes say it is poorly resolved") {
      REQUIRE(anyNote(m, "U1", "poorly resolved"));
    }
  }
  GIVEN("a dielectric body with and without a label") {
    board::Selection sel;
    sel.bodies["M1"] = {board::BodyKind::Dielectric, 6.0};
    THEN("an unlabelled body is called assumed, a labelled one prints its label") {
      REQUIRE(anyNote(board::voxelize(snap, sel, kPlan, kF0), "M1", "assumed, not fitted"));
      sel.bodies["M1"].label = "not fitted: poor datasheet match";
      REQUIRE(anyNote(board::voxelize(snap, sel, kPlan, kF0), "M1", "not fitted: poor datasheet match"));
    }
  }
}

SCENARIO("A metal component body joins the copper it sits on in the port-path check") {
  GIVEN("a feed strip, a separate ground patch, a via from the patch to the plane, and a metal body bridging "
        "the strip and the patch") {
    const std::string text =
        R"({"format":"kicad-em-fdtd.snapshot","version":1,"board":{"name":"t","kicad_version":"10"},
      "outline":[[0,0],[0.01,0],[0.01,0.01],[0,0.01]],
      "stackup":[{"name":"F.Cu","kind":"copper"},
                 {"name":"core","kind":"dielectric","thickness":0.0008,"epsilon_r":4.4,"loss_tangent":0.02},
                 {"name":"B.Cu","kind":"copper"}],
      "copper":[{"layer":"B.Cu","owner":"","net":"GND","polygon":[[0.0002,0.0002],[0.0098,0.0002],[0.0098,0.0098],[0.0002,0.0098]]},
                {"layer":"F.Cu","owner":"","net":"RF","polygon":[[0.002,0.0045],[0.005,0.0045],[0.005,0.0055],[0.002,0.0055]]},
                {"layer":"F.Cu","owner":"","net":"GND","polygon":[[0.0065,0.0035],[0.0085,0.0035],[0.0085,0.0065],[0.0065,0.0065]]}],
      "vias":[{"x":0.0075,"y":0.005,"diameter":0.0006,"drill":0.0003,"net":"GND","owner":""}],
      "components":[{"ref":"X1","value":"clip","x":0.0055,"y":0.005,"layer":"F.Cu",
                     "body":{"polygon":[[0.0035,0.004],[0.0075,0.004],[0.0075,0.006],[0.0035,0.006]],"height":0.0009}}],
      "ports":[{"name":"J1","layer":"F.Cu","layer_b":"B.Cu","a":[0.003,0.005],"width":0.0005,"impedance":50}]})";
    board::Selection sel;
    sel.bodies["X1"] = {board::BodyKind::Metal, 1.0};
    const auto withBody = board::voxelize(board::parseSnapshot(text), sel, kPlan, kF0);
    const auto without = board::voxelize(board::parseSnapshot(text), board::Selection{}, kPlan, kF0);
    THEN("with the body the port's terminals are joined by copper, without it they are not") {
      REQUIRE(anyNote(withBody, "port 'J1'", "joined by copper"));
      REQUIRE_FALSE(anyNote(without, "joined by copper"));
    }
  }
}

SCENARIO("A via that lands on copper of another net is reported") {
  auto board = [](const std::string& viaNet, double viaX) {
    return R"({"format":"kicad-em-fdtd.snapshot","version":1,"board":{"name":"t","kicad_version":"10"},
      "outline":[[0,0],[0.01,0],[0.01,0.01],[0,0.01]],
      "stackup":[{"name":"F.Cu","kind":"copper"},
                 {"name":"core","kind":"dielectric","thickness":0.0008,"epsilon_r":4.4,"loss_tangent":0.02},
                 {"name":"B.Cu","kind":"copper"}],
      "copper":[{"layer":"F.Cu","owner":"","net":"RF","polygon":[[0.002,0.0045],[0.006,0.0045],[0.006,0.0055],[0.002,0.0055]]}],
      "vias":[{"x":)" +
           std::to_string(viaX) + R"(,"y":0.005,"diameter":0.0006,"drill":0.0003,"net":")" + viaNet +
           R"(","owner":""}],"components":[]})";
  };
  GIVEN("a ground via in the middle of an RF strip") {
    const auto m = board::voxelize(board::parseSnapshot(board("GND", 0.004)), board::Selection{}, kPlan, kF0);
    THEN("the notes say it short-circuits the RF net") {
      REQUIRE(anyNote(m, "via of net 'GND'", "RF"));
    }
  }
  GIVEN("a ground via 3 mm from the strip, and an RF via on the strip") {
    THEN("neither is reported") {
      REQUIRE_FALSE(
          anyNote(board::voxelize(board::parseSnapshot(board("GND", 0.008)), board::Selection{}, kPlan, kF0),
                  "via of net"));
      REQUIRE_FALSE(
          anyNote(board::voxelize(board::parseSnapshot(board("RF", 0.004)), board::Selection{}, kPlan, kF0),
                  "via of net"));
    }
  }
}

SCENARIO("A via that is only joined to another net by the grid is not called a short circuit") {
  // a ground via 0.5 mm clear of an RF strip's edge on a 1 mm grid: legal on the board, joined in the model
  const std::string text =
      R"({"format":"kicad-em-fdtd.snapshot","version":1,"board":{"name":"t","kicad_version":"10"},
      "outline":[[0,0],[0.01,0],[0.01,0.01],[0,0.01]],
      "stackup":[{"name":"F.Cu","kind":"copper"},
                 {"name":"core","kind":"dielectric","thickness":0.0008,"epsilon_r":4.4,"loss_tangent":0.02},
                 {"name":"B.Cu","kind":"copper"}],
      "copper":[{"layer":"F.Cu","owner":"","net":"RF","polygon":[[0.002,0.0054],[0.006,0.0054],[0.006,0.0064],[0.002,0.0064]]}],
      "vias":[{"x":0.004,"y":0.0046,"diameter":0.0006,"drill":0.0003,"net":"GND","owner":""}],"components":[]})";
  GIVEN("that board on a grid whose cell is wider than the gap") {
    const auto m = board::voxelize(board::parseSnapshot(text), board::Selection{}, kPlan, kF0);
    THEN("the note says the grid joins them, gives the real gap, and does not say short-circuit") {
      REQUIRE(anyNote(m, "via of net 'GND'", "RF"));
      REQUIRE(anyNote(m, "via of net 'GND'", "the grid"));
      REQUIRE(anyNote(m, "via of net 'GND'", "0.50 mm"));
      REQUIRE_FALSE(anyNote(m, "short-circuits"));
    }
  }
}

SCENARIO("A via is checked against the copper of every layer it crosses, not only its end layers") {
  GIVEN("a through via on a three-copper-layer board whose middle layer has a strip of another net there") {
    const std::string text =
        R"({"format":"kicad-em-fdtd.snapshot","version":1,"board":{"name":"t","kicad_version":"10"},
      "outline":[[0,0],[0.01,0],[0.01,0.01],[0,0.01]],
      "stackup":[{"name":"F.Cu","kind":"copper"},
                 {"name":"d1","kind":"dielectric","thickness":0.0004,"epsilon_r":4.4,"loss_tangent":0.02},
                 {"name":"In1.Cu","kind":"copper"},
                 {"name":"d2","kind":"dielectric","thickness":0.0004,"epsilon_r":4.4,"loss_tangent":0.02},
                 {"name":"B.Cu","kind":"copper"}],
      "copper":[{"layer":"In1.Cu","owner":"","net":"RF","polygon":[[0.002,0.0045],[0.006,0.0045],[0.006,0.0055],[0.002,0.0055]]}],
      "vias":[{"x":0.004,"y":0.005,"diameter":0.0006,"drill":0.0003,"net":"GND","owner":""}],
      "components":[]})";
    const auto m = board::voxelize(board::parseSnapshot(text), board::Selection{}, kPlan, kF0);
    THEN("the notes say it short-circuits the RF strip on In1.Cu") {
      REQUIRE(anyNote(m, "via of net 'GND'", "In1.Cu"));
    }
  }
}

namespace {
// The fixture with a finished copper thickness on both layers.
std::string thickFixture(double thicknessM) {
  std::string text = fixture();
  const std::string t = ",\"thickness\":" + std::to_string(thicknessM);
  for (const char* layer : {"F.Cu", "B.Cu"}) {
    const std::string from = std::string("{\"name\":\"") + layer + "\",\"kind\":\"copper\"";
    text.replace(text.find(from), from.size(), from + t);
  }
  return text;
}
std::size_t edgesOf(const board::Model& m, fdtd::Axis component, std::size_t zNode) {
  const auto& g = m.grid;
  std::size_t n = 0;
  for (const auto& e : m.pecEdges)
    n += e.axis == component && (e.node / (g.nodes(fdtd::X) * g.nodes(fdtd::Y))) == zNode;
  return n;
}
} // namespace

SCENARIO("Copper of a given thickness is a slab of cells when asked and a sheet otherwise") {
  const double t = 35e-6;
  GIVEN("the fixture with 35 um of copper on both layers") {
    const board::Snapshot snap = board::parseSnapshot(thickFixture(t));
    const board::Snapshot bare = board::parseSnapshot(fixture());
    board::GridPlan sheetPlan = kPlan, slabPlan = kPlan;
    slabPlan.copperSlab = true;
    WHEN("the default (sheet) model is built") {
      const auto withT = board::voxelize(snap, board::Selection{}, sheetPlan, kF0);
      const auto without = board::voxelize(bare, board::Selection{}, sheetPlan, kF0);
      THEN("the thickness changes nothing: same grid, same PEC edges, same note") {
        REQUIRE(withT.grid.cellCount() == without.grid.cellCount());
        REQUIRE(withT.pecEdges.size() == without.pecEdges.size());
        REQUIRE(anyNote(withT, "zero-thickness PEC sheets"));
      }
    }
    WHEN("the slab model is built") {
      const auto m = board::voxelize(snap, board::Selection{}, slabPlan, kF0);
      const auto sheet = board::voxelize(snap, board::Selection{}, sheetPlan, kF0);
      const std::size_t top = m.copperPlaneNode[0], bottom = m.copperPlaneNode[2];
      const auto& g = m.grid;
      THEN("the top copper fills one z cell of its own thickness above its dielectric interface") {
        REQUIRE(g.edge(fdtd::Z, top + 1) - g.edge(fdtd::Z, top) == Approx(t).margin(1e-9));
      }
      AND_THEN("the bottom copper fills one z cell below its interface") {
        REQUIRE(g.edge(fdtd::Z, bottom) - g.edge(fdtd::Z, bottom - 1) == Approx(t).margin(1e-9));
      }
      AND_THEN("the pad is PEC in both planes of its slab and PEC along z between them, nowhere else") {
        const std::size_t padEx = edgesOf(sheet, fdtd::X, sheet.copperPlaneNode[0]);
        REQUIRE(padEx > 0);
        REQUIRE(edgesOf(m, fdtd::X, top) == padEx);
        REQUIRE(edgesOf(m, fdtd::X, top + 1) == padEx);
        REQUIRE(edgesOf(sheet, fdtd::Z, sheet.copperPlaneNode[0]) == 0);
        REQUIRE(edgesOf(m, fdtd::Z, top) > 0);
        REQUIRE(edgesOf(m, fdtd::Z, top + 1) == 0);
      }
      AND_THEN("the ground plane gets the same: PEC in the planes of its slab (below its interface)") {
        const std::size_t gndEx = edgesOf(sheet, fdtd::X, sheet.copperPlaneNode[2]);
        REQUIRE(gndEx > 0);
        REQUIRE(edgesOf(m, fdtd::X, bottom - 1) == gndEx);
        REQUIRE(edgesOf(m, fdtd::Z, bottom - 1) > 0);
      }
      AND_THEN("the note says copper is slabs of its finished thickness") {
        REQUIRE(anyNote(m, "slab"));
        REQUIRE_FALSE(anyNote(m, "zero-thickness PEC sheets"));
      }
    }
    WHEN("the slab model is asked for but a layer has no thickness") {
      const auto m = board::voxelize(bare, board::Selection{}, slabPlan, kF0);
      const auto sheet = board::voxelize(bare, board::Selection{}, kPlan, kF0);
      THEN("that layer stays a sheet, and the note says which") {
        REQUIRE(m.pecEdges.size() == sheet.pecEdges.size());
        REQUIRE(anyNote(m, "no thickness"));
      }
    }
  }
}

SCENARIO("A slab's walls are solid on both sides when grid lines fall exactly on its edges") {
  GIVEN("the pad with grid lines snapped onto its corners (x 3.6 and 6.4 mm), 35 um of copper, slab model") {
    const board::Snapshot snap = board::parseSnapshot(thickFixture(35e-6));
    board::GridPlan plan = kPlan;
    plan.copperSlab = true;
    plan.snapToGeometry = true;
    const auto m = board::voxelize(snap, board::Selection{}, plan, kF0);
    const auto& g = m.grid;
    const std::size_t top = m.copperPlaneNode[0];
    auto columns = [&](fdtd::Axis axis, std::size_t plane) {
      std::vector<double> xs;
      for (const auto& e : m.pecEdges)
        if (e.axis == axis && e.node / (g.nodes(fdtd::X) * g.nodes(fdtd::Y)) == plane)
          xs.push_back((g.edge(fdtd::X, e.node % g.nodes(fdtd::X)) + m.originX) * 1e3);
      std::sort(xs.begin(), xs.end());
      xs.erase(std::unique(xs.begin(), xs.end(), [](double a, double b) { return std::abs(a - b) < 1e-6; }),
               xs.end());
      return xs;
    };
    THEN("Ez and Ey are PEC on the column at each edge of the pad, left and right alike") {
      for (const auto& [axis, plane] :
           {std::pair{fdtd::Z, top}, std::pair{fdtd::Y, top}, std::pair{fdtd::Y, top + 1}}) {
        const auto xs = columns(axis, plane);
        REQUIRE_FALSE(xs.empty());
        INFO("axis " << static_cast<int>(axis) << " plane " << plane << " first " << xs.front() << " last "
                     << xs.back());
        REQUIRE(xs.front() == Approx(3.6).margin(1e-6));
        REQUIRE(xs.back() == Approx(6.4).margin(1e-6));
      }
    }
  }
}
