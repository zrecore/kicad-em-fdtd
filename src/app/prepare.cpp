#include "app/prepare.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

#include "fdtd/grid.hpp"

namespace app {

BoardFacts boardFacts(const board::Snapshot& s) {
  BoardFacts f;
  for (const board::Layer& l : s.stackup)
    if (!l.copper) {
      f.maxEpsilonR = std::max(f.maxEpsilonR, l.epsilonR);
      f.minDielectricThickness =
          f.minDielectricThickness == 0 ? l.thickness : std::min(f.minDielectricThickness, l.thickness);
    }
  return f;
}

std::string applyCopperThickness(const Settings& settings, board::Snapshot& snapshot) {
  // Copper thickness: what the user typed, else what the board file says, else the profile's. The model keeps
  // copper as a sheet; the feed-line estimate (board::checkFeed) uses the thickness.
  std::size_t firstCu = snapshot.stackup.size(), lastCu = 0;
  for (std::size_t i = 0; i < snapshot.stackup.size(); ++i)
    if (snapshot.stackup[i].copper) {
      firstCu = std::min(firstCu, i);
      lastCu = i;
    }
  std::string copperNote;
  for (std::size_t i = 0; i < snapshot.stackup.size(); ++i) {
    board::Layer& l = snapshot.stackup[i];
    if (!l.copper)
      continue;
    const bool outerLayer = i == firstCu || i == lastCu;
    const char* key = outerLayer ? "copper_outer_um" : "copper_inner_um";
    const double typed = settings.number(key) * 1e-6;
    const bool user = settings.source(key) == Source::User;
    if (typed > 0 && (user || l.thickness == 0))
      l.thickness = typed;
    if (l.thickness > 0) {
      char buf[96];
      std::snprintf(buf, sizeof buf, "%s%s %g um%s", copperNote.empty() ? "" : ", ", l.name.c_str(),
                    l.thickness * 1e6, user && typed > 0 ? " (yours)" : "");
      copperNote += buf;
    }
  }
  if (copperNote.empty())
    return {};
  return "copper thickness " + copperNote +
         (settings.text("copper_model") == "slab"
              ? ": the model fills cells of that thickness (copper model: slab), and so does the feed-line "
                "impedance estimate"
              : ": the model uses zero-thickness sheets (copper model: sheet); the feed-line impedance "
                "estimate "
                "uses the thickness");
}

std::vector<std::string> applyPcbSettings(const Settings& settings, board::Snapshot& snapshot) {
  std::vector<std::string> notes;
  if (snapshot.hasBoardProper) {
    if (settings.text("board_outline") == "whole")
      notes.push_back("board outline: the whole Edge.Cuts as drawn, panel frame included (your setting)");
    else if (board::restrictToBoardProper(snapshot))
      notes.push_back("board outline: the board proper only; the panel frame, its copper and its parts are "
                      "left out (setting 'Board outline' = whole keeps them)");
  }
  const std::string pid = settings.text("pcb_profile");
  const FabProfile* fab = pid == "board" ? nullptr : &settings.catalog().fabProfile(pid);
  std::size_t copperLayers = 0;
  std::vector<board::Layer*> dielectrics; // top to bottom
  for (board::Layer& l : snapshot.stackup) {
    if (l.copper)
      ++copperLayers;
    else
      dielectrics.push_back(&l);
  }
  const bool fits = fab && fab->layers == copperLayers;
  if (fab && !fits)
    notes.push_back("PCB profile '" + fab->name + "' is for " + std::to_string(fab->layers) +
                    " copper layers but the board has " + std::to_string(copperLayers) +
                    ": its values are not applied (values you typed are)");
  if (fits)
    notes.push_back("PCB stackup from '" + fab->name + "' (" + fab->source + ")");
  for (std::size_t i = 0; i < kMaxDielectrics; ++i) {
    const std::string n = std::to_string(i + 1);
    const std::array<const char*, 3> keys{"_thickness_mm", "_epsilon_r", "_loss_tangent"};
    const bool present = i < dielectrics.size();
    std::string changes;
    for (std::size_t k = 0; k < keys.size(); ++k) {
      const std::string key = "diel" + n + keys[k];
      const double v = settings.number(key);
      if (v == 0)
        continue;
      const bool user = settings.source(key) == Source::User;
      if (!user && !fits)
        continue;
      if (!present) {
        if (user)
          notes.push_back("dielectric " + n + " does not exist on this board: " + key + " ignored");
        continue;
      }
      board::Layer& l = *dielectrics[i];
      double& target = k == 0 ? l.thickness : k == 1 ? l.epsilonR : l.lossTangent;
      const double value = k == 0 ? v * 1e-3 : v;
      if (target == value)
        continue;
      char buf[96];
      std::snprintf(buf, sizeof buf, "%s%s %g%s (%s", changes.empty() ? "" : ", ",
                    k == 0   ? "thickness"
                    : k == 1 ? "er"
                             : "tan d",
                    v, k == 0 ? " mm" : "", user ? "your value" : "profile");
      changes += buf;
      if (!user && fab && i < fab->dielectrics.size()) {
        const auto& a = fab->dielectrics[i].assumed;
        const char* name = k == 0 ? "thickness_mm" : k == 1 ? "epsilon_r" : "loss_tangent";
        if (std::find(a.begin(), a.end(), name) != a.end())
          changes += ", not published by the fab: assumed";
      }
      changes += ")";
      target = value;
    }
    if (!changes.empty())
      notes.push_back("dielectric " + n + " (" + dielectrics[i]->name + "): " + changes);
  }
  if (const std::string n = applyCopperThickness(settings, snapshot); !n.empty())
    notes.push_back(n);
  return notes;
}

Prepared prepare(const board::Snapshot& original, const Settings& settings, const board::Selection& selection,
                 const std::vector<board::RefineBox>& extraRefine) {
  board::Snapshot snapshot = original; // the PCB settings edit the stackup of this copy only
  const std::vector<std::string> pcbNotes = applyPcbSettings(settings, snapshot);
  const BoardFacts facts = boardFacts(snapshot);
  Prepared p{makePlan(settings, facts.maxEpsilonR, facts.minDielectricThickness),
             {fdtd::Grid::uniform({1, 1, 1}, {1, 1, 1})},
             {}};
  const RunPlan& plan = p.plan;

  std::vector<board::RefineBox> refine =
      board::refineAroundPorts(snapshot, plan.fineCell, 2 * plan.coarseCell);
  refine.insert(refine.end(), extraRefine.begin(), extraRefine.end());
  refine.insert(refine.end(), selection.refine.begin(),
                selection.refine.end()); // the user's switched-on boxes
  // The absorbing layer sits outside the free space, so the margin is not eaten by it.
  const board::GridPlan grid{.lateralCell = plan.coarseCell,
                             .maxDielectricCell = plan.dielectricCell,
                             .airMargin =
                                 plan.airMargin + static_cast<double>(plan.pmlCells) * plan.coarseCell,
                             .airCell = plan.coarseCell,
                             .refine = refine,
                             .snapToGeometry = true,
                             .airNearCell = plan.airNearCell,
                             .airNearHeight = plan.airNearHeight,
                             .airGrowth = plan.airGrowth,
                             .copperSlab = plan.copperSlab,
                             .lateralGrowth = plan.lateralGrowth};
  board::Selection withBodies = selection;
  if (withBodies.defaultBody.kind == board::BodyKind::Ignore) { // the settings give the default treatment
    const std::string mode = settings.text("component_bodies");
    withBodies.defaultBody.kind = mode == "dielectric" ? board::BodyKind::Dielectric
                                  : mode == "metal"    ? board::BodyKind::Metal
                                                       : board::BodyKind::Ignore;
    withBodies.defaultBody.epsilonR = settings.number("component_body_eps");
    withBodies.defaultBodyHeight = settings.number("component_body_height_mm") * 1e-3;
  }
  p.model = board::voxelize(snapshot, withBodies, grid, 0.5 * (plan.bandHz[0] + plan.bandHz[1]));
  p.model.notes.insert(p.model.notes.end(), pcbNotes.begin(), pcbNotes.end());

  p.options.fMinHz = plan.sweepHz[0];
  p.options.fMaxHz = plan.sweepHz[1];
  p.options.excitationMinHz = plan.excitationHz[0];
  p.options.excitationMaxHz = plan.excitationHz[1];
  p.options.points = plan.sweepPoints;
  p.options.cpmlCells = plan.pmlCells;
  p.options.courant = plan.courant;
  p.options.maxSteps = plan.maxSteps;
  p.options.sampleEvery = plan.sampleEvery;
  p.options.decay = plan.decay;
  return p;
}

Estimate estimate(const Prepared& p, bool gpu, double deviceBytes) {
  Estimate e;
  const fdtd::Grid& g = p.model.grid;
  e.cells = g.cellCount();
  e.timeStep = p.options.courant * fdtd::maxStableDt(g);
  for (fdtd::Axis a : {fdtd::X, fdtd::Y, fdtd::Z})
    e.smallestCell = e.smallestCell == 0 ? g.minCellSize(a) : std::min(e.smallestCell, g.minCellSize(a));
  const double nodes = static_cast<double>(g.nodeCount());
  // 6 float fields + one packed material word per node, plus CPML auxiliary slabs (about 8 N faces).
  double faces = 0;
  const double n = static_cast<double>(p.plan.pmlCells);
  faces += static_cast<double>(g.nodes(fdtd::X) * g.nodes(fdtd::Y) + g.nodes(fdtd::Y) * g.nodes(fdtd::Z) +
                               g.nodes(fdtd::X) * g.nodes(fdtd::Z));
  e.memoryBytes = nodes * 28.0 + 8.0 * n * faces * 4.0;
  e.exceedsMemory = deviceBytes > 0 && e.memoryBytes > 0.8 * deviceBytes;
  const double tau = 1.1 / (p.plan.excitationHz[1] - p.plan.excitationHz[0]);
  const double pulseSteps = 8.0 * tau / e.timeStep;
  e.steps = static_cast<std::size_t>(std::min(3.0 * pulseSteps, static_cast<double>(p.plan.maxSteps)));
  const double cellsPerSecond = gpu ? 7.0e8 : 9.0e7; // measured on the M1 Pro, CPML and one port included
  e.seconds = static_cast<double>(e.cells) * static_cast<double>(e.steps) / cellsPerSecond;
  return e;
}

} // namespace app
