// One-port S-parameter from a lumped port driven by a source Vs in series with R = reference impedance:
// V = Vs - R*I, so the forward wave is a = Vs/2 and S11 = (V - R*I)/(V + R*I) = 2*V/Vs - 1 (frequency
// domain).
#pragma once
#include <complex>
#include <iosfwd>
#include <vector>

namespace fdtd {

struct SParam1 {
  std::vector<double> frequenciesHz;
  std::vector<std::complex<double>> s11;
  double referenceOhm = 50;
};

// voltage[n] is the port voltage after step n (time (n+1)*dt); source[n] is Vs at the step's mid time
// (n+1/2)*dt. Each series is transformed with its own time stamps.
SParam1 s11FromPort(const std::vector<double>& voltage, const std::vector<double>& source, double dt,
                    std::vector<double> frequenciesHz, double referenceOhm);

// Touchstone v1 one-port file, real/imaginary format, frequencies in Hz.
void writeTouchstone(std::ostream& out, const SParam1& s, const std::string& comment);

} // namespace fdtd
