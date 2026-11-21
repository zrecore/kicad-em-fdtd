#include "board/netwalk.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "board/parts.hpp"

namespace board {
namespace {

double ringArea(const Ring& r) {
  double a = 0;
  for (std::size_t i = 0; i < r.size(); ++i) {
    const Point &p = r[i], &q = r[(i + 1) % r.size()];
    a += p.x * q.y - q.x * p.y;
  }
  return std::abs(a) * 0.5;
}

bool groundByName(const std::string& name) {
  std::string u;
  for (char ch : name)
    u += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  return u.find("GND") != std::string::npos || u.rfind("VSS", 0) == 0 || u == "0V" || u == "AGND";
}

} // namespace

std::set<std::string> guessGroundNets(const Snapshot& s, double planeFraction) {
  std::set<std::string> out;
  double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
  for (const Point& p : s.outline) {
    x0 = std::min(x0, p.x), y0 = std::min(y0, p.y), x1 = std::max(x1, p.x), y1 = std::max(y1, p.y);
  }
  const double board = std::max((x1 - x0) * (y1 - y0), 1e-12);
  std::map<std::pair<std::string, std::string>, double> area; // (net, layer) -> copper area
  for (const CopperShape& c : s.copper) {
    if (c.net.empty())
      continue;
    if (groundByName(c.net))
      out.insert(c.net);
    double a = ringArea(c.outer);
    for (const Ring& h : c.holes)
      a -= ringArea(h);
    area[{c.net, c.layer}] += a;
  }
  for (const auto& [key, a] : area)
    if (a > planeFraction * board)
      out.insert(key.first);
  return out;
}

NetWalk walkNets(const Snapshot& s, const std::set<std::string>& picked, const std::set<std::string>& ground,
                 const std::map<std::string, PartTreatment>& treatments) {
  NetWalk w;
  w.picked = picked;
  w.nets.insert(picked.begin(), picked.end()); // a net the user picked is signal, whatever the guess says
  std::set<std::string> stoppedNote;           // one note per part and reason
  bool changed = true;
  while (changed) {
    changed = false;
    for (const Component& c : s.components) {
      std::string via; // the net that brought us here
      for (const ComponentPad& p : c.pads)
        if (w.nets.count(p.net)) {
          via = p.net;
          break;
        }
      if (via.empty())
        continue;
      if (!w.parts.count(c.ref)) {
        w.parts.insert(c.ref);
        w.reasons[c.ref] = "pad on net " + via;
        changed = true;
      }
      if (!isTwoTerminal(c)) {
        if (stoppedNote.insert(c.ref + "/n").second)
          w.notes.push_back("net walk: " + c.ref + " touches net " + via + " and has " +
                            std::to_string(c.pads.size()) + " pad(s): included, not crossed");
        continue;
      }
      const auto t = treatments.find(c.ref);
      const PartModel pm = modelPart(c, t == treatments.end() ? PartTreatment{} : t->second);
      if (pm.kind != PartModel::Kind::Lumped) {
        if (stoppedNote.insert(c.ref + "/m").second)
          w.notes.push_back(std::string("net walk: ") + c.ref + " touches net " + via + " but is " +
                            (pm.kind == PartModel::Kind::Open
                                 ? "open (do not populate, or set open)"
                                 : "not modelled (its value is not R, L or C: give it "
                                   "values)") +
                            ": included, not crossed");
        continue;
      }
      for (const ComponentPad& p : c.pads)
        if (!p.net.empty() && !ground.count(p.net) && w.nets.insert(p.net).second) {
          w.reasons[c.ref] += ", joins net " + p.net;
          changed = true;
        }
    }
  }
  return w;
}

void applyNetSelection(Selection& sel, const NetWalk& walk, const std::set<std::string>& ground) {
  sel.all = false;
  sel.owners = walk.parts;
  sel.netFilter = true;
  sel.pickedNets = walk.picked;
  sel.nets = walk.nets;
  sel.nets.insert(ground.begin(), ground.end());
  sel.netNotes = walk.notes;
  for (const auto& [ref, why] : walk.reasons)
    sel.netNotes.push_back("net selection: " + ref + " included (" + why + ")");
  std::string nets;
  for (const std::string& n : walk.nets)
    nets += (nets.empty() ? "" : ", ") + n;
  sel.netNotes.push_back("net selection: nets " + nets + "; ground and plane nets kept as the reference");
}

void clearNetSelection(Selection& sel) {
  sel.all = true;
  sel.owners.clear();
  sel.netFilter = false;
  sel.pickedNets.clear();
  sel.nets.clear();
  sel.netNotes.clear();
}

} // namespace board
