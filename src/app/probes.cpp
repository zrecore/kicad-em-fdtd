#include "app/probes.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "app/settings.hpp"

namespace app {

std::vector<ProbeSpec> parseProbes(const std::string& text) {
  std::vector<ProbeSpec> out;
  std::string normalised = text;
  std::replace(normalised.begin(), normalised.end(), '\n', ';');
  std::stringstream ss(normalised);
  for (std::string item; std::getline(ss, item, ';');) {
    if (item.find_first_not_of(" \t\r") == std::string::npos)
      continue;
    std::stringstream cs(item);
    double v[3];
    int n = 0;
    for (std::string c; std::getline(cs, c, ','); ++n) {
      char* end = nullptr;
      if (n >= 3)
        throw SettingsError("a probe is x,y,z in millimetres, got '" + item + "'");
      v[n] = std::strtod(c.c_str(), &end);
      if (end == c.c_str() || (*end && *end != ' ' && *end != '\t' && *end != '\r'))
        throw SettingsError("'" + c + "' is not a number in probe '" + item + "'");
    }
    if (n != 3)
      throw SettingsError("a probe is x,y,z in millimetres, got '" + item + "'");
    out.push_back({v[0], v[1], v[2]});
  }
  if (out.size() > 16)
    throw SettingsError("at most 16 probes");
  return out;
}

std::uint32_t probeNode(const fdtd::Grid& grid, const ProbeSpec& probe, double originX, double originY,
                        double bottomZ) {
  const double pos[3] = {probe.xMm * 1e-3 - originX, probe.yMm * 1e-3 - originY, bottomZ + probe.zMm * 1e-3};
  std::size_t idx[3];
  for (int a = 0; a < 3; ++a) {
    const auto ax = static_cast<fdtd::Axis>(a);
    std::size_t best = 0;
    for (std::size_t i = 1; i < grid.nodes(ax); ++i)
      if (std::abs(grid.edge(ax, i) - pos[a]) < std::abs(grid.edge(ax, best) - pos[a]))
        best = i;
    idx[a] = best;
  }
  return static_cast<std::uint32_t>(grid.nodeIndex(idx[0], idx[1], idx[2]));
}

std::string probesCsv(const std::vector<double>& frequenciesHz, const std::vector<ProbeSpec>& probes,
                      const std::vector<std::vector<std::complex<double>>>& phasors) {
  std::ostringstream out;
  out << "f_hz";
  for (std::size_t p = 0; p < probes.size(); ++p) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "probe%zu_(%.4g_%.4g_%.4g_mm)", p + 1, probes[p].xMm, probes[p].yMm,
                  probes[p].zMm);
    out << ',' << buf << "_re," << buf << "_im";
  }
  out << "\n";
  for (std::size_t k = 0; k < frequenciesHz.size(); ++k) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.12g", frequenciesHz[k]);
    out << buf;
    for (std::size_t p = 0; p < probes.size(); ++p) {
      std::snprintf(buf, sizeof buf, ",%.9g,%.9g", phasors[p][k].real(), phasors[p][k].imag());
      out << buf;
    }
    out << "\n";
  }
  return out.str();
}

} // namespace app
