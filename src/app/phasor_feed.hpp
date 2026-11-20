// Feeds a viz::PhasorAccumulator from a backend's own accumulator: samples go to an engine phasor list (on
// the GPU one kernel, no field readback), and the viewer's copy is refreshed at most every `syncSeconds` and
// once at the end. Replaces per-sample CPU accumulation over the display voxels (docs/PLAN.md, Next steps
// 11a).
#pragma once
#include <chrono>
#include <mutex>
#include <vector>

#include "sim/engine.hpp"
#include "viz/phasor.hpp"

namespace app {

class PhasorFeed {
public:
  PhasorFeed(viz::PhasorAccumulator& target, std::vector<fdtd::PhasorChannel> channels,
             std::vector<double> frequenciesHz)
      : target_(target), channels_(std::move(channels)), freqs_(std::move(frequenciesHz)) {}

  // Call after every advance (it is a RunOptions::onSample). The list is created on the first call.
  // Returns true if the viewer's copy was refreshed.
  bool sample(sim::Engine& e, double weight, std::mutex& mu, double syncSeconds = 1.0) {
    if (!created_) {
      list_ = e.addPhasorList(channels_, freqs_);
      created_ = true;
      last_ = std::chrono::steady_clock::now();
    }
    e.accumulate(list_, weight);
    if (std::chrono::duration<double>(std::chrono::steady_clock::now() - last_).count() < syncSeconds)
      return false;
    sync(e, mu);
    return true;
  }
  // Call from RunOptions::onFinish so the viewer ends with the final values.
  void sync(sim::Engine& e, std::mutex& mu) {
    if (!created_)
      return;
    re_.resize(freqs_.size() * channels_.size());
    im_.resize(re_.size());
    e.copyPhasors(list_, re_, im_); // outside the lock: the solver thread only
    std::lock_guard<std::mutex> lock(mu);
    target_.load(re_, im_);
    last_ = std::chrono::steady_clock::now();
  }

private:
  viz::PhasorAccumulator& target_;
  std::vector<fdtd::PhasorChannel> channels_;
  std::vector<double> freqs_;
  std::size_t list_ = 0;
  bool created_ = false;
  std::chrono::steady_clock::time_point last_;
  std::vector<float> re_, im_;
};

} // namespace app
