// What the simulation driver needs from a backend (CPU reference or GPU): advance steps, and the voltage
// (E * edge length, unsigned) of every lumped-port edge after each step so far.
#pragma once
#include <cstdint>
#include <span>
#include <vector>

#include "fdtd/solver.hpp"

namespace sim {

class Engine {
public:
  virtual ~Engine() = default;
  virtual void advance(std::uint32_t steps) = 0;
  virtual const std::vector<double>& edgeVoltage(std::size_t edge) const = 0;
  // Node-sized field array (grid.hpp layout), valid between advance() calls; time of E after the last step.
  virtual std::span<const float> field(fdtd::Kind kind, fdtd::Axis component) const = 0;
  virtual double time() const = 0;
};

} // namespace sim
