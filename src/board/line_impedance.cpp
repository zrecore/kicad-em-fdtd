#include "board/line_impedance.hpp"

#include <cmath>

namespace board {
namespace {

constexpr double kEta0 = 376.730313668; // free-space wave impedance, ohm

double agm(double a, double b) {
  for (int i = 0; i < 40; ++i) {
    const double next = 0.5 * (a + b);
    b = std::sqrt(a * b);
    a = next;
  }
  return a;
}

double ellipticK(double k) {
  return M_PI / (2.0 * agm(1.0, std::sqrt(1.0 - k * k)));
}

} // namespace

double ellipticRatio(double k) {
  return ellipticK(k) / ellipticK(std::sqrt(1.0 - k * k));
}

LineImpedance microstripImpedance(double w, double h, double epsR) {
  const double u = w / h;
  const double a = 1.0 +
                   std::log((std::pow(u, 4) + std::pow(u / 52.0, 2)) / (std::pow(u, 4) + 0.432)) / 49.0 +
                   std::log(1.0 + std::pow(u / 18.1, 3)) / 18.7;
  const double b = 0.564 * std::pow((epsR - 0.9) / (epsR + 3.0), 0.053);
  const double eff = (epsR + 1.0) / 2.0 + (epsR - 1.0) / 2.0 * std::pow(1.0 + 10.0 / u, -a * b);
  const double fu = 6.0 + (2.0 * M_PI - 6.0) * std::exp(-std::pow(30.666 / u, 0.7528));
  const double zHomogeneous = kEta0 / (2.0 * M_PI) * std::log(fu / u + std::sqrt(1.0 + std::pow(2.0 / u, 2)));
  return {zHomogeneous / std::sqrt(eff), eff};
}

LineImpedance groundedCoplanarImpedance(double w, double gap, double h, double epsR) {
  const double a = w / 2.0, b = w / 2.0 + gap;
  const double k = a / b;
  const double k1 = std::tanh(M_PI * a / (2.0 * h)) / std::tanh(M_PI * b / (2.0 * h));
  const double r = ellipticRatio(k), r1 = ellipticRatio(k1);
  const double eff = 1.0 + (epsR - 1.0) * r1 / (r + r1); // filling factor q = r1 / (r + r1)
  return {kEta0 / 2.0 / std::sqrt(eff) / (r + r1), eff};
}

} // namespace board
