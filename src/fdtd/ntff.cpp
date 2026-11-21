#include "fdtd/ntff.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <thread>
#include <tuple>

#include "core/units.hpp"

namespace fdtd {
namespace {

constexpr std::uint32_t kNone = std::numeric_limits<std::uint32_t>::max();

// Width of the dual cell of node n inside [lo, hi]: half a cell at the ends of the range.
double dual(const Grid& g, Axis a, std::size_t n, std::size_t lo, std::size_t hi) {
  const double below = n > lo ? g.cellSize(a, n - 1) : 0.0, above = n < hi ? g.cellSize(a, n) : 0.0;
  return 0.5 * (below + above);
}

double centre(const Grid& g, Axis a, std::size_t cell) {
  return 0.5 * (g.edge(a, cell) + g.edge(a, cell + 1));
}

class Registry {
public:
  explicit Registry(NtffPlan& plan) : plan_(plan) {}
  std::uint32_t get(Kind kind, Axis axis, std::uint32_t node) {
    const auto key = std::make_tuple(static_cast<int>(kind), static_cast<int>(axis), node);
    const auto it = ids_.find(key);
    if (it != ids_.end())
      return it->second;
    const auto id = static_cast<std::uint32_t>(plan_.channels.size());
    plan_.channels.push_back({kind, axis, node});
    ids_[key] = id;
    return id;
  }

private:
  NtffPlan& plan_;
  std::map<std::tuple<int, int, std::uint32_t>, std::uint32_t> ids_;
};

} // namespace

NtffPlan makeNtffPlan(const Grid& g, std::array<std::size_t, 3> lo, std::array<std::size_t, 3> hi) {
  NtffPlan plan;
  Registry reg(plan);
  for (int a = 0; a < 3; ++a)
    plan.centre[static_cast<std::size_t>(a)] =
        0.5 * (g.edge(static_cast<Axis>(a), lo[static_cast<std::size_t>(a)]) +
               g.edge(static_cast<Axis>(a), hi[static_cast<std::size_t>(a)]));
  for (int ai = 0; ai < 3; ++ai) {
    const Axis A = static_cast<Axis>(ai), T1 = static_cast<Axis>((ai + 1) % 3),
               T2 = static_cast<Axis>((ai + 2) % 3);
    for (int side : {-1, 1}) {
      const double s = side;
      const std::size_t p = side < 0 ? lo[ai] : hi[ai];
      auto node = [&](std::size_t i, std::size_t j, std::size_t k) { // t1 = i, t2 = j, a = k
        std::array<std::size_t, 3> idx{};
        idx[A] = k, idx[T1] = i, idx[T2] = j;
        return static_cast<std::uint32_t>(g.nodeIndex(idx[0], idx[1], idx[2]));
      };
      auto position = [&](double t1, double t2) {
        std::array<double, 3> x{};
        x[A] = g.edge(A, p), x[T1] = t1, x[T2] = t2;
        return x;
      };
      // J_t2 = s H_t1 at (t1 node, t2 centre); H_t1 is half a cell either side of the face along a
      for (std::size_t j = lo[T2]; j < hi[T2]; ++j)
        for (std::size_t i = lo[T1]; i <= hi[T1]; ++i) {
          const double w = dual(g, T1, i, lo[T1], hi[T1]) * g.cellSize(T2, j);
          const auto ha = reg.get(Kind::H, T1, node(i, j, p - 1)), hb = reg.get(Kind::H, T1, node(i, j, p));
          plan.elements.push_back({'J', T2, position(g.edge(T1, i), centre(g, T2, j)), w, s, ha, hb});
          // M_t1 = s E_t2 at the same place, E on the face itself
          const auto e = reg.get(Kind::E, T2, node(i, j, p));
          plan.elements.push_back({'M', T1, position(g.edge(T1, i), centre(g, T2, j)), w, s, e, kNone});
          plan.flux.push_back({-s, w, e, ha, hb}); // -s 1/2 Re(E_t2 conj(H_t1))
        }
      // J_t1 = -s H_t2 at (t1 centre, t2 node); M_t2 = -s E_t1 at the same place
      for (std::size_t j = lo[T2]; j <= hi[T2]; ++j)
        for (std::size_t i = lo[T1]; i < hi[T1]; ++i) {
          const double w = g.cellSize(T1, i) * dual(g, T2, j, lo[T2], hi[T2]);
          const auto ha = reg.get(Kind::H, T2, node(i, j, p - 1)), hb = reg.get(Kind::H, T2, node(i, j, p));
          plan.elements.push_back({'J', T1, position(centre(g, T1, i), g.edge(T2, j)), w, -s, ha, hb});
          const auto e = reg.get(Kind::E, T1, node(i, j, p));
          plan.elements.push_back({'M', T2, position(centre(g, T1, i), g.edge(T2, j)), w, -s, e, kNone});
          plan.flux.push_back({s, w, e, ha, hb}); // +s 1/2 Re(E_t1 conj(H_t2))
        }
    }
  }
  return plan;
}

FarField farField(const NtffPlan& plan, const PhasorOf& phasor, double frequencyHz, std::size_t nTheta,
                  std::size_t nPhi) {
  using C = std::complex<double>;
  const double pi = std::numbers::pi;
  const double k = 2 * pi * frequencyHz / units::kSpeedOfLight;
  const double eta = std::sqrt(units::kMu0 / units::kEps0);
  // the value of every element once
  std::vector<C> value(plan.elements.size());
  for (std::size_t n = 0; n < value.size(); ++n) {
    const NtffElement& e = plan.elements[n];
    C v = phasor(e.chanA);
    if (e.chanB != kNone)
      v = 0.5 * (v + phasor(e.chanB));
    value[n] = e.sign * v * e.weight;
  }
  FarField ff;
  ff.nTheta = nTheta, ff.nPhi = nPhi;
  for (std::size_t i = 0; i < nTheta; ++i)
    ff.theta.push_back((static_cast<double>(i) + 0.5) * pi / static_cast<double>(nTheta));
  for (std::size_t j = 0; j < nPhi; ++j)
    ff.phi.push_back((static_cast<double>(j) + 0.5) * 2 * pi / static_cast<double>(nPhi));
  ff.u.assign(nTheta * nPhi, 0.0);
  const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
  auto work = [&](std::size_t first, std::size_t step) {
    for (std::size_t it = first; it < nTheta; it += step)
      for (std::size_t ip = 0; ip < nPhi; ++ip) {
        const double st = std::sin(ff.theta[it]), ct = std::cos(ff.theta[it]);
        const double sp = std::sin(ff.phi[ip]), cp = std::cos(ff.phi[ip]);
        const std::array<double, 3> rhat{st * cp, st * sp, ct};
        std::array<C, 3> N{}, L{};
        for (std::size_t n = 0; n < value.size(); ++n) {
          const NtffElement& e = plan.elements[n];
          double dot = 0;
          for (std::size_t a = 0; a < 3; ++a)
            dot += rhat[a] * (e.pos[a] - plan.centre[a]);
          const C term = value[n] * std::polar(1.0, k * dot); // exp(+j k r_hat . r')
          (e.type == 'J' ? N : L)[static_cast<std::size_t>(e.axis)] += term;
        }
        const std::array<double, 3> th{ct * cp, ct * sp, -st}, ph{-sp, cp, 0.0};
        auto comp = [](const std::array<C, 3>& v, const std::array<double, 3>& d) {
          return v[0] * d[0] + v[1] * d[1] + v[2] * d[2];
        };
        const C nt = comp(N, th), np = comp(N, ph), lt = comp(L, th), lp = comp(L, ph);
        ff.u[it * nPhi + ip] =
            k * k / (32 * pi * pi * eta) * (std::norm(lp + eta * nt) + std::norm(lt - eta * np));
      }
  };
  std::vector<std::thread> pool;
  for (unsigned t = 1; t < threads; ++t)
    pool.emplace_back(work, t, threads);
  work(0, threads);
  for (auto& t : pool)
    t.join();
  const double dTheta = pi / static_cast<double>(nTheta), dPhi = 2 * pi / static_cast<double>(nPhi);
  for (std::size_t i = 0; i < nTheta; ++i)
    for (std::size_t j = 0; j < nPhi; ++j) {
      const double u = ff.u[i * nPhi + j];
      ff.power += u * std::sin(ff.theta[i]) * dTheta * dPhi;
      if (u > ff.peak) {
        ff.peak = u;
        ff.peakTheta = i;
        ff.peakPhi = j;
      }
    }
  return ff;
}

AntennaFigures antennaFigures(const FarField& ff, std::complex<double> vs, std::complex<double> s11,
                              double z0, double accepted) {
  AntennaFigures a;
  a.radiatedPower = 2 * ff.power;
  a.availablePower = std::norm(vs) / (4 * z0);
  a.acceptedPower = accepted >= 0 ? accepted : a.availablePower * (1 - std::min(1.0, std::norm(s11)));
  a.efficiency = a.acceptedPower > 0 ? a.radiatedPower / a.acceptedPower : 0;
  a.directivity = ff.directivity();
  a.gain = a.efficiency * a.directivity;
  a.realisedGain =
      a.availablePower > 0 ? 4 * std::numbers::pi * 2 * ff.peak / a.availablePower : 0; // 4 pi U / P_avail
  a.peakThetaDeg = ff.theta.empty() ? 0 : ff.theta[ff.peakTheta] * 180 / std::numbers::pi;
  a.peakPhiDeg = ff.phi.empty() ? 0 : ff.phi[ff.peakPhi] * 180 / std::numbers::pi;
  return a;
}

double surfaceFlux(const NtffPlan& plan, const PhasorOf& phasor) {
  double sum = 0;
  for (const NtffFluxTerm& t : plan.flux) {
    const std::complex<double> h = 0.5 * (phasor(t.chanHa) + phasor(t.chanHb));
    sum += t.sign * 0.5 * (phasor(t.chanE) * std::conj(h)).real() * t.weight;
  }
  return sum;
}

} // namespace fdtd
