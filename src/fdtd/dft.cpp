#include "fdtd/dft.hpp"

#include <cmath>
#include <numbers>

namespace fdtd {

template <class T> void RunningDft<T>::add(double timeSeconds, double value) {
  for (std::size_t n = 0; n < f_.size(); ++n) {
    const double phase = 2.0 * std::numbers::pi * f_[n] * timeSeconds;
    re_[n].add(static_cast<T>(value * std::cos(phase)));
    im_[n].add(static_cast<T>(-value * std::sin(phase)));
  }
}

template class RunningDft<double>;
template class RunningDft<float>;

} // namespace fdtd
