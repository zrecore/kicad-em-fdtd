// What the simulation driver needs from a backend (CPU reference or GPU): advance steps, and the voltage
// (E * edge length, unsigned) of every lumped-port edge after each step so far.
#pragma once
#include <complex>
#include <cstdint>
#include <span>
#include <vector>

#include "core/bytes.hpp"
#include "fdtd/phasor_bank.hpp"
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
  // Running DFT of a list of field samples at N frequencies, on the backend (GPU: one kernel for every list,
  // docs/PLAN.md Next steps 11). accumulate() adds the current fields, `weight` seconds long; X_k then
  // approximates the Fourier integral. phasor() may be read between advance() calls.
  virtual std::size_t addPhasorList(std::vector<fdtd::PhasorChannel> channels,
                                    std::vector<double> frequenciesHz) = 0;
  virtual void accumulate(std::size_t list, double weight) = 0;
  virtual std::complex<double> phasor(std::size_t list, std::size_t frequency, std::size_t channel) const = 0;
  // Every phasor of a list as floats, index frequency * channels + channel; re and im each N * channels long.
  virtual void copyPhasors(std::size_t list, std::span<float> re, std::span<float> im) const = 0;
  // Checkpoint: the backend's whole run state as bytes, and back (docs/checkpoint.md). Only between advance()
  // calls, on the same machine, device and binary. A backend that holds phasor lists refuses (the lists are
  // not part of the checkpoint and a silent loss would corrupt the animation); restoreState throws if the
  // bytes do not fit this engine.
  virtual void saveState(core::ByteWriter& w) const = 0;
  virtual void restoreState(core::ByteReader& r) = 0;
};

} // namespace sim
