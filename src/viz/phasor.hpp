// Multi-frequency phasor accumulation for the animated view (CPU reference of the GPU accumulator).
// During the solve every display voxel accumulates X_k = sum_n f(t_n) exp(-j w_k t_n) for N frequencies; a
// frame is then reconstructed on demand: steady-state animation of one frequency, Re{X_k exp(j phi)}, or a
// band-limited movie of the pulse, sum_k X_k exp(j w_k tau) (see docs/PLAN.md, Viewer design).
#pragma once
#include <cmath>
#include <complex>
#include <numbers>
#include <span>
#include <vector>

namespace viz {

class PhasorAccumulator {
public:
  PhasorAccumulator(std::vector<double> frequenciesHz, std::size_t voxels)
      : f_(std::move(frequenciesHz)), voxels_(voxels), re_(f_.size() * voxels, 0.f),
        im_(f_.size() * voxels, 0.f) {}

  // field[v] is the sampled field at time t (voxel-major); weight dt makes X_k a Fourier integral.
  void add(double t, double dt, std::span<const float> field) {
    for (std::size_t k = 0; k < f_.size(); ++k) {
      const double w = 2 * std::numbers::pi * f_[k] * t;
      const float c = static_cast<float>(std::cos(w) * dt), s = static_cast<float>(std::sin(w) * dt);
      float* re = &re_[k * voxels_];
      float* im = &im_[k * voxels_];
      for (std::size_t v = 0; v < voxels_; ++v) {
        re[v] += field[v] * c;
        im[v] -= field[v] * s;
      }
    }
  }

  // Replaces the accumulated values with phasors computed elsewhere (layout k * voxels + v, as stored here),
  // for example by the backend's accumulator (docs/PLAN.md, Next steps 11).
  void load(std::span<const float> re, std::span<const float> im) {
    re_.assign(re.begin(), re.end());
    im_.assign(im.begin(), im.end());
  }
  // One frequency's real and imaginary parts over the voxels (for the GPU renderer's upload).
  std::span<const float> re(std::size_t k) const { return {&re_[k * voxels_], voxels_}; }
  std::span<const float> im(std::size_t k) const { return {&im_[k * voxels_], voxels_}; }
  std::complex<float> phasor(std::size_t k, std::size_t voxel) const {
    return {re_[k * voxels_ + voxel], im_[k * voxels_ + voxel]};
  }
  std::size_t frequencies() const { return f_.size(); }
  std::size_t voxels() const { return voxels_; }
  double frequencyHz(std::size_t k) const { return f_[k]; }

  // Steady-state animation of frequency k at animation phase phi (radians): Re{X_k exp(j phi)}.
  float steady(std::size_t k, std::size_t voxel, double phi) const {
    const auto x = phasor(k, voxel);
    return static_cast<float>(x.real() * std::cos(phi) - x.imag() * std::sin(phi));
  }

  // Band-limited reconstruction at time tau from all N frequencies, weights[k] applied to X_k (for example
  // the frequency spacing, so the sum approximates the inverse transform).
  float movie(std::size_t voxel, double tau, std::span<const double> weights) const {
    double sum = 0;
    for (std::size_t k = 0; k < f_.size(); ++k) {
      const auto x = phasor(k, voxel);
      const double w = 2 * std::numbers::pi * f_[k] * tau;
      sum += weights[k] * (x.real() * std::cos(w) - x.imag() * std::sin(w));
    }
    return static_cast<float>(sum);
  }

private:
  std::vector<double> f_;
  std::size_t voxels_;
  std::vector<float> re_, im_;
};

} // namespace viz
