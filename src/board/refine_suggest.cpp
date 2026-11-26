#include "board/refine_suggest.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
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

} // namespace board
