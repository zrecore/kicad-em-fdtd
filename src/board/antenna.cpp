#include "board/antenna.hpp"

#include "board/raster.hpp"

#include "board/line_impedance.hpp"

#include <algorithm>
#include <cctype>
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
// Natural order: runs of digits compare as numbers, everything else as lower-case text.
bool naturalLess(const std::string& a, const std::string& b) {
  std::size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    if (std::isdigit(static_cast<unsigned char>(a[i])) && std::isdigit(static_cast<unsigned char>(b[j]))) {
      std::size_t ie = i, je = j;
      while (ie < a.size() && std::isdigit(static_cast<unsigned char>(a[ie])))
        ++ie;
      while (je < b.size() && std::isdigit(static_cast<unsigned char>(b[je])))
        ++je;
      const std::string na = a.substr(i, ie - i), nb = b.substr(j, je - j);
      const std::size_t za = na.find_first_not_of('0'), zb = nb.find_first_not_of('0');
      const std::string ta = za == std::string::npos ? "" : na.substr(za),
                        tb = zb == std::string::npos ? "" : nb.substr(zb);
      if (ta.size() != tb.size())
        return ta.size() < tb.size();
      if (ta != tb)
        return ta < tb;
      i = ie;
      j = je;
    } else {
      const char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i]))),
                 cb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[j])));
      if (ca != cb)
        return ca < cb;
      ++i;
      ++j;
    }
  }
  return a.size() - i < b.size() - j;
}
} // namespace

bool looksLikeAntenna(const Component& c) {
  std::size_t n = 0; // the reference's leading letters: ANT1, ANT_A, AE1
  while (n < c.ref.size() && std::isalpha(static_cast<unsigned char>(c.ref[n])))
    ++n;
  const std::string prefix = lower(c.ref.substr(0, n));
  if (prefix == "ant" || prefix == "ae")
    return true;
  const std::string value = lower(c.value);
  if (value.find("antenna") != std::string::npos)
    return true;
  std::string token; // a whole word of the value: IFA, PIFA, MIFA
  for (std::size_t k = 0; k <= value.size(); ++k) {
    if (k < value.size() && std::isalnum(static_cast<unsigned char>(value[k]))) {
      token += value[k];
      continue;
    }
    if (token == "ifa" || token == "pifa" || token == "mifa")
      return true;
    token.clear();
  }
  return false;
}

std::vector<const Component*> antennaPickOrder(const std::vector<Component>& parts) {
  std::vector<const Component*> out;
  for (const Component& c : parts)
    if (looksLikeAntenna(c))
      out.push_back(&c);
  const std::size_t antennas = out.size();
  std::vector<const Component*> rest;
  for (const Component& c : parts)
    if (!looksLikeAntenna(c))
      rest.push_back(&c);
  std::stable_sort(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(antennas),
                   [](const Component* a, const Component* b) { return naturalLess(a->ref, b->ref); });
  std::stable_sort(rest.begin(), rest.end(),
                   [](const Component* a, const Component* b) { return naturalLess(a->ref, b->ref); });
  out.insert(out.end(), rest.begin(), rest.end());
  return out;
}

const Component* defaultAntennaPart(const std::vector<Component>& parts,
                                    const std::vector<AntennaRule>& rules) {
  for (const Component& c : parts)
    if (ruleFor(rules, c))
      return &c;
  const Component* only = nullptr;
  for (const Component& c : parts)
    if (looksLikeAntenna(c)) {
      if (only)
        return nullptr; // two candidates: the user decides
      only = &c;
    }
  return only;
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

namespace board {

namespace {

// One measurement of the feed line: distance from the part's pad, local width and side gap (-1: no ground
// beside it), all in metres.
struct FeedSample {
  double d, width, gap;
};

// Fills r.segments and r.turnsAtMm from the samples. A sample whose "width" is over 1.5 times the line's is
// not on a straight stretch (the copper runs far both ways: a corner, a T or the start of a stub) and is
// counted as a turn instead; the rest are cut into segments wherever the width (median of 5 along the line)
// moves more than 12 % from the segment's start.
void analyseUniformity(FeedReport& r, std::vector<FeedSample> samples) {
  constexpr double mm = 1e-3;
  std::sort(samples.begin(), samples.end(),
            [](const FeedSample& a, const FeedSample& b) { return a.d < b.d; });
  std::vector<FeedSample> straight;
  std::vector<double> turnD; // every sample that is on a turn, for the exclusion zone below
  for (const FeedSample& s : samples)
    if (s.width > 1.5 * r.widthMm * mm) {
      turnD.push_back(s.d);
      if (r.turnsAtMm.empty() || s.d / mm - r.turnsAtMm.back() > 1.0)
        r.turnsAtMm.push_back(s.d / mm);
    }
  for (const FeedSample& s : samples) { // a corner's own samples and the 0.5 mm around it are not a stretch
    const bool nearTurn =
        std::any_of(turnD.begin(), turnD.end(), [&](double d) { return std::fabs(d - s.d) < 0.5 * mm; });
    if (s.width <= 1.5 * r.widthMm * mm && !nearTurn)
      straight.push_back(s);
  }
  std::vector<double> filtered(straight.size());
  for (std::size_t i = 0; i < straight.size(); ++i) {
    std::vector<double> w;
    for (std::size_t k = i >= 2 ? i - 2 : 0; k <= std::min(i + 2, straight.size() - 1); ++k)
      w.push_back(straight[k].width);
    std::sort(w.begin(), w.end());
    filtered[i] = w[w.size() / 2];
  }
  std::size_t start = 0;
  auto close = [&](std::size_t from, std::size_t to) { // samples [from, to)
    FeedSegment seg;
    std::vector<double> ws, gs;
    seg.fromMm = straight[from].d / mm;
    seg.toMm = straight[to - 1].d / mm;
    for (std::size_t i = from; i < to; ++i) {
      ws.push_back(straight[i].width);
      if (straight[i].gap >= 0)
        gs.push_back(straight[i].gap);
    }
    std::sort(ws.begin(), ws.end());
    std::sort(gs.begin(), gs.end());
    seg.widthMm = ws[ws.size() / 2] / mm;
    seg.gapMm = gs.size() * 2 >= ws.size() ? gs[gs.size() / 2] / mm : 0;
    if (seg.toMm - seg.fromMm >= 0.3) // shorter stretches are transitions (a ramp, a pad), not lines
      r.segments.push_back(seg);
  };
  for (std::size_t i = 1; i < straight.size(); ++i)
    if (std::fabs(filtered[i] - filtered[start]) > 0.12 * filtered[start]) {
      close(start, i);
      start = i;
    }
  if (!straight.empty())
    close(start, straight.size());
}

// The sentences about uniformity, most important first.
std::vector<std::string> uniformityFindings(const FeedReport& r) {
  std::vector<std::string> out;
  char buf[400];
  const bool uniform = r.segments.size() <= 1 && r.turnsAtMm.empty() && r.otherParts.empty();
  if (uniform) {
    std::snprintf(
        buf, sizeof buf,
        "The line is uniform over the stretch measured (1 to 6 mm from the part's pad): one width, no other "
        "part's pad and "
        "no turn or branch there. The line beyond 6 mm was not read.");
    out.push_back(buf);
    return out;
  }
  if (r.segments.size() > 1) {
    std::string t = "The width changes along the line: ";
    for (std::size_t i = 0; i < r.segments.size(); ++i) {
      std::snprintf(buf, sizeof buf, "%s%.2f mm from %.1f to %.1f mm (about %.0f ohm)", i ? ", then " : "",
                    r.segments[i].widthMm, r.segments[i].fromMm, r.segments[i].toMm, r.segments[i].z0Ohm);
      t += buf;
    }
    t += " from the pad (closed form; short ramps are not listed), so the impedance changes along the line; "
         "the "
         "figure above is for the width most of the stretch has, which is not necessarily the end at the "
         "antenna.";
    out.push_back(t);
  }
  if (!r.otherParts.empty()) {
    std::string t = "Pads of other parts touch this net within the stretch:";
    for (const std::string& ref : r.otherParts)
      t += " " + ref;
    t += ". Their copper is not counted as line, and the parts themselves are not in the estimate.";
    out.push_back(t);
  }
  if (!r.turnsAtMm.empty()) {
    std::string t = "The copper turns or branches about";
    for (std::size_t i = 0; i < r.turnsAtMm.size(); ++i) {
      std::snprintf(buf, sizeof buf, "%s %.1f mm", i ? "," : "", r.turnsAtMm[i]);
      t += buf;
    }
    t += " from the pad (a corner, a T or a stub). The figures above leave it out. A corner or a step much "
         "shorter than a twentieth of a wavelength changes the match only slightly (a 0.4 mm neck at 91 ohm "
         "on a "
         "50 ohm line: about 2 ohm of reactance at 2.4 GHz); a stub of a twentieth of a wavelength or more "
         "does "
         "matter (a 6 mm open stub: -16 dB), and one a quarter wavelength long shorts the line "
         "(scripts/antenna/"
         "cascade.py sizes these).";
    out.push_back(t);
  }
  return out;
}

} // namespace

FeedReport checkFeed(const Snapshot& s, const Component& part, const std::set<std::string>& groundNets,
                     double portOhm) {
  FeedReport r;
  // The signal pad: the part's first pad on a net that is neither empty nor ground.
  const ComponentPad* pad = nullptr;
  for (const ComponentPad& p : part.pads)
    if (!p.net.empty() && !groundNets.count(p.net)) {
      pad = &p;
      break;
    }
  if (!pad) {
    r.why = "the part has no signal pad on a net that is not ground, so there is no feed line to read";
    return r;
  }
  r.net = pad->net;
  r.layer = part.layer;
  constexpr double mm = 1e-3;
  const double half = 8 * mm, cell = 0.02 * mm;
  const double x0 = pad->x - half, y0 = pad->y - half;
  auto paint = [&](Raster& raster, const std::string& layer, auto&& wanted) {
    for (const CopperShape& c : s.copper) {
      if (c.layer != layer || c.outer.size() < 3 || !wanted(c))
        continue;
      std::vector<const Ring*> rings{&c.outer};
      for (const Ring& h : c.holes)
        rings.push_back(&h);
      raster.paint(rings);
    }
  };
  Raster feed(x0, y0, x0 + 2 * half, y0 + 2 * half, cell), other(x0, y0, x0 + 2 * half, y0 + 2 * half, cell);
  paint(feed, part.layer,
        [&](const CopperShape& c) { return c.net == pad->net && c.owner != part.ref && c.owner.empty(); });
  // Other parts' pads on the feed net (a series part of the match, a decoupling cap on the line) join the
  // flood fill, so the line beyond them is found, but are labelled so their copper does not count as line.
  std::vector<std::string> padOwners; // label 2 + k belongs to padOwners[k]
  for (const CopperShape& c : s.copper) {
    if (c.layer != part.layer || c.net != pad->net || c.owner.empty() || c.owner == part.ref ||
        c.outer.size() < 3)
      continue;
    auto it = std::find(padOwners.begin(), padOwners.end(), c.owner);
    if (it == padOwners.end()) {
      padOwners.push_back(c.owner);
      it = padOwners.end() - 1;
    }
    std::vector<const Ring*> rings{&c.outer};
    for (const Ring& h : c.holes)
      rings.push_back(&h);
    feed.paint(rings, static_cast<std::uint16_t>(2 + (it - padOwners.begin())));
  }
  paint(other, part.layer, [&](const CopperShape& c) { return c.net != pad->net && c.owner != part.ref; });
  auto at = [](const Raster& rs, int i, int j) {
    return i >= 0 && j >= 0 && i < rs.nu && j < rs.nv &&
           rs.on[static_cast<std::size_t>(j) * static_cast<std::size_t>(rs.nu) + static_cast<std::size_t>(i)];
  };
  // Start at the feed copper nearest the pad (within 1.5 mm) and flood-fill what is connected to it.
  const int pi = static_cast<int>((pad->x - x0) / cell), pj = static_cast<int>((pad->y - y0) / cell);
  const int reach = static_cast<int>(1.5 * mm / cell);
  int si = -1, sj = -1;
  double bestD = 1e300;
  for (int j = std::max(0, pj - reach); j <= std::min(feed.nv - 1, pj + reach); ++j)
    for (int i = std::max(0, pi - reach); i <= std::min(feed.nu - 1, pi + reach); ++i)
      if (at(feed, i, j)) {
        const double d = std::hypot(i - pi, j - pj);
        if (d < bestD) {
          bestD = d;
          si = i;
          sj = j;
        }
      }
  if (si < 0) {
    r.why = "no copper of net " + pad->net + " on " + part.layer + " touches the part's signal pad";
    return r;
  }
  std::vector<std::uint8_t> seen(feed.on.size(), 0);
  std::vector<std::pair<int, int>> stack{{si, sj}}, connected;
  seen[static_cast<std::size_t>(sj) * static_cast<std::size_t>(feed.nu) + static_cast<std::size_t>(si)] = 1;
  while (!stack.empty()) {
    const auto [i, j] = stack.back();
    stack.pop_back();
    connected.push_back({i, j});
    for (const auto& [di, dj] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
      const int a = i + di, b = j + dj;
      const std::size_t k =
          static_cast<std::size_t>(b) * static_cast<std::size_t>(feed.nu) + static_cast<std::size_t>(a);
      if (at(feed, a, b) && !seen[k]) {
        seen[k] = 1;
        stack.push_back({a, b});
      }
    }
  }
  auto connectedAt = [&](int i, int j) {
    return i >= 0 && j >= 0 && i < feed.nu && j < feed.nv &&
           seen[static_cast<std::size_t>(j) * static_cast<std::size_t>(feed.nu) +
                static_cast<std::size_t>(i)];
  };
  auto lineAt = [&](int i, int j) { return connectedAt(i, j) && feed.labelAt(i, j) == 1; };
  // Sample the line between 1 and 6 mm from the pad: local width is the shorter of the horizontal and
  // vertical runs of connected copper through the cell; the gap is the distance, across the line, from its
  // edge to the first copper of another net.
  std::vector<double> widths, gaps, oneSideGaps;
  std::vector<FeedSample> detail;
  std::vector<std::pair<int, int>> samples;
  const double maxGap = 3 * mm;
  std::size_t counter = 0;
  for (const auto& [i, j] : connected) {
    const double d = std::hypot((i - pi) * cell, (j - pj) * cell);
    if (d < 1 * mm || d > 6 * mm || (++counter % 7) != 0 || !lineAt(i, j))
      continue;
    int run[4] = {0, 0, 0, 0}; // +x, -x, +y, -y
    const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (int k = 0; k < 4; ++k)
      while (lineAt(i + dirs[k][0] * (run[k] + 1), j + dirs[k][1] * (run[k] + 1)))
        ++run[k];
    const double wx = (run[0] + run[1] + 1) * cell, wy = (run[2] + run[3] + 1) * cell;
    const bool alongY = wx < wy; // the line runs along y when it is narrower across x
    const int a = alongY ? 0 : 2, b = alongY ? 1 : 3;
    widths.push_back(std::min(wx, wy));
    samples.push_back({i, j});
    double side[2] = {-1, -1};
    for (int k : {a, b}) {
      int n = run[k] + 1;
      const int limit = static_cast<int>(maxGap / cell);
      for (int steps = 0; steps < limit; ++steps, ++n)
        if (at(other, i + dirs[k][0] * n, j + dirs[k][1] * n)) {
          side[k == a ? 0 : 1] = (n - run[k] - 1) * cell;
          break;
        }
    }
    if (side[0] >= 0 && side[1] >= 0)
      gaps.push_back(0.5 * (side[0] + side[1]));
    else if (side[0] >= 0 || side[1] >= 0)
      oneSideGaps.push_back(std::max(side[0], side[1]));
    detail.push_back({d, std::min(wx, wy), side[0] >= 0 && side[1] >= 0 ? 0.5 * (side[0] + side[1]) : -1.0});
  }
  if (widths.empty()) {
    r.why = "the copper leaving the signal pad is shorter than 1 mm, so no line was measured";
    return r;
  }
  auto median = [](std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
  };
  r.widthMm = median(widths) / mm;
  if (gaps.size() * 2 >= widths.size()) { // ground copper close beside the line along most of its length
    r.coplanar = true;
    r.gapMm = median(gaps) / mm;
  } else if (oneSideGaps.size() * 2 >= widths.size()) { // ground along one side only
    r.oneSided = true;
    r.gapMm = median(oneSideGaps) / mm;
  }
  for (std::size_t k = 0; k < feed.on.size(); ++k) // other parts whose pads the connected copper reaches
    if (seen[k] && feed.on[k] >= 2) {
      const std::string& ref = padOwners[static_cast<std::size_t>(feed.on[k] - 2)];
      if (std::find(r.otherParts.begin(), r.otherParts.end(), ref) == r.otherParts.end())
        r.otherParts.push_back(ref);
    }
  std::sort(r.otherParts.begin(), r.otherParts.end());
  analyseUniformity(r, detail);
  const auto [mi, mj] = samples[samples.size() / 2];
  r.atXMm = (x0 + (mi + 0.5) * cell) * 1e3;
  r.atYMm = (y0 + (mj + 0.5) * cell) * 1e3;

  // The reference plane: the nearest copper layer below the line (above it on the bottom layer) whose ground
  // copper lies under most of the sampled points.
  std::size_t layerIdx = s.stackup.size();
  for (std::size_t i = 0; i < s.stackup.size(); ++i)
    if (s.stackup[i].copper && s.stackup[i].name == part.layer)
      layerIdx = i;
  if (layerIdx == s.stackup.size()) {
    r.why = "the part's layer is not in the stackup";
    return r;
  }
  for (int dir : {+1, -1}) {
    double thickness = 0, capSum = 0; // capSum: sum of t / epsilon over the dielectrics crossed
    for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(layerIdx) + dir;
         i >= 0 && i < static_cast<std::ptrdiff_t>(s.stackup.size()); i += dir) {
      const Layer& l = s.stackup[static_cast<std::size_t>(i)];
      if (!l.copper) {
        thickness += l.thickness;
        capSum += l.thickness / std::max(l.epsilonR, 1.0);
        continue;
      }
      Raster plane(x0, y0, x0 + 2 * half, y0 + 2 * half, cell);
      paint(plane, l.name, [&](const CopperShape& c) { return groundNets.count(c.net) > 0; });
      std::size_t under = 0;
      for (const auto& [si2, sj2] : samples)
        under += at(plane, si2, sj2) ? 1 : 0;
      if (under * 10 >= samples.size() * 8 &&
          thickness > 0) { // ground under at least 80 percent of the samples
        r.referenceLayer = l.name;
        r.heightMm = thickness / mm;
        r.epsR = thickness / capSum;
        break;
      }
    }
    if (!r.referenceLayer.empty())
      break;
  }
  if (r.referenceLayer.empty()) {
    r.why =
        "no ground plane was found under the feed line on any layer, so its impedance cannot be estimated";
    return r;
  }
  r.copperMm = s.stackup[layerIdx].thickness / mm;
  const LineImpedance z =
      r.coplanar ? groundedCoplanarImpedance(r.widthMm * mm, r.gapMm * mm, r.heightMm * mm, r.epsR)
                 : microstripImpedance(r.widthMm * mm, r.heightMm * mm, r.epsR);
  r.z0Ohm = z.z0Ohm;
  r.epsEff = z.epsEff;
  char buf[640];
  const double off = 100.0 * (r.z0Ohm - portOhm) / portOhm;
  std::snprintf(
      buf, sizeof buf,
      "Feed line (net %s on %s, measured near (%.1f, %.1f) mm): %.2f mm wide over %.2f mm of dielectric "
      "(er about %.1f) above ground on %s%s%.2f mm: %s, about %.0f ohm (effective permittivity %.2f), "
      "%+.0f %% against the %.0f ohm port. %s",
      r.net.c_str(), r.layer.c_str(), r.atXMm, r.atYMm, r.widthMm, r.heightMm, r.epsR,
      r.referenceLayer.c_str(), r.coplanar ? ", with ground beside it at a gap of " : "",
      r.coplanar ? r.gapMm : 0.0, r.coplanar ? "grounded coplanar waveguide" : "microstrip", r.z0Ohm,
      r.epsEff, off, portOhm,
      std::fabs(off) <= 10 ? "That is within 10 %."
                           : "That is more than 10 % from the port: expect a reflection at the feed, and "
                             "consider a different line width.");
  r.findings.push_back(buf);
  if (r.oneSided) {
    std::snprintf(
        buf, sizeof buf,
        "Ground copper runs along one side only, at a gap of %.2f mm. The closed form above leaves it "
        "out (it reads this as microstrip); the field solve below includes it.",
        r.gapMm);
    r.findings.push_back(buf);
  }
  for (FeedSegment& seg : r.segments)
    seg.z0Ohm =
        (seg.gapMm > 0 ? groundedCoplanarImpedance(seg.widthMm * mm, seg.gapMm * mm, r.heightMm * mm, r.epsR)
                       : microstripImpedance(seg.widthMm * mm, r.heightMm * mm, r.epsR))
            .z0Ohm;
  for (const std::string& f : uniformityFindings(r))
    r.findings.push_back(f);
  r.findings.push_back("This is a closed form (zero-thickness copper, quasi-static), read from the board as "
                       "drawn. Microstrip was "
                       "within about 2 % of this project's FDTD solver on 50 and 87 ohm lines. A coplanar "
                       "line with real copper "
                       "(35 um) measures 6 to 9 % lower in the 2D field solve than this form says, because "
                       "the strip's sides add "
                       "capacitance to the ground beside it (a vendor's 50 ohm coplanar line reads 54.6 ohm "
                       "here and 50.1 ohm with "
                       "35 um of copper). Bends, stubs, vias and nearby parts are not included.");
  r.ok = true;
  return r;
}

} // namespace board
