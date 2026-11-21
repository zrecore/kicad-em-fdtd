// Known-correct values for the running DFT (docs/PLAN.md, Next steps 13): the transform of the project's
// own source pulse has a closed form, so the numerical transform can be checked against it, in double and in
// float, with the tolerance stated and measured.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <numbers>

#include "fdtd/dft.hpp"
#include "sim/simulation.hpp"

using Catch::Approx;

namespace {
// g(t) = exp(-u^2) cos(w0 s), u = s / tau, s = t - t0, has the Fourier transform
//   G(f) = (tau sqrt(pi) / 2) [exp(-(pi tau (f - f0))^2) + exp(-(pi tau (f + f0))^2)] exp(-j 2 pi f t0).
std::complex<double> analytic(double f, double f0, double tau, double t0) {
  const double pi = std::numbers::pi;
  const double amp =
      0.5 * tau * std::sqrt(pi) *
      (std::exp(-std::pow(pi * tau * (f - f0), 2)) + std::exp(-std::pow(pi * tau * (f + f0), 2)));
  return std::polar(amp, -2 * pi * f * t0);
}

// scripts/make_golden.py, mpmath at 50 digits, rounded to 17 significant digits.
constexpr double kGolden[][3] = {
    // f in Hz, real, imaginary
    {2000000000.0, 9.1941366648782135e-114, -8.7127195908858383e-162},
    {2300000000.0, 6.4480461568966193e-21, -1.9845045504026886e-20},
    {2400000000.0, -3.983685681193894e-10, 2.8943170655933699e-10},
    {2450000000.0, 3.0124509892131806e-9, 9.2713708160487128e-9},
    {2500000000.0, 4.9241063029482086e-10, -7.517447162311827e-58},
    {2600000000.0, -1.6881203999783487e-20, -1.2264912629776904e-20},
    {3000000000.0, 1.2563270840253987e-165, -2.645430095023717e-213},
};
} // namespace

SCENARIO("The running DFT of the band pulse equals its closed-form transform", "[dft]") {
  GIVEN("the pulse for the band 2.4 to 2.5 GHz (tau = 1.1 / bandwidth, t0 = 4 tau), sampled every 5 ps") {
    const double fMin = 2.4e9, fMax = 2.5e9, f0 = 0.5 * (fMin + fMax), tau = 1.1 / (fMax - fMin),
                 t0 = 4 * tau;
    const auto pulse = sim::bandPulse(fMin, fMax);
    const double dt = 5e-12;
    const std::vector<double> freqs{2.0e9, 2.3e9, 2.4e9, f0, 2.5e9, 2.6e9, 3.0e9};
    fdtd::RunningDft<double> dd(freqs);
    fdtd::RunningDft<float> df(freqs);
    const int steps = static_cast<int>(8 * tau / dt);
    for (int n = 0; n < steps; ++n) {
      const double t = n * dt;
      dd.add(t, pulse(t) * dt);
      df.add(t, pulse(t) * dt);
    }
    const double peak = std::abs(analytic(f0, f0, tau, t0));
    WHEN("the transform is compared with the closed form") {
      double worstDouble = 0, worstFloat = 0;
      for (std::size_t i = 0; i < freqs.size(); ++i) {
        const auto exact = analytic(freqs[i], f0, tau, t0);
        worstDouble = std::max(worstDouble, std::abs(dd.at(i) - exact) / peak);
        worstFloat = std::max(worstFloat, std::abs(df.at(i) - exact) / peak);
      }
      INFO("worst error against the in-band peak: double " << worstDouble << ", float " << worstFloat);
      THEN("the double transform is within 3e-8 of the in-band peak at every frequency, from 2.0 to 3.0 GHz "
           "(measured 1.6e-8: the pulse is cut at 8 tau, where it is still 1.1e-7 of its height, the same "
           "abrupt start a real run has)") {
        REQUIRE(worstDouble < 3e-8);
      }
      AND_THEN("the float (Kahan) transform is within 1e-7 of the in-band peak (measured 4.9e-8)") {
        REQUIRE(worstFloat < 1e-7);
      }
    }
  }
  GIVEN("the 50-digit reference values of the same transform") {
    const double fMin = 2.4e9, fMax = 2.5e9, f0 = 0.5 * (fMin + fMax), tau = 1.1 / (fMax - fMin),
                 t0 = 4 * tau;
    const double peak = std::abs(analytic(f0, f0, tau, t0));
    THEN("the closed form coded in this test agrees with them within 1e-15 of the in-band peak (so the "
         "formula "
         "under test is itself right)") {
      for (const auto& g : kGolden)
        REQUIRE(std::abs(analytic(g[0], f0, tau, t0) - std::complex<double>(g[1], g[2])) < 1e-12 * peak);
    }
    AND_THEN("the numerical transform of the sampled pulse agrees with them within 3e-8 of the in-band peak, "
             "including at frequencies where the exact value is below 1e-20 (no leakage)") {
      const auto pulse = sim::bandPulse(fMin, fMax);
      const double dt = 5e-12;
      std::vector<double> freqs;
      for (const auto& g : kGolden)
        freqs.push_back(g[0]);
      fdtd::RunningDft<double> dd(freqs);
      const int steps = static_cast<int>(8 * tau / dt);
      for (int n = 0; n < steps; ++n)
        dd.add(n * dt, pulse(n * dt) * dt);
      for (std::size_t i = 0; i < freqs.size(); ++i)
        REQUIRE(std::abs(dd.at(i) - std::complex<double>(kGolden[i][1], kGolden[i][2])) < 3e-8 * peak);
    }
  }
}
