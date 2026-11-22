#include "board/antenna.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <nlohmann/json.hpp>

namespace board {

using Json = nlohmann::json;

std::vector<AntennaRule> parseAntennaRules(std::string_view text, bool fromProject) {
  std::vector<AntennaRule> out;
  try {
    const Json j = Json::parse(text);
    for (const Json& r : j.at("rules")) {
      AntennaRule a;
      a.id = r.at("id");
      a.source = r.value("source", "");
      a.status = r.value("status", "unverified");
      a.placement = r.value("placement", "");
      a.fromProject = fromProject;
      a.parts = r.value("parts", std::vector<std::string>{});
      a.keepoutAlongMm = r.value("keepout_along_mm", 0.0);
      a.keepoutDepthMm = r.value("keepout_depth_mm", 0.0);
      if (r.contains("test_board_mm")) {
        a.testBoardLengthMm = r.at("test_board_mm").at(0);
        a.testBoardWidthMm = r.at("test_board_mm").at(1);
      }
      out.push_back(std::move(a));
    }
  } catch (const std::exception& e) {
    throw SnapshotError(std::string("antenna rules: ") + e.what());
  }
  return out;
}

std::vector<AntennaRule> mergeRules(const std::vector<AntennaRule>& shipped,
                                    const std::vector<AntennaRule>& project) {
  std::vector<AntennaRule> out = project;
  for (const AntennaRule& r : shipped) {
    bool redefined = false;
    for (const AntennaRule& p : project)
      redefined = redefined || p.id == r.id;
    if (!redefined)
      out.push_back(r);
  }
  return out;
}

static std::string lower(std::string s) {
  for (char& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

const AntennaRule* ruleFor(const std::vector<AntennaRule>& rules, const Component& c) {
  const std::string hay = lower(c.value + " " + c.ref);
  for (const AntennaRule& r : rules)
    for (const std::string& p : r.parts)
      if (!p.empty() && hay.find(lower(p)) != std::string::npos)
        return &r;
  return nullptr;
}

namespace {
using P = Point;

bool insideRing(const Ring& r, P p) {
  bool hit = false;
  for (std::size_t i = 0, n = r.size(); i < n; ++i) {
    const P &a = r[i], &b = r[(i + 1) % n];
    if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
      hit = !hit;
  }
  return hit;
}

// Copper is a union of overlapping shapes (zones, pads, tracks), so areas come from painting every shape
// into a grid of cells (even-odd, so holes stay empty) and counting the cells.
struct Raster {
  double u0, v0, cell;
  int nu, nv;
  std::vector<std::uint8_t> on;
  Raster(double u0_, double v0_, double u1, double v1, double cell_)
      : u0(u0_), v0(v0_), cell(cell_), nu(std::max(1, static_cast<int>(std::ceil((u1 - u0_) / cell_)))),
        nv(std::max(1, static_cast<int>(std::ceil((v1 - v0_) / cell_)))),
        on(static_cast<std::size_t>(nu) * static_cast<std::size_t>(nv), 0) {}
  // Paints the shape (rings already in this raster's frame); returns the cells it covers.
  std::size_t paint(const std::vector<const Ring*>& rings) {
    std::vector<std::vector<double>> xs(static_cast<std::size_t>(nv));
    for (const Ring* r : rings)
      for (std::size_t i = 0, n = r->size(); i < n; ++i) {
        const P a = (*r)[i], b = (*r)[(i + 1) % n];
        if (a.y == b.y)
          continue;
        const double lo = std::min(a.y, b.y), hi = std::max(a.y, b.y);
        const int j0 = std::max(0, static_cast<int>(std::ceil((lo - v0) / cell - 0.5)));
        const int j1 = std::min(nv - 1, static_cast<int>(std::ceil((hi - v0) / cell - 0.5)) - 1);
        for (int j = j0; j <= j1; ++j) {
          const double yc = v0 + (j + 0.5) * cell;
          if (yc >= lo && yc < hi)
            xs[static_cast<std::size_t>(j)].push_back(a.x + (yc - a.y) / (b.y - a.y) * (b.x - a.x));
        }
      }
    std::size_t covered = 0;
    for (int j = 0; j < nv; ++j) {
      auto& row = xs[static_cast<std::size_t>(j)];
      std::sort(row.begin(), row.end());
      for (std::size_t k = 0; k + 1 < row.size(); k += 2) {
        const int i0 = std::max(0, static_cast<int>(std::ceil((row[k] - u0) / cell - 0.5)));
        const int i1 = std::min(nu - 1, static_cast<int>(std::ceil((row[k + 1] - u0) / cell - 0.5)) - 1);
        for (int i = i0; i <= i1; ++i) {
          on[static_cast<std::size_t>(j) * static_cast<std::size_t>(nu) + static_cast<std::size_t>(i)] = 1;
          ++covered;
        }
      }
    }
    return covered;
  }
  double areaM2() const {
    return static_cast<double>(std::count(on.begin(), on.end(), std::uint8_t{1})) * cell * cell;
  }
};

// A frame at the board edge: origin on the edge under the part's centre, u along the edge, v into the board.
struct Frame {
  double ox, oy, ux, uy, vx, vy;
  P to(P p) const { return {(p.x - ox) * ux + (p.y - oy) * uy, (p.x - ox) * vx + (p.y - oy) * vy}; }
};
} // namespace

AntennaReport checkAntenna(const Snapshot& s, const Component& part, double alongMm, double depthMm,
                           const std::set<std::string>& groundNets, double testLengthMm, double testWidthMm,
                           Placement placement) {
  AntennaReport r;
  r.corner = placement == Placement::Corner;
  r.ref = part.ref;
  r.alongMm = alongMm;
  r.depthMm = depthMm;
  if (alongMm <= 0 || depthMm <= 0) {
    r.why = "the keepout size is not known for this part: enter it from its datasheet (Settings, Antenna)";
    return r;
  }
  if (s.outline.size() < 3) {
    r.why = "the board has no outline";
    return r;
  }
  // the part's centre: its pads' centroid when it has pads, else its position
  double cx = part.x, cy = part.y;
  if (!part.pads.empty()) {
    cx = cy = 0;
    for (const auto& p : part.pads) {
      cx += p.x;
      cy += p.y;
    }
    cx /= static_cast<double>(part.pads.size());
    cy /= static_cast<double>(part.pads.size());
  }
  // the nearest outline segment
  double best = 1e300;
  Frame f{};
  P bestA{}, bestB{};
  const Ring& o = s.outline;
  for (std::size_t i = 0, n = o.size(); i < n; ++i) {
    const P a = o[i], b = o[(i + 1) % n];
    const double dx = b.x - a.x, dy = b.y - a.y, len = std::hypot(dx, dy);
    if (len < 1e-9)
      continue;
    const double t = std::clamp(((cx - a.x) * dx + (cy - a.y) * dy) / (len * len), 0.0, 1.0);
    const double px = a.x + t * dx, py = a.y + t * dy, d = std::hypot(cx - px, cy - py);
    if (d < best) {
      best = d;
      bestA = a;
      bestB = b;
      const double ux = dx / len, uy = dy / len;
      // the foot of the part's centre on the segment's line (the edge may end before it: keep the line)
      const double tl = ((cx - a.x) * dx + (cy - a.y) * dy) / len;
      f = {a.x + ux * tl, a.y + uy * tl, ux, uy, -uy, ux};
      // v must point into the board
      const double step = 1e-4;
      if (!insideRing(o, {px + f.vx * step, py + f.vy * step})) {
        f.vx = -f.vx;
        f.vy = -f.vy;
      }
    }
  }
  r.edgeDistanceMm = best * 1e3;
  const double mm = 1e-3;
  double u0 = -alongMm * mm / 2, u1 = alongMm * mm / 2;
  const double v0 = 0, v1 = depthMm * mm;
  if (placement == Placement::Corner) { // anchor at the end of the nearest edge that is nearer the part
    const bool aNearer = std::hypot(cx - bestA.x, cy - bestA.y) <= std::hypot(cx - bestB.x, cy - bestB.y);
    const P corner = aNearer ? bestA : bestB, other = aNearer ? bestB : bestA;
    const double len = std::hypot(other.x - corner.x, other.y - corner.y);
    const double ux = (other.x - corner.x) / len, uy = (other.y - corner.y) / len;
    f = {corner.x, corner.y, ux, uy, -uy, ux};
    const P mid{(corner.x + other.x) / 2, (corner.y + other.y) / 2};
    if (!insideRing(o, {mid.x + f.vx * 1e-4, mid.y + f.vy * 1e-4})) {
      f.vx = -f.vx;
      f.vy = -f.vy;
    }
    u0 = 0;
    u1 = alongMm * mm;
  }

  auto toFrame = [&](const Ring& r) {
    Ring o;
    for (const P& p : r)
      o.push_back(f.to(p));
    return o;
  };
  std::map<std::string, KeepoutCopper> perLayer;
  std::map<std::string, Raster> keepRaster;
  for (const CopperShape& c : s.copper) {
    if (c.owner == part.ref || c.outer.size() < 3)
      continue;
    std::vector<Ring> rings{toFrame(c.outer)};
    for (const Ring& h : c.holes)
      rings.push_back(toFrame(h));
    std::vector<const Ring*> ptrs;
    for (const Ring& ring : rings)
      ptrs.push_back(&ring);
    auto it = keepRaster.try_emplace(c.layer, u0, v0, u1, v1, 0.01 * mm).first;
    if (it->second.paint(ptrs) == 0)
      continue;
    KeepoutCopper& k = perLayer[c.layer];
    k.layer = c.layer;
    k.nets.insert(c.net.empty() ? "(no net)" : c.net);
  }
  for (auto& [layer, k] : perLayer)
    k.areaMm2 = keepRaster.at(layer).areaM2() * 1e6;
  for (const Layer& l : s.stackup) // stackup order, copper only
    if (l.copper && perLayer.count(l.name))
      r.keepout.push_back(perLayer[l.name]);
  for (const Via& v : s.vias) {
    const P p = f.to({v.x, v.y});
    if (p.x >= u0 && p.x <= u1 && p.y >= v0 && p.y <= v1)
      ++r.viasInKeepout;
  }

  // ground: per copper layer, area and reach in the edge frame, on a grid over the board's extent
  double bu0 = 1e300, bu1 = -1e300, bv0 = 1e300, bv1 = -1e300;
  for (const P& p : s.outline) {
    const P q = f.to(p);
    bu0 = std::min(bu0, q.x);
    bu1 = std::max(bu1, q.x);
    bv0 = std::min(bv0, q.y);
    bv1 = std::max(bv1, q.y);
  }
  const double gcell = std::max(0.1 * mm, std::max(bu1 - bu0, bv1 - bv0) / 600);
  std::map<std::string, Raster> gRaster;
  for (const CopperShape& c : s.copper) {
    if (!groundNets.count(c.net) || c.outer.size() < 3)
      continue;
    std::vector<Ring> rings{toFrame(c.outer)};
    for (const Ring& h : c.holes)
      rings.push_back(toFrame(h));
    std::vector<const Ring*> ptrs;
    for (const Ring& ring : rings)
      ptrs.push_back(&ring);
    gRaster.try_emplace(c.layer, bu0, bv0, bu1, bv1, gcell).first->second.paint(ptrs);
  }
  for (const Layer& l : s.stackup)
    if (l.copper && gRaster.count(l.name)) {
      const Raster& g = gRaster.at(l.name);
      GroundLayer gl;
      gl.layer = l.name;
      gl.areaMm2 = g.areaM2() * 1e6;
      int jmax = -1, imin = g.nu, imax = -1;
      for (int j = 0; j < g.nv; ++j)
        for (int i = 0; i < g.nu; ++i)
          if (g.on[static_cast<std::size_t>(j) * static_cast<std::size_t>(g.nu) +
                   static_cast<std::size_t>(i)]) {
            jmax = std::max(jmax, j);
            imin = std::min(imin, i);
            imax = std::max(imax, i);
          }
      if (jmax >= 0) {
        gl.reachMm = (g.v0 + (jmax + 1) * g.cell) * 1e3;
        gl.widthMm = (imax - imin + 1) * g.cell * 1e3;
      }
      r.ground.push_back(gl);
    }

  // stackup: total thickness, and the dielectric between the part's layer and the next copper layer (below
  // it, or above it when it is on the bottom layer)
  for (const Layer& l : s.stackup)
    if (!l.copper)
      r.totalThicknessMm += l.thickness * 1e3;
  std::size_t at = s.stackup.size();
  for (std::size_t i = 0; i < s.stackup.size(); ++i)
    if (s.stackup[i].copper && s.stackup[i].name == part.layer)
      at = i;
  auto walk = [&](int dir) {
    double sum = 0;
    for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(at) + dir;
         i >= 0 && i < static_cast<std::ptrdiff_t>(s.stackup.size()); i += dir) {
      const Layer& l = s.stackup[static_cast<std::size_t>(i)];
      if (l.copper)
        return sum;
      sum += l.thickness * 1e3;
    }
    return 0.0;
  };
  if (at < s.stackup.size())
    r.toNextPlaneMm = walk(+1) > 0 ? walk(+1) : walk(-1);

  // findings
  char buf[512];
  for (const KeepoutCopper& k : r.keepout) {
    std::string nets;
    for (const std::string& n : k.nets)
      nets += (nets.empty() ? "" : ", ") + n;
    std::snprintf(
        buf, sizeof buf,
        "%s: %.2f mm2 of copper (nets %s) inside the %.2f x %.2f mm keepout. The vendor asks for no "
        "copper on any layer there except the part's own pads.",
        k.layer.c_str(), k.areaMm2, nets.c_str(), alongMm, depthMm);
    r.findings.push_back(buf);
  }
  if (r.viasInKeepout) {
    std::snprintf(buf, sizeof buf,
                  "%zu via(s) inside the keepout (their copper rings are counted above; the barrel crosses "
                  "every layer it spans).",
                  r.viasInKeepout);
    r.findings.push_back(buf);
  }
  if (testLengthMm > 0 && testWidthMm > 0 && !r.ground.empty()) {
    const GroundLayer* top = &r.ground[0];
    for (const GroundLayer& gl : r.ground)
      if (gl.areaMm2 > top->areaMm2)
        top = &gl;
    const double ref = testLengthMm * testWidthMm, share = top->areaMm2 / ref;
    if (share < 0.6) {
      std::snprintf(
          buf, sizeof buf,
          "Ground: the largest layer (%s) holds %.0f mm2, %.0f %% of the vendor's %.0f x %.0f mm "
          "test board (%.0f mm2), reaching %.1f mm from the edge. A chip antenna tunes and radiates "
          "with the ground it sits on, so expect a shifted resonance and lower efficiency than the "
          "datasheet shows; this is guidance, not a formula.",
          top->layer.c_str(), top->areaMm2, 100 * share, testLengthMm, testWidthMm, ref, top->reachMm);
      r.findings.push_back(buf);
    }
  }
  r.ok = true;
  return r;
}

} // namespace board
