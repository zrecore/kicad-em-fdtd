#include "board/voxelize.hpp"

#include <algorithm>
#include <cmath>
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
  auto layerIndex = [&](const std::string& name) -> std::size_t {
    for (std::size_t i = 0; i < s.stackup.size(); ++i)
      if (s.stackup[i].copper && s.stackup[i].name == name)
        return i;
    throw std::invalid_argument("copper shape on a layer that is not in the stackup: " + name);
  };

  std::set<std::string> leftOut;
  for (const CopperShape& shape : s.copper) {
    if (!sel.includes(shape.owner)) {
      leftOut.insert(shape.owner);
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
  for (const PortDef& def : s.ports) {
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
    m.ports.push_back(std::move(plan2));
  }

  std::sort(m.pecEdges.begin(), m.pecEdges.end(), [](const fdtd::PecEdge& a, const fdtd::PecEdge& b) {
    return a.axis != b.axis ? a.axis < b.axis : a.node < b.node;
  });
  m.pecEdges.erase(std::unique(m.pecEdges.begin(), m.pecEdges.end(),
                               [](const fdtd::PecEdge& a, const fdtd::PecEdge& b) {
                                 return a.axis == b.axis && a.node == b.node;
                               }),
                   m.pecEdges.end());

  for (const std::string& owner : leftOut)
    m.notes.push_back("left out of the model by the selection: " + owner);
  m.notes.push_back("copper is modelled as zero-thickness PEC sheets; dielectric thickness is used as given");
  return m;
}

} // namespace board
