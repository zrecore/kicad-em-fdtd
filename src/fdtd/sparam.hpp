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

// The source's transform at the given frequencies, stamped as s11FromPort stamps it ((n + 1/2) dt): the Vs
// that the antenna figures need (fdtd/ntff.hpp); includes the sample interval, so it is the Fourier integral
// and comparable with the phasors the accumulators make (which carry their weight).
std::vector<std::complex<double>> sourceSpectrum(const std::vector<double>& source, double dt,
                                                 const std::vector<double>& frequenciesHz);

// The power the port delivers into the structure, per frequency, in the transform convention of
// fdtd/ntff.hpp (twice the phasor value): sum over the strips of Re(V_k conj(I_k)) with I_k = (Vs - V_k) /
// stripResistance. Unlike (1 - |S11|^2) |Vs|^2 / (4 z0) it does not assume that all strips see the same
// voltage. stripVoltage[k][n] is strip k's voltage after step n (time (n + 1) dt); each is averaged with the
// one before to refer it to (n + 1/2) dt, where the source and the port current are.
std::vector<double> acceptedPower(const std::vector<std::vector<double>>& stripVoltage,
                                  const std::vector<double>& source, double dt, double stripResistance,
                                  const std::vector<double>& frequenciesHz);

// voltage[n] is the port voltage after step n (time (n+1)*dt); source[n] is Vs at the step's mid time
// (n+1/2)*dt. Each series is transformed with its own time stamps.
SParam1 s11FromPort(const std::vector<double>& voltage, const std::vector<double>& source, double dt,
                    std::vector<double> frequenciesHz, double referenceOhm);

// Touchstone v1 one-port file, real/imaginary format, frequencies in Hz.
void writeTouchstone(std::ostream& out, const SParam1& s, const std::string& comment);
// Reads what writeTouchstone wrote (Hz, RI, one port). Throws std::runtime_error for another format.
SParam1 readTouchstone(std::istream& in);

} // namespace fdtd
