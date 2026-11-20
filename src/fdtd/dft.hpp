// Running DFT of a scalar time series at chosen frequencies, with compensated (Kahan) accumulation.
// Templated on the accumulator type: float is the CPU twin of the GPU accumulator, double is the reference.
#pragma once
#include <complex>
#include <vector>

namespace fdtd {

template <class T = double> class RunningDft {
public:
  explicit RunningDft(std::vector<double> frequenciesHz)
      : f_(std::move(frequenciesHz)), re_(f_.size()), im_(f_.size()) {}
  void add(double timeSeconds, double value); // accumulates value * exp(-j 2 pi f t) for every frequency
  std::complex<double> at(std::size_t index) const {
    return {static_cast<double>(re_[index].sum), static_cast<double>(im_[index].sum)};
  }
  const std::vector<double>& frequencies() const { return f_; }

private:
  struct Kahan {
    T sum = 0, c = 0;
    void add(T x) {
      const T y = x - c;
      const T t = sum + y;
      c = (t - sum) - y;
      sum = t;
    }
  };
  std::vector<double> f_;
  std::vector<Kahan> re_, im_;
};

} // namespace fdtd
