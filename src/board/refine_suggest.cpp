#include "board/refine_suggest.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <set>
#include <tuple>

#include "board/raster.hpp"

namespace board {
namespace {

struct Found {
  double gap = 1e300;                                      // narrowest gap, metres
  double cx = 0, cy = 0;                                   // where that narrowest gap is
  double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300; // bounding box of the narrow runs
  std::vector<std::array<double, 3>> runs;                 // x, y, gap of every run, for the final box
};

} // namespace

std::vector<RefineSuggestion> suggestRefinement(const Snapshot& s, const Selection& sel, double cellsAcross,
                                                double minCellMm, double maxGapMm, double nearPortMm,
                                                std::size_t limit) {
  constexpr double mm = 1e-3;
  const double cell = 0.01 * mm;
  std::map<std::tuple<std::string, std::string, std::string>, Found> found; // layer, netA, netB (sorted)
  std::set<std::string> layers;
  for (const CopperShape& c : s.copper)
    layers.insert(c.layer);
  for (const PortDef& port : s.ports) {
    const double x0 = std::min(port.a.x, port.b.x) - nearPortMm * mm,
                 x1 = std::max(port.a.x, port.b.x) + nearPortMm * mm;
    const double y0 = std::min(port.a.y, port.b.y) - nearPortMm * mm,
                 y1 = std::max(port.a.y, port.b.y) + nearPortMm * mm;
    for (const std::string& layer : layers) {
      Raster raster(x0, y0, x1, y1, cell);
      std::map<std::string, std::uint16_t> ids;
      std::vector<std::string> names{""};
      for (const CopperShape& c : s.copper) {
        if (c.layer != layer || c.net.empty() || c.outer.size() < 3 || !sel.includes(c.owner) ||
            !sel.netIncluded(c.net))
          continue;
        const auto it = ids.try_emplace(c.net, static_cast<std::uint16_t>(names.size()));
        if (it.second)
          names.push_back(c.net);
        std::vector<const Ring*> rings{&c.outer};
        for (const Ring& h : c.holes)
          rings.push_back(&h);
        raster.paint(rings, it.first->second);
      }
      if (names.size() < 3)
        continue; // fewer than two nets in the window
      const int maxRun = static_cast<int>(std::ceil(maxGapMm * mm / cell));
      auto note = [&](std::uint16_t a, std::uint16_t b, int n, int i, int j) {
        if (a == b)
          return;
        auto pair = std::minmax(names[a], names[b]);
        Found& f = found[{layer, pair.first, pair.second}];
        const double gap = n * cell, x = x0 + (i + 0.5) * cell, y = y0 + (j + 0.5) * cell;
        f.runs.push_back({x, y, gap});
        if (gap < f.gap) {
          f.gap = gap;
          f.cx = x;
          f.cy = y;
        }
      };
      for (int j = 0; j < raster.nv; ++j) { // along x
        std::uint16_t last = 0;
        int run = 0, start = 0;
        for (int i = 0; i < raster.nu; ++i) {
          const std::uint16_t v = raster.labelAt(i, j);
          if (v == 0) {
            if (last != 0 && run == 0)
              start = i;
            if (last != 0)
              ++run;
            continue;
          }
          if (last != 0 && run > 0 && run <= maxRun)
            note(last, v, run, start + run / 2, j);
          last = v;
          run = 0;
        }
      }
      for (int i = 0; i < raster.nu; ++i) { // along y
        std::uint16_t last = 0;
        int run = 0, start = 0;
        for (int j = 0; j < raster.nv; ++j) {
          const std::uint16_t v = raster.labelAt(i, j);
          if (v == 0) {
            if (last != 0 && run == 0)
              start = j;
            if (last != 0)
              ++run;
            continue;
          }
          if (last != 0 && run > 0 && run <= maxRun)
            note(last, v, run, i, start + run / 2);
          last = v;
          run = 0;
        }
      }
    }
  }
  std::vector<std::pair<double, std::tuple<std::string, std::string, std::string>>> order;
  for (const auto& [key, f] : found)
    order.push_back({f.gap, key});
  std::sort(order.begin(), order.end());
  std::vector<RefineSuggestion> out;
  for (const auto& [gap, key] : order) {
    if (out.size() >= limit)
      break;
    const Found& f = found.at(key);
    // the box covers the narrow runs (up to 1.5 times the narrowest gap), padded by a few gaps
    double bx0 = 1e300, by0 = 1e300, bx1 = -1e300, by1 = -1e300;
    for (const auto& r : f.runs)
      if (r[2] <= 1.5 * f.gap + 0.05 * mm) {
        bx0 = std::min(bx0, r[0]);
        bx1 = std::max(bx1, r[0]);
        by0 = std::min(by0, r[1]);
        by1 = std::max(by1, r[1]);
      }
    const double pad = std::max(0.5 * mm, 4 * f.gap);
    RefineSuggestion g;
    g.layer = std::get<0>(key);
    g.netA = std::get<1>(key);
    g.netB = std::get<2>(key);
    g.gapMm = f.gap / mm;
    g.cellMm = std::max(minCellMm, g.gapMm / cellsAcross);
    g.box = {bx0 - pad, by0 - pad, bx1 + pad, by1 + pad, g.cellMm * mm};
    char buf[512];
    std::snprintf(buf, sizeof buf, "gap|%s|%s|%s|%.1f|%.1f", g.layer.c_str(), g.netA.c_str(), g.netB.c_str(),
                  std::round(f.cx / mm * 2) / 2, std::round(f.cy / mm * 2) / 2);
    g.id = buf;
    std::snprintf(
        buf, sizeof buf,
        "%.2g mm gap between %s and %s on %s near (%.1f, %.1f) mm: %.3g mm cells across it (%.0f cells)",
        g.gapMm, g.netA.c_str(), g.netB.c_str(), g.layer.c_str(), f.cx / mm, f.cy / mm, g.cellMm,
        g.gapMm / g.cellMm);
    g.reason = buf;
    out.push_back(std::move(g));
  }
  return out;
}

std::vector<RefineSuggestion> suggestCopperEnds(const Snapshot& s, const Selection& sel, double cellMm,
                                                double depthMm, double reachMm, double groundShare) {
  constexpr double mm = 1e-3;
  const double cell = 0.1 * mm;
  double ox0 = 1e300, oy0 = 1e300, ox1 = -1e300, oy1 = -1e300;
  for (const Point& p : s.outline) {
    ox0 = std::min(ox0, p.x);
    ox1 = std::max(ox1, p.x);
    oy0 = std::min(oy0, p.y);
    oy1 = std::max(oy1, p.y);
  }
  const double boardArea = std::max(0.0, ox1 - ox0) * std::max(0.0, oy1 - oy0);
  std::vector<RefineSuggestion> out;
  std::set<std::string> seen;
  for (const PortDef& port : s.ports) {
    const double reach = reachMm * mm;
    const double px = port.a.x, py = port.a.y;
    Raster raster(px - reach, py - reach, px + reach, py + reach, cell);
    for (const CopperShape& c : s.copper) {
      if (c.layer != port.layer || c.outer.size() < 3 || !sel.includes(c.owner) || !sel.netIncluded(c.net))
        continue;
      double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
      for (const Point& p : c.outer) {
        x0 = std::min(x0, p.x);
        x1 = std::max(x1, p.x);
        y0 = std::min(y0, p.y);
        y1 = std::max(y1, p.y);
      }
      if (boardArea > 0 && (x1 - x0) * (y1 - y0) >= groundShare * boardArea)
        continue; // a pour or plane: no ends
      std::vector<const Ring*> rings{&c.outer};
      for (const Ring& h : c.holes)
        rings.push_back(&h);
      raster.paint(rings);
    }
    // the copper under the port: the nearest painted cell within 1 mm, then everything touching it
    int si = -1, sj = -1;
    double best = 1e300;
    const int look = static_cast<int>(std::ceil(1.0 * mm / cell));
    const int ci = raster.nu / 2, cj = raster.nv / 2;
    for (int j = cj - look; j <= cj + look; ++j)
      for (int i = ci - look; i <= ci + look; ++i)
        if (raster.at(i, j)) {
          const double d = std::hypot(i - ci, j - cj);
          if (d < best) {
            best = d;
            si = i;
            sj = j;
          }
        }
    if (si < 0)
      continue;
    std::vector<char> in(raster.on.size(), 0);
    std::deque<std::pair<int, int>> todo{{si, sj}};
    in[raster.index(si, sj)] = 1;
    std::vector<std::pair<int, int>> cells;
    while (!todo.empty()) {
      const auto [i, j] = todo.front();
      todo.pop_front();
      cells.push_back({i, j});
      for (const auto& [di, dj] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
        const int a = i + di, b = j + dj;
        if (raster.at(a, b) && !in[raster.index(a, b)]) {
          in[raster.index(a, b)] = 1;
          todo.push_back({a, b});
        }
      }
    }
    int imin = raster.nu, imax = -1, jmin = raster.nv, jmax = -1;
    for (const auto& [i, j] : cells) {
      imin = std::min(imin, i);
      imax = std::max(imax, i);
      jmin = std::min(jmin, j);
      jmax = std::max(jmax, j);
    }
    const int depth = static_cast<int>(std::ceil(depthMm * mm / cell));
    const double pad = 0.5 * mm;
    // side: 0 left, 1 right, 2 bottom, 3 top
    for (int side = 0; side < 4; ++side) {
      const bool xSide = side < 2;
      const int ext = side == 0 ? imin : side == 1 ? imax : side == 2 ? jmin : jmax;
      const int lim = xSide ? raster.nu : raster.nv;
      if (ext <= 0 || ext >= lim - 1)
        continue; // the copper reaches the edge of the neighbourhood: it goes on, so this is no end
      int lo = 1 << 30, hi = -1; // extent across, among the copper within `depth` of the end
      for (const auto& [i, j] : cells) {
        const int along = xSide ? i : j, across = xSide ? j : i;
        if (std::abs(along - ext) <= depth) {
          lo = std::min(lo, across);
          hi = std::max(hi, across);
        }
      }
      const double edge = (xSide ? raster.u0 : raster.v0) + (side % 2 == 0 ? ext : ext + 1) * cell;
      const double a0 = (xSide ? raster.v0 : raster.u0) + lo * cell - pad;
      const double a1 = (xSide ? raster.v0 : raster.u0) + (hi + 1) * cell + pad;
      RefineSuggestion g;
      g.layer = port.layer;
      g.cellMm = cellMm;
      g.box = xSide ? RefineBox{edge - depthMm * mm, a0, edge + depthMm * mm, a1, cellMm * mm}
                    : RefineBox{a0, edge - depthMm * mm, a1, edge + depthMm * mm, cellMm * mm};
      char buf[512];
      static const char* const names[] = {"left", "right", "bottom", "top"};
      std::snprintf(buf, sizeof buf, "end|%s|%s|%.1f|%.1f", g.layer.c_str(), names[side],
                    std::round(0.5 * (g.box.x0 + g.box.x1) / mm * 2) / 2,
                    std::round(0.5 * (g.box.y0 + g.box.y1) / mm * 2) / 2);
      g.id = buf;
      if (!seen.insert(g.id).second)
        continue;
      std::snprintf(buf, sizeof buf,
                    "open copper end (%s side) on %s at %.1f mm: %.3g mm cells within %.3g mm of the edge",
                    names[side], g.layer.c_str(), edge / mm, cellMm, depthMm);
      g.reason = buf;
      out.push_back(std::move(g));
    }
  }
  return out;
}

} // namespace board
