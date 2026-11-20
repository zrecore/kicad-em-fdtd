// Running DFT of many field samples at N frequencies: the CPU twin of the GPU accumulator
// (shaders/phasor_accum.comp). A channel is one field component at one node; E is taken at time n dt and H
// half a step earlier, the leapfrog offsets of the scheme (solver.hpp). Accumulates in double, so it is also
// the reference the GPU result is judged against.
#pragma once
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>
#include <span>
#include <stdexcept>
#include <vector>

#include "fdtd/solver.hpp"

namespace fdtd {

struct PhasorChannel {
  Kind kind;
  Axis component;
  std::uint32_t node; // grid.hpp node index
};

class PhasorBank {
public:
  std::size_t add(std::vector<PhasorChannel> channels, std::vector<double> frequenciesHz) {
    if (channels.empty() || frequenciesHz.empty())
      throw std::invalid_argument("a phasor list needs channels and frequencies");
    List l;
    l.x.assign(channels.size() * frequenciesHz.size(), {});
    l.channels = std::move(channels);
    l.f = std::move(frequenciesHz);
    lists_.push_back(std::move(l));
    return lists_.size() - 1;
  }
  // value(kind, component, node) gives the field; tE is the time of E, dtHalf the H offset (dt / 2).
  template <class FieldAt>
  void accumulate(std::size_t list, double weight, double tE, double dtHalf, FieldAt&& value) {
    List& l = lists_.at(list);
    std::vector<std::complex<double>> ph[2]; // exp(-j w t) per frequency, for E and for H
    for (int kind = 0; kind < 2; ++kind)
      for (double f : l.f)
        ph[kind].push_back(std::polar(weight, -2 * std::numbers::pi * f * (kind == 0 ? tE : tE - dtHalf)));
    for (std::size_t c = 0; c < l.channels.size(); ++c) {
      const PhasorChannel& ch = l.channels[c];
      const double v = value(ch.kind, ch.component, ch.node);
      const auto& p = ph[ch.kind == Kind::E ? 0 : 1];
      for (std::size_t k = 0; k < l.f.size(); ++k)
        l.x[k * l.channels.size() + c] += v * p[k];
    }
  }
  // All phasors as floats, index k * channels + c (the layout of viz::PhasorAccumulator).
  void copy(std::size_t list, std::span<float> re, std::span<float> im) const {
    const List& l = lists_.at(list);
    if (re.size() != l.x.size() || im.size() != l.x.size())
      throw std::invalid_argument("phasor copy: wrong size");
    for (std::size_t i = 0; i < l.x.size(); ++i) {
      re[i] = static_cast<float>(l.x[i].real());
      im[i] = static_cast<float>(l.x[i].imag());
    }
  }
  std::complex<double> phasor(std::size_t list, std::size_t k, std::size_t c) const {
    const List& l = lists_.at(list);
    if (k >= l.f.size() || c >= l.channels.size())
      throw std::out_of_range("phasor index");
    return l.x[k * l.channels.size() + c];
  }
  std::size_t channels(std::size_t list) const { return lists_.at(list).channels.size(); }

private:
  struct List {
    std::vector<PhasorChannel> channels;
    std::vector<double> f;
    std::vector<std::complex<double>> x; // [k * channels + c]
  };
  std::vector<List> lists_;
};

} // namespace fdtd
