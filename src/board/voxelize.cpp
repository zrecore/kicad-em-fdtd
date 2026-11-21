#include "board/voxelize.hpp"

#include "board/parts.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <stdexcept>

#include "board/mesh.hpp"
#include "core/units.hpp"

namespace board {
namespace {

using fdtd::Axis;

bool insideRing(const Ring& r, double x, double y) { // even-odd ray casting
  bool in = false;
  for (std::size_t a = 0, b = r.size() - 1; a < r.size(); b = a++)
    if ((r[a].y > y) != (r[b].y > y) && x < (r[b].x - r[a].x) * (y - r[a].y) / (r[b].y - r[a].y) + r[a].x)
      in = !in;
  return in;
}

bool insideShape(const CopperShape& s, double x, double y) {
  if (!insideRing(s.outer, x, y))
    return false;
  return std::none_of(s.holes.begin(), s.holes.end(), [&](const Ring& h) { return insideRing(h, x, y); });
}

// n equal cells covering [lo, hi] with cells no larger than `cell`.
void appendUniform(std::vector<double>& edges, double lo, double hi, double cell) {
  const std::size_t n =
      std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil((hi - lo) / cell - 1e-9)));
  if (edges.empty())
    edges.push_back(lo);
  for (std::size_t i = 1; i <= n; ++i)
    edges.push_back(lo + (hi - lo) * static_cast<double>(i) / static_cast<double>(n));
}

std::size_t nearestEdge(const std::vector<double>& edges, double z) {
  return static_cast<std::size_t>(
      std::min_element(edges.begin(), edges.end(),
                       [&](double a, double b) { return std::abs(a - z) < std::abs(b - z); }) -
      edges.begin());
}

struct ZStack {
  std::vector<double> edges;               // z node positions, bottom to top
  std::vector<std::size_t> layerFirstCell; // per dielectric (bottom to top): first z cell
  std::vector<std::size_t> layerCells;
  std::vector<std::size_t> dielectric; // stackup indices of the dielectrics, bottom to top
  std::vector<double> interfaceZ;      // dielectric count + 1 interface heights, bottom to top
};

ZStack buildZ(const Snapshot& s, const GridPlan& plan) {
  ZStack z;
  for (std::size_t i = s.stackup.size(); i-- > 0;)
    if (!s.stackup[i].copper)
      z.dielectric.push_back(i);
  if (z.dielectric.empty())
    throw std::invalid_argument("stackup has no dielectric layer");
  appendUniform(z.edges, -plan.airMargin, 0.0, plan.airCell);
  double top = 0;
  z.interfaceZ.push_back(0.0);
  for (std::size_t d : z.dielectric) {
    const double t = s.stackup[d].thickness;
    z.layerFirstCell.push_back(z.edges.size() - 1);
    const std::size_t before = z.edges.size();
    appendUniform(z.edges, top, top + t, plan.maxDielectricCell);
    z.layerCells.push_back(z.edges.size() - before);
    top += t;
    z.interfaceZ.push_back(top);
  }
  appendUniform(z.edges, top, top + plan.airMargin, plan.airCell);
  return z;
}

} // namespace

std::vector<RefineBox> refineAroundPorts(const Snapshot& s, double cell, double pad) {
  std::vector<RefineBox> boxes;
  for (const PortDef& p : s.ports)
    boxes.push_back(
        {std::min(p.a.x, p.b.x) - pad - 0.5 * p.width, std::min(p.a.y, p.b.y) - pad - 0.5 * p.width,
         std::max(p.a.x, p.b.x) + pad + 0.5 * p.width, std::max(p.a.y, p.b.y) + pad + 0.5 * p.width, cell});
  return boxes;
}

Model voxelize(const Snapshot& s, const Selection& sel, const GridPlan& plan, double f0) {
  const ZStack z = buildZ(s, plan);

  double minX = s.outline[0].x, maxX = minX, minY = s.outline[0].y, maxY = minY;
  for (const Point& p : s.outline) {
    minX = std::min(minX, p.x);
    maxX = std::max(maxX, p.x);
    minY = std::min(minY, p.y);
    maxY = std::max(maxY, p.y);
  }
  std::array<std::vector<double>, 3> edges;
  if (plan.refine.empty() && !plan.snapToGeometry) {
    appendUniform(edges[0], minX - plan.airMargin, maxX + plan.airMargin, plan.lateralCell);
    appendUniform(edges[1], minY - plan.airMargin, maxY + plan.airMargin, plan.lateralCell);
  } else {
    std::vector<Refinement> rx, ry;
    for (const RefineBox& b : plan.refine) {
      rx.push_back({b.x0, b.x1, b.cell});
      ry.push_back({b.y0, b.y1, b.cell});
    }
    // Positions that should be grid lines: the outline, simple (pad-like) copper shapes, and port ends.
    // Shapes with many vertices (arcs, zone fills) would add hundreds of lines and are skipped.
    std::vector<double> sx, sy;
    if (plan.snapToGeometry) {
      auto add = [&](double x, double y) {
        sx.push_back(x);
        sy.push_back(y);
      };
      for (const Point& p : s.outline)
        add(p.x, p.y);
      for (const CopperShape& c : s.copper)
        if (sel.includes(c.owner) && c.outer.size() <= 12)
          for (const Point& p : c.outer)
            add(p.x, p.y);
      for (const PortDef& p : s.ports) {
        add(p.a.x, p.a.y);
        add(p.b.x, p.b.y);
      }
    }
    edges[0] = gradedEdges(minX - plan.airMargin, maxX + plan.airMargin, plan.lateralCell, rx, 1.3, sx);
    edges[1] = gradedEdges(minY - plan.airMargin, maxY + plan.airMargin, plan.lateralCell, ry, 1.3, sy);
  }
  edges[2] = z.edges;
  const double originX = edges[0].front(), originY = edges[1].front();
  // The solver works in grid coordinates starting at 0; board coordinates are shifted by the origin.
  for (int a = 0; a < 3; ++a) {
    const double o = edges[a].front();
    for (double& e : edges[a])
      e -= o;
  }
  const double zShift = -z.edges.front(); // board bottom (z = 0) sits at grid z = zShift

  Model m{fdtd::Grid(edges)};
  m.copperPlaneNode.assign(s.stackup.size(), 0);
  m.originX = originX;
  m.originY = originY;
  const fdtd::Grid& g = m.grid;
  const auto toBoardX = [&](double gx) { return gx + originX; };
  const auto toBoardY = [&](double gy) { return gy + originY; };

  // Materials: id 0 is air, then one per dielectric layer (loss tangent as an equivalent conductivity at f0).
  m.materials.push_back({});
  for (std::size_t d : z.dielectric) {
    const Layer& l = s.stackup[d];
    m.materials.push_back(
        {l.epsilonR, 2 * std::numbers::pi * f0 * fdtd::units::kEps0 * l.epsilonR * l.lossTangent, false});
  }
  m.cellMaterial.assign(g.cellCount(), 0);
  for (std::size_t k = 0; k < z.dielectric.size(); ++k)
    for (std::size_t kk = z.layerFirstCell[k]; kk < z.layerFirstCell[k] + z.layerCells[k]; ++kk)
      for (std::size_t j = 0; j < g.cells(fdtd::Y); ++j)
        for (std::size_t i = 0; i < g.cells(fdtd::X); ++i) {
          const double cx = toBoardX(g.edge(fdtd::X, i) + 0.5 * g.cellSize(fdtd::X, i));
          const double cy = toBoardY(g.edge(fdtd::Y, j) + 0.5 * g.cellSize(fdtd::Y, j));
          if (insideRing(s.outline, cx, cy))
            m.cellMaterial[g.cellIndex(i, j, kk)] = static_cast<std::uint8_t>(k + 1);
        }

  // z node of every copper layer: the dielectric interface it sits on.
  std::vector<std::size_t> copperNode(s.stackup.size(), 0);
  std::size_t below = 0; // dielectrics already passed, counted from the top of the stackup
  for (std::size_t i = 0; i < s.stackup.size(); ++i) {
    if (!s.stackup[i].copper) {
      ++below;
      continue;
    }
    const double zInterface =
        z.interfaceZ[z.dielectric.size() - below]; // interface under `below` dielectrics from the top
    copperNode[i] = nearestEdge(edges[2], zInterface + zShift);
    m.copperPlaneNode[i] = copperNode[i];
  }
  // Component bodies: the footprint outline extruded away from the board on the component's side. Only air
  // cells whose centre lies inside are changed, so a body never overwrites the board, and a body thinner than
  // half a cell (or beyond the air margin) is reported, not silently dropped.
  {
    std::size_t topCopper = s.stackup.size(), bottomCopper = s.stackup.size();
    for (std::size_t i = 0; i < s.stackup.size(); ++i)
      if (s.stackup[i].copper) {
        if (topCopper == s.stackup.size())
          topCopper = i;
        bottomCopper = i;
      }
    std::vector<std::pair<BodyTreatment, std::uint8_t>> bodyMaterial; // one material per distinct treatment
    for (const Component& c : s.components) {
      const BodyTreatment t = sel.bodyOf(c.ref);
      if (!sel.includes(c.ref) || t.kind == BodyKind::Ignore)
        continue;
      const double height = c.bodyHeight > 0 ? c.bodyHeight : sel.defaultBodyHeight;
      if (c.body.empty() || !(height > 0)) {
        m.notes.push_back("component " + c.ref +
                          ": no body outline (or no height) in the snapshot, body left out");
        continue;
      }
      const bool top = topCopper < s.stackup.size() && c.layer == s.stackup[topCopper].name;
      const bool bottom = bottomCopper < s.stackup.size() && c.layer == s.stackup[bottomCopper].name;
      if (!top && !bottom) {
        m.notes.push_back("component " + c.ref + ": its layer '" + c.layer +
                          "' is not an outer copper layer, body left out");
        continue;
      }
      std::uint8_t id = 0;
      for (const auto& [bt, bid] : bodyMaterial)
        if (bt.kind == t.kind && (t.kind == BodyKind::Metal || bt.epsilonR == t.epsilonR))
          id = bid;
      if (id == 0) {
        id = static_cast<std::uint8_t>(m.materials.size());
        m.materials.push_back(t.kind == BodyKind::Metal ? fdtd::Material{1.0, 0.0, true}
                                                        : fdtd::Material{t.epsilonR, 0.0, false});
        bodyMaterial.push_back({t, id});
      }
      const double surface = top ? z.interfaceZ.back() + zShift : z.interfaceZ.front() + zShift;
      const double zLo = top ? surface : surface - height, zHi = top ? surface + height : surface;
      std::size_t cells = 0;
      for (std::size_t kk = 0; kk < g.cells(fdtd::Z); ++kk) {
        const double cz = g.edge(fdtd::Z, kk) + 0.5 * g.cellSize(fdtd::Z, kk);
        if (cz <= zLo || cz >= zHi)
          continue;
        for (std::size_t j = 0; j < g.cells(fdtd::Y); ++j)
          for (std::size_t i = 0; i < g.cells(fdtd::X); ++i) {
            const double cx = toBoardX(g.edge(fdtd::X, i) + 0.5 * g.cellSize(fdtd::X, i));
            const double cy = toBoardY(g.edge(fdtd::Y, j) + 0.5 * g.cellSize(fdtd::Y, j));
            std::uint8_t& cell = m.cellMaterial[g.cellIndex(i, j, kk)];
            if (cell == 0 && insideRing(c.body, cx, cy)) {
              cell = id;
              ++cells;
            }
          }
      }
      const double reach = top ? g.edge(fdtd::Z, g.cells(fdtd::Z)) - surface : surface - g.edge(fdtd::Z, 0);
      std::string how = t.kind == BodyKind::Metal
                            ? "metal body"
                            : "dielectric body, epsilon_r " + std::to_string(t.epsilonR);
      if (cells == 0)
        m.notes.push_back("component " + c.ref + ": " + how +
                          " covers no cell (too thin or too small for the grid), left out");
      else if (height > reach)
        m.notes.push_back("component " + c.ref + ": " + how +
                          " is taller than the air margin, cut off at the grid edge");
      else
        m.notes.push_back("component " + c.ref + ": " + how + ", " + std::to_string(cells) + " cells");
    }
  }
  auto layerIndex = [&](const std::string& name) -> std::size_t {
    for (std::size_t i = 0; i < s.stackup.size(); ++i)
      if (s.stackup[i].copper && s.stackup[i].name == name)
        return i;
    throw std::invalid_argument("copper shape on a layer that is not in the stackup: " + name);
  };

  std::set<std::string> leftOut, netLeftOut;
  for (const CopperShape& shape : s.copper) {
    if (!sel.includes(shape.owner)) {
      leftOut.insert(shape.owner);
      continue;
    }
    if (!sel.netIncluded(shape.net)) {
      netLeftOut.insert(shape.net.empty() ? "(no net)" : shape.net);
      continue;
    }
    const std::size_t k = copperNode[layerIndex(shape.layer)];
    for (std::size_t j = 0; j <= g.cells(fdtd::Y); ++j)
      for (std::size_t i = 0; i < g.cells(fdtd::X); ++i) { // Ex edges: centre of cell i, node j
        const double x = toBoardX(g.edge(fdtd::X, i) + 0.5 * g.cellSize(fdtd::X, i));
        if (insideShape(shape, x, toBoardY(g.edge(fdtd::Y, j))))
          m.pecEdges.push_back({fdtd::X, g.nodeIndex(i, j, k)});
      }
    for (std::size_t j = 0; j < g.cells(fdtd::Y); ++j)
      for (std::size_t i = 0; i <= g.cells(fdtd::X); ++i) { // Ey edges: node i, centre of cell j
        const double y = toBoardY(g.edge(fdtd::Y, j) + 0.5 * g.cellSize(fdtd::Y, j));
        if (insideShape(shape, toBoardX(g.edge(fdtd::X, i)), y))
          m.pecEdges.push_back({fdtd::Y, g.nodeIndex(i, j, k)});
      }
  }

  // Vias: PEC columns (Ez edges) between the lowest and highest copper sheet.
  std::size_t kLow = g.nodes(fdtd::Z), kHigh = 0;
  for (std::size_t i = 0; i < s.stackup.size(); ++i)
    if (s.stackup[i].copper) {
      kLow = std::min(kLow, copperNode[i]);
      kHigh = std::max(kHigh, copperNode[i]);
    }
  for (const Via& v : s.vias) {
    if (!sel.includes(v.owner)) {
      leftOut.insert(v.owner);
      continue;
    }
    if (!sel.netIncluded(v.net)) {
      netLeftOut.insert(v.net.empty() ? "(no net)" : v.net);
      continue;
    }
    const double r = 0.5 * v.diameter;
    for (std::size_t j = 0; j < g.nodes(fdtd::Y); ++j)
      for (std::size_t i = 0; i < g.nodes(fdtd::X); ++i) {
        const double dx = toBoardX(g.edge(fdtd::X, i)) - v.x, dy = toBoardY(g.edge(fdtd::Y, j)) - v.y;
        if (dx * dx + dy * dy <= r * r)
          for (std::size_t k = kLow; k < kHigh; ++k)
            m.pecEdges.push_back({fdtd::Z, g.nodeIndex(i, j, k)});
      }
  }

  // Ports: in-plane (between two metal areas of one layer) or vertical (between two copper layers).
  const std::array<double, 3> org{originX, originY, -zShift}; // board coordinate of grid position 0, per axis
  auto nearestNode = [&](Axis ax, double pos) {
    std::size_t best = 1;
    for (std::size_t j = 1; j + 1 < g.nodes(ax); ++j)
      if (std::abs(org[ax] + g.edge(ax, j) - pos) < std::abs(org[ax] + g.edge(ax, best) - pos))
        best = j;
    return best;
  };
  auto expandGap = [&](const PortDef& def) {
    const bool vertical = !def.layerB.empty();
    Axis d, t, f;          // along the gap, across it (parallel strips), and the fixed third axis
    double a0, b0, across; // gap ends along d (a positive) and the strip centre along t
    std::size_t fixedNode;
    if (vertical) {
      const std::size_t kA = copperNode[layerIndex(def.layer)], kB = copperNode[layerIndex(def.layerB)];
      d = fdtd::Z, t = fdtd::X, f = fdtd::Y;
      a0 = org[2] + g.edge(fdtd::Z, kA);
      b0 = org[2] + g.edge(fdtd::Z, kB);
      across = def.a.x;
      fixedNode = nearestNode(f, def.a.y);
    } else {
      const bool alongX = std::abs(def.b.x - def.a.x) >= std::abs(def.b.y - def.a.y);
      d = alongX ? fdtd::X : fdtd::Y, t = alongX ? fdtd::Y : fdtd::X, f = fdtd::Z;
      a0 = alongX ? def.a.x : def.a.y, b0 = alongX ? def.b.x : def.b.y;
      across = alongX ? def.a.y : def.a.x;
      fixedNode = copperNode[layerIndex(def.layer)];
    }
    const double lo = std::min(a0, b0), hi = std::max(a0, b0);
    const double origin = org[d], originT = org[t];
    PortPlan plan2{def.name, def.impedance, 0, 0, {}};
    std::vector<std::size_t> cells, strips;
    for (std::size_t i = 0; i < g.cells(d); ++i) {
      const double c = origin + g.edge(d, i) + 0.5 * g.cellSize(d, i);
      if (c > lo && c < hi)
        cells.push_back(i);
    }
    for (std::size_t j = 0; j < g.nodes(t); ++j) {
      const double pos = originT + g.edge(t, j);
      if (std::abs(pos - across) <= 0.5 * def.width && j > 0 && j + 1 < g.nodes(t))
        strips.push_back(j);
    }
    if (strips.empty()) // thin port: the single nearest node
      strips.push_back(nearestNode(t, across));
    if (cells.empty()) { // gap narrower than a cell: use the cell nearest the gap centre and say so
      std::size_t best = 0;
      const double mid = 0.5 * (lo + hi);
      for (std::size_t i = 1; i < g.cells(d); ++i)
        if (std::abs(origin + g.edge(d, i) + 0.5 * g.cellSize(d, i) - mid) <
            std::abs(origin + g.edge(d, best) + 0.5 * g.cellSize(d, best) - mid))
          best = i;
      cells.push_back(best);
      m.notes.push_back("port '" + def.name + "': the gap is narrower than a cell, widened to one " +
                        std::to_string(g.cellSize(d, best) * 1e3) + " mm cell");
    }
    double length = 0;
    for (std::size_t i : cells)
      length += g.cellSize(d, i);
    plan2.series = cells.size();
    plan2.parallel = strips.size();
    for (std::size_t s2 = 0; s2 < strips.size(); ++s2)
      for (std::size_t i : cells) {
        const double dl = g.cellSize(d, i);
        std::array<std::size_t, 3> idx{};
        idx[d] = i;
        idx[t] = strips[s2];
        idx[f] = fixedNode;
        plan2.edges.push_back({{idx[0], idx[1], idx[2], d,
                                def.impedance * static_cast<double>(strips.size()) * dl / length, nullptr},
                               s2,
                               dl / length,
                               b0 >= a0 ? 1.0 : -1.0});
        const fdtd::PecEdge port{d, g.nodeIndex(idx[0], idx[1], idx[2])};
        m.pecEdges.erase(std::remove_if(m.pecEdges.begin(), m.pecEdges.end(),
                                        [&](const fdtd::PecEdge& e) {
                                          return e.axis == port.axis && e.node == port.node;
                                        }),
                         m.pecEdges.end());
      }
    return plan2;
  };
  for (const PortDef& def : s.ports) {
    m.ports.push_back(expandGap(def));
    // A port that spans (nearly) the whole width of the copper it feeds averages a voltage that is not
    // uniform across the strips, so |S11| of a lossless load reads below 1 (measured: docs/PLAN.md, "Wide
    // ports").
    if (def.width > 0) {
      double narrowest = 1e300;
      for (const CopperShape& cs : s.copper)
        if (cs.layer == def.layer && insideRing(cs.outer, def.a.x, def.a.y)) {
          double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
          for (const Point& q : cs.outer) {
            x0 = std::min(x0, q.x), x1 = std::max(x1, q.x), y0 = std::min(y0, q.y), y1 = std::max(y1, q.y);
          }
          narrowest = std::min(narrowest, std::min(x1 - x0, y1 - y0));
        }
      if (narrowest < 1e299 && def.width > 0.6 * narrowest)
        m.notes.push_back(
            "port '" + def.name + "' is " + std::to_string(def.width * 1e3) + " mm wide on copper " +
            std::to_string(narrowest * 1e3) +
            " mm across: its strips see different voltages, so |S11| "
            "of a lossless load reads below 1 (apparent loss, growing with frequency) and the radiation "
            "efficiency is overstated; a port under about a third of the copper width reads true");
    }
  }

  // Fitted two-terminal resistors (and 0 ohm links) across the gap between their two pads: the gap runs from
  // the inner edge of one pad's copper to the inner edge of the other, across the overlap of the two pads'
  // widths. A 0 ohm value forces the gap edges (a short); another value is a passive lumped load.
  std::set<std::string> lumpedRefs;
  for (const Component& c : s.components) {
    if (!sel.includes(c.ref) || c.pads.size() != 2 || c.pads[0].net == c.pads[1].net)
      continue;
    const auto found = sel.parts.find(c.ref);
    const PartModel pm = modelPart(c, found == sel.parts.end() ? PartTreatment{} : found->second);
    if (!pm.note.empty())
      m.notes.push_back(pm.note);
    if (pm.kind == PartModel::Kind::Unmodelled)
      continue; // reported below as not modelled
    if (pm.kind == PartModel::Kind::Open) {
      if (pm.note.empty() && c.dnp)
        continue; // reported below as do not populate
      lumpedRefs.insert(c.ref);
      continue;
    }
    const std::optional<double> rOhm = pm.r, lHenry = pm.l, cFarad = pm.c;
    const bool shortIt = pm.shortIt;
    const std::string asRead = pm.asRead;
    auto padBox = [&](const ComponentPad& p, std::array<double, 4>& box) {
      for (const CopperShape& cs : s.copper)
        if (cs.owner == c.ref && cs.layer == c.layer && insideRing(cs.outer, p.x, p.y)) {
          box = {1e9, 1e9, -1e9, -1e9};
          for (const Point& q : cs.outer) {
            box[0] = std::min(box[0], q.x);
            box[1] = std::min(box[1], q.y);
            box[2] = std::max(box[2], q.x);
            box[3] = std::max(box[3], q.y);
          }
          return true;
        }
      return false;
    };
    std::array<double, 4> A{}, B{};
    if (!padBox(c.pads[0], A) || !padBox(c.pads[1], B) || layerIndex(c.layer) >= s.stackup.size()) {
      m.notes.push_back("component " + c.ref + ": its pads are not found as copper on " + c.layer +
                        ", left open");
      lumpedRefs.insert(c.ref); // already reported: not again as "not modelled"
      continue;
    }
    const bool alongX = std::abs(c.pads[1].x - c.pads[0].x) >= std::abs(c.pads[1].y - c.pads[0].y);
    const int lo = alongX ? 0 : 1, hi = alongX ? 2 : 3, tl = alongX ? 1 : 0, th = alongX ? 3 : 2;
    const bool aFirst = (A[lo] + A[hi]) <= (B[lo] + B[hi]);
    const auto& first = aFirst ? A : B;
    const auto& second = aFirst ? B : A;
    const double gapLo = first[hi], gapHi = second[lo];
    const double wLo = std::max(A[tl], B[tl]), wHi = std::min(A[th], B[th]);
    if (!(gapHi > gapLo) || !(wHi > wLo)) {
      m.notes.push_back("component " + c.ref +
                        ": its pads touch, overlap or do not face each other, left open");
      lumpedRefs.insert(c.ref);
      continue;
    }
    PortDef def;
    def.name = c.ref;
    def.layer = c.layer;
    def.impedance = rOhm && *rOhm > 0 ? *rOhm : 1.0;
    def.width = wHi - wLo;
    const double across = 0.5 * (wLo + wHi);
    def.a = alongX ? Point{gapLo, across} : Point{across, gapLo};
    def.b = alongX ? Point{gapHi, across} : Point{across, gapHi};
    PortPlan gapPlan = expandGap(def);
    gapPlan.lumped = true;
    std::string what;
    if (shortIt) {
      for (const PortEdge& pe : gapPlan.edges)
        m.pecEdges.push_back({pe.edge.component, g.nodeIndex(pe.edge.i, pe.edge.j, pe.edge.k)});
      m.notes.push_back("component " + c.ref + ": 0 ohm (" + asRead +
                        "), modelled as a short across the gap (" + std::to_string(gapPlan.edges.size()) +
                        " edge(s))");
    } else {
      // Equal voltage per unit length across the gap's cells: a series chain of cells with parallel strips
      // gives R for the whole gap with R_edge = R n dl / length (already set by the port code), L_edge =
      // L n dl / length, and C_edge = C length / (n dl) (the same field in every cell, the same charge).
      for (PortEdge& pe : gapPlan.edges) {
        const double share = pe.share, n = static_cast<double>(gapPlan.parallel);
        if (!(rOhm && *rOhm > 0))
          pe.edge.resistance = std::numeric_limits<double>::infinity();
        if (lHenry)
          pe.edge.inductance = *lHenry * n * share;
        if (cFarad)
          pe.edge.capacitance = *cFarad / (n * share);
      }
      auto si = [](double v, const char* unit) {
        char buf[40];
        std::snprintf(buf, sizeof buf, "%.4g %s", v, unit);
        return std::string(buf);
      };
      std::string parts;
      if (rOhm && *rOhm > 0)
        parts += si(*rOhm, "ohm");
      if (lHenry)
        parts += (parts.empty() ? "" : " || ") + si(*lHenry * 1e9, "nH");
      if (cFarad)
        parts += (parts.empty() ? "" : " || ") + si(*cFarad * 1e12, "pF");
      m.notes.push_back("component " + c.ref + ": " + parts + " (" + asRead + "), " +
                        std::to_string(gapPlan.series) + " cell(s) across the gap, " +
                        std::to_string(gapPlan.parallel) + " strip(s); ideal elements, no parasitics");
      m.ports.push_back(std::move(gapPlan));
    }
    lumpedRefs.insert(c.ref);
  }

  std::sort(m.pecEdges.begin(), m.pecEdges.end(), [](const fdtd::PecEdge& a, const fdtd::PecEdge& b) {
    return a.axis != b.axis ? a.axis < b.axis : a.node < b.node;
  });
  m.pecEdges.erase(std::unique(m.pecEdges.begin(), m.pecEdges.end(),
                               [](const fdtd::PecEdge& a, const fdtd::PecEdge& b) {
                                 return a.axis == b.axis && a.node == b.node;
                               }),
                   m.pecEdges.end());

  for (const std::string& n : sel.netNotes)
    m.notes.push_back(n);
  if (!netLeftOut.empty()) {
    std::string list;
    for (const std::string& n : netLeftOut)
      list += (list.empty() ? "" : ", ") + n;
    m.notes.push_back("left out of the model by the net selection (copper and vias on): " + list);
  }
  for (const std::string& owner : leftOut)
    m.notes.push_back("left out of the model by the selection: " + owner);
  // Two-terminal parts (two pads on two nets) are not modelled yet: the gap between their pads is open.
  {
    std::string fitted, unfitted;
    for (const Component& c : s.components) {
      if (!sel.includes(c.ref) || c.pads.size() != 2 || c.pads[0].net == c.pads[1].net ||
          lumpedRefs.count(c.ref))
        continue;
      (c.dnp ? unfitted : fitted) += (c.dnp ? unfitted : fitted).empty() ? c.ref : ", " + c.ref;
    }
    if (!fitted.empty())
      m.notes.push_back(
          "two-terminal parts whose value is not a resistance, capacitance or inductance are not modelled "
          "yet (ferrite beads, part numbers): the gap between their pads is an open circuit; give "
          "them values to model them: " +
          fitted);
    if (!unfitted.empty())
      m.notes.push_back("do not populate, left open between the pads: " + unfitted);
  }
  m.notes.push_back("copper is modelled as zero-thickness PEC sheets; dielectric thickness is used as given");
  return m;
}

} // namespace board
