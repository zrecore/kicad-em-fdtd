#include "fdtd/sparam.hpp"

#include <iomanip>
#include <ostream>
#include <stdexcept>

#include "fdtd/dft.hpp"

namespace fdtd {

SParam1 s11FromPort(const std::vector<double>& voltage, const std::vector<double>& source, double dt,
                    std::vector<double> frequenciesHz, double referenceOhm) {
  if (voltage.size() != source.size())
    throw std::invalid_argument("voltage and source series must have the same length");
  RunningDft<double> v(frequenciesHz), vs(frequenciesHz);
  for (std::size_t n = 0; n < voltage.size(); ++n) {
    v.add((static_cast<double>(n) + 1.0) * dt, voltage[n]);
    vs.add((static_cast<double>(n) + 0.5) * dt, source[n]);
  }
  SParam1 out;
  out.referenceOhm = referenceOhm;
  out.frequenciesHz = std::move(frequenciesHz);
  for (std::size_t i = 0; i < out.frequenciesHz.size(); ++i)
    out.s11.push_back(2.0 * v.at(i) / vs.at(i) - 1.0);
  return out;
}

std::vector<std::complex<double>> sourceSpectrum(const std::vector<double>& source, double dt,
                                                 const std::vector<double>& frequenciesHz) {
  RunningDft<double> vs(frequenciesHz);
  for (std::size_t n = 0; n < source.size(); ++n)
    vs.add((static_cast<double>(n) + 0.5) * dt, source[n]);
  std::vector<std::complex<double>> out;
  for (std::size_t i = 0; i < frequenciesHz.size(); ++i)
    out.push_back(vs.at(i) * dt); // the sum times the sample interval: the Fourier integral
  return out;
}

std::vector<double> acceptedPower(const std::vector<std::vector<double>>& stripVoltage,
                                  const std::vector<double>& source, double dt, double stripResistance,
                                  const std::vector<double>& frequenciesHz) {
  RunningDft<double> vs(frequenciesHz);
  for (std::size_t n = 0; n < source.size(); ++n)
    vs.add((static_cast<double>(n) + 0.5) * dt, source[n] * dt);
  std::vector<double> power(frequenciesHz.size(), 0.0);
  for (const auto& series : stripVoltage) {
    RunningDft<double> v(frequenciesHz);
    double previous = 0;
    for (std::size_t n = 0; n < series.size(); ++n) {
      v.add((static_cast<double>(n) + 0.5) * dt, 0.5 * (previous + series[n]) * dt);
      previous = series[n];
    }
    for (std::size_t i = 0; i < frequenciesHz.size(); ++i) {
      const std::complex<double> current = (vs.at(i) - v.at(i)) / stripResistance;
      power[i] += (v.at(i) * std::conj(current)).real();
    }
  }
  return power;
}

void writeTouchstone(std::ostream& out, const SParam1& s, const std::string& comment) {
  out << "! " << comment << "\n# Hz S RI R " << s.referenceOhm << "\n" << std::setprecision(9);
  for (std::size_t i = 0; i < s.s11.size(); ++i)
    out << s.frequenciesHz[i] << ' ' << s.s11[i].real() << ' ' << s.s11[i].imag() << '\n';
}

} // namespace fdtd
