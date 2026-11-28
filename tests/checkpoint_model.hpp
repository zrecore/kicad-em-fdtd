// The small board and short run the checkpoint scenarios share (CPU and GPU): two pads, a 50 ohm port and a
// 100 ohm load, CPML, a run of exactly 640 steps (decay off).
#pragma once
#include <cstring>
#include <vector>

#include "board/snapshot.hpp"
#include "sim/simulation.hpp"

namespace checkpoint_test {

inline board::Snapshot snapshot() {
  return board::parseSnapshot(R"({"format":"kicad-em-fdtd.snapshot","version":1,
    "board":{"name":"load"},
    "outline":[[0,0],[0.01,0],[0.01,0.01],[0,0.01]],
    "stackup":[{"name":"F.Cu","kind":"copper"},
               {"name":"core","kind":"dielectric","thickness":0.0008,"epsilon_r":4.4,"loss_tangent":0.0}],
    "copper":[{"layer":"F.Cu","owner":"","net":"A","polygon":[[0.0012,0.0012],[0.0029,0.0012],[0.0029,0.0088],[0.0012,0.0088]]},
              {"layer":"F.Cu","owner":"","net":"B","polygon":[[0.0051,0.0012],[0.0088,0.0012],[0.0088,0.0088],[0.0051,0.0088]]}],
    "vias":[],"components":[],
    "ports":[{"name":"P1","layer":"F.Cu","a":[0.003,0.004],"b":[0.005,0.004],"width":0.0022,"impedance":50},
             {"name":"load","layer":"F.Cu","a":[0.003,0.007],"b":[0.005,0.007],"width":0.0022,"impedance":100}]})");
}

inline board::Model loadModel() {
  const auto snap = snapshot();
  const board::GridPlan plan{
      .lateralCell = 1e-3, .maxDielectricCell = 0.4e-3, .airMargin = 8e-3, .airCell = 1e-3};
  return board::voxelize(snap, board::Selection{}, plan, 0.45e9);
}

inline sim::RunOptions options() {
  sim::RunOptions opt;
  opt.fMinHz = 0.3e9;
  opt.fMaxHz = 0.6e9;
  opt.points = 7;
  opt.cpmlCells = 6;
  opt.decay = 0; // never stops on decay: the run lasts exactly maxSteps
  opt.maxSteps = 640;
  return opt;
}

inline bool same(const std::vector<double>& a, const std::vector<double>& b) {
  return a.size() == b.size() &&
         (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(double)) == 0);
}

} // namespace checkpoint_test
