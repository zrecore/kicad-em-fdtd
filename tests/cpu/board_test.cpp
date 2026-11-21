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
