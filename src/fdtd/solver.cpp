#include "fdtd/solver.hpp"

#include <cmath>
#include <stdexcept>

#include "core/units.hpp"

namespace fdtd {
namespace {

using units::kEps0;
using units::kMu0;

// The four cells sharing an E edge along `axis` at node (i, j, k); out-of-grid cells are skipped.
template <class F>
void forEdgeCells(const Grid& g, Axis axis, std::size_t i, std::size_t j, std::size_t k, F&& f) {
  const std::array<std::size_t, 3> node{i, j, k};
  // along the edge's own axis the edge lies inside one cell (index node[axis]); across the other two axes
  // it borders the cells on either side of the node.
  if (node[axis] >= g.cells(axis))
    return; // no edge sample exists at the far wall along its own axis
  const Axis u = axis == X ? Y : X, v = axis == Z ? Y : Z;
  for (int du = -1; du <= 0; ++du)
    for (int dv = -1; dv <= 0; ++dv) {
      const long cu = static_cast<long>(node[u]) + du, cv = static_cast<long>(node[v]) + dv;
      if (cu < 0 || cv < 0 || cu >= static_cast<long>(g.cells(u)) || cv >= static_cast<long>(g.cells(v)))
        continue;
      std::array<std::size_t, 3> c{};
      c[axis] = node[axis];
      c[u] = static_cast<std::size_t>(cu);
      c[v] = static_cast<std::size_t>(cv);
      f(g.cellIndex(c[X], c[Y], c[Z]));
    }
}

} // namespace

template <class T>
Solver<T>::Solver(const Grid& grid, double courant, double dtOverride)
    : grid_(grid), dt_(dtOverride > 0 ? dtOverride : courant * maxStableDt(grid)) {
  for (int a = 0; a < 3; ++a) {
    e_[a].assign(grid_.nodeCount(), T(0));
    h_[a].assign(grid_.nodeCount(), T(0));
  }
  for (Axis a : {X, Y, Z}) {
    invCell_[a].resize(grid_.cells(a));
    invDual_[a].resize(grid_.nodes(a));
    for (std::size_t i = 0; i < grid_.cells(a); ++i)
      invCell_[a][i] = static_cast<T>(1.0 / grid_.cellSize(a, i));
    for (std::size_t i = 0; i < grid_.nodes(a); ++i)
      invDual_[a][i] = static_cast<T>(1.0 / grid_.dualSize(a, i));
  }
  setMaterials({Material{}}, std::vector<std::uint8_t>(grid_.cellCount(), 0));
}

template <class T>
void Solver<T>::setMaterials(std::vector<Material> table, std::vector<std::uint8_t> cellMaterial) {
  if (cellMaterial.size() != grid_.cellCount())
    throw std::invalid_argument("one material id per cell");
  for (auto id : cellMaterial)
    if (id >= table.size())
      throw std::invalid_argument("material id out of range");
  table_ = std::move(table);
  cellMaterial_ = std::move(cellMaterial);
  buildCoefficients(table_, cellMaterial_);
}

template <class T> void Solver<T>::forcePecEdges(std::vector<PecEdge> edges) {
  for (const PecEdge& e : edges)
    if (e.node >= grid_.nodeCount())
      throw std::invalid_argument("PEC edge outside the grid");
  pecEdges_ = std::move(edges);
  buildCoefficients(table_, cellMaterial_);
}

template <class T> double Solver<T>::portArea(const LumpedPort& p) const {
  const std::array<std::size_t, 3> idx{p.i, p.j, p.k};
  double area = 1;
  for (Axis a : {X, Y, Z})
    if (a != p.component)
      area *= grid_.dualSize(a, idx[a]);
  return area;
}

template <class T> void Solver<T>::addLumpedPort(LumpedPort p) {
  checkInteriorSample(p.component, p.i, p.j, p.k);
  if (!(p.resistance > 0))
    throw std::invalid_argument("port resistance must be positive");
  if (!(p.inductance >= 0) || !(p.capacitance >= 0))
    throw std::invalid_argument("port inductance and capacitance must not be negative");
  ports_.push_back(std::move(p));
  portState_.emplace_back();
  portEOld_.push_back(T(0));
  buildCoefficients(table_, cellMaterial_); // the port adds conductivity to its edge
}

template <class T>
void Solver<T>::buildCoefficients(const std::vector<Material>& table,
                                  const std::vector<std::uint8_t>& cellMaterial) {
  for (Axis a : {X, Y, Z}) {
    ca_[a].assign(grid_.nodeCount(), T(0));
    cb_[a].assign(grid_.nodeCount(), T(0));
    for (std::size_t k = 0; k < grid_.nodes(Z); ++k)
      for (std::size_t j = 0; j < grid_.nodes(Y); ++j)
        for (std::size_t i = 0; i < grid_.nodes(X); ++i) {
          double eps = 0, sigma = 0;
          int count = 0;
          bool pec = false;
          forEdgeCells(grid_, a, i, j, k, [&](std::size_t cell) {
            const Material& m = table[cellMaterial[cell]];
            eps += m.epsR;
            sigma += m.sigma;
            pec = pec || m.pec;
            ++count;
          });
          if (count == 0 || pec)
            continue; // not a real sample, or inside a conductor: stays zero
          eps = eps / count * kEps0;
          sigma /= count;
          for (const LumpedPort& p : ports_)
            if (p.component == a && portNode(p) == grid_.nodeIndex(i, j, k)) {
              const double dl = grid_.cellSize(a, std::array<std::size_t, 3>{i, j, k}[a]);
              sigma += dl / (p.resistance * portArea(p));
              eps += p.capacitance * dl / portArea(p); // a lumped capacitor is extra permittivity on the edge
              // a lumped inductor, trapezoidal in time (I^(n+1/2) = I^(n-1/2) + dt dl / (2 L) (E^n +
              // E^(n+1))): the E^(n+1) part is an effective conductivity dt dl / (2 L A), the rest a current
              // state
              if (p.inductance > 0)
                sigma += dt_ * dl / (2 * p.inductance * portArea(p));
            }
          const double loss = sigma * dt_ / (2 * eps);
          const std::size_t n = grid_.nodeIndex(i, j, k);
          ca_[a][n] = static_cast<T>((1 - loss) / (1 + loss));
          cb_[a][n] = static_cast<T>(dt_ / eps / (1 + loss));
        }
  }
  for (const PecEdge& e : pecEdges_) {
    ca_[e.axis][e.node] = T(0);
    cb_[e.axis][e.node] = T(0);
  }
}

template <class T>
void Solver<T>::checkInteriorSample(Axis component, std::size_t i, std::size_t j, std::size_t k) const {
  const std::array<std::size_t, 3> idx{i, j, k};
  for (Axis a : {X, Y, Z})
    if (idx[a] >= grid_.cells(a) + 1 ||
        (a == component ? idx[a] >= grid_.cells(a) : idx[a] == 0 || idx[a] >= grid_.cells(a)))
      throw std::invalid_argument("source or port must sit on an interior E sample");
}

template <class T> void Solver<T>::addSource(SoftSource s) {
  checkInteriorSample(s.component, s.i, s.j, s.k);
  sources_.push_back(std::move(s));
}

template <class T> void Solver<T>::enableCpml(const CpmlSpec& spec) {
  const double eps0 = kEps0, eta0 = std::sqrt(kMu0 / kEps0);
  for (Axis a : {X, Y, Z}) {
    const std::size_t n = grid_.cells(a);
    if (2 * spec.cells >= n)
      throw std::invalid_argument("CPML thicker than the domain");
    const double lLo = grid_.edge(a, spec.cells) - grid_.edge(a, 0);
    const double lHi = grid_.edge(a, n) - grid_.edge(a, n - spec.cells);
    const double lo = grid_.edge(a, 0), hi = grid_.edge(a, n);
    // Coefficients at position x: depth into the layer d, sigma = sigma_max (d/L)^m, alpha falls to 0 at the
    // outer face of the layer's inner side (maximum at the wall).
    auto fill = [&](std::vector<T>& b, std::vector<T>& c, std::size_t count, auto position) {
      b.assign(count, T(0));
      c.assign(count, T(0));
      for (std::size_t i = 0; i < count; ++i) {
        const double x = position(i);
        const double l = x < 0.5 * (lo + hi) ? lLo : lHi;
        const double d = x < 0.5 * (lo + hi) ? std::max(0.0, lo + l - x) : std::max(0.0, x - (hi - l));
        if (d <= 0)
          continue;
        const double sigmaMax = -(spec.order + 1) * std::log(spec.reflectionTarget) / (2 * eta0 * l);
        const double sigma = sigmaMax * std::pow(d / l, spec.order);
        const double alpha = spec.alphaMax * (1 - d / l);
        const double bb = std::exp(-(sigma + alpha) * dt_ / eps0);
        b[i] = static_cast<T>(bb);
        c[i] = static_cast<T>(sigma / (sigma + alpha) * (bb - 1));
      }
    };
    fill(bH_[a], aH_[a], n, [&](std::size_t i) { return 0.5 * (grid_.edge(a, i) + grid_.edge(a, i + 1)); });
    fill(bE_[a], aE_[a], n + 1, [&](std::size_t i) { return grid_.edge(a, i); });
  }
  for (auto& p : psi_)
    p.assign(grid_.nodeCount(), T(0));
  cpml_ = true;
  cpmlCells_ = spec.cells;
}

template <class T> void Solver<T>::step() {
  const Grid& g = grid_;
  const std::size_t nx = g.cells(X), ny = g.cells(Y), nz = g.cells(Z);
  const T mu = static_cast<T>(dt_ / kMu0);
  auto& [ex, ey, ez] = e_;
  auto& [hx, hy, hz] = h_;

  for (std::size_t k = 0; k < nz; ++k)
    for (std::size_t j = 0; j < ny; ++j)
      for (std::size_t i = 0; i <= nx; ++i) { // Hx
        const T dy = invCell_[Y][j], dz = invCell_[Z][k];
        const std::size_t n = g.nodeIndex(i, j, k);
        hx[n] -=
            mu * ((ez[g.nodeIndex(i, j + 1, k)] - ez[n]) * dy - (ey[g.nodeIndex(i, j, k + 1)] - ey[n]) * dz);
      }
  for (std::size_t k = 0; k < nz; ++k)
    for (std::size_t j = 0; j <= ny; ++j)
      for (std::size_t i = 0; i < nx; ++i) { // Hy
        const T dx = invCell_[X][i], dz = invCell_[Z][k];
        const std::size_t n = g.nodeIndex(i, j, k);
        hy[n] -=
            mu * ((ex[g.nodeIndex(i, j, k + 1)] - ex[n]) * dz - (ez[g.nodeIndex(i + 1, j, k)] - ez[n]) * dx);
      }
  for (std::size_t k = 0; k <= nz; ++k)
    for (std::size_t j = 0; j < ny; ++j)
      for (std::size_t i = 0; i < nx; ++i) { // Hz
        const T dx = invCell_[X][i], dy = invCell_[Y][j];
        const std::size_t n = g.nodeIndex(i, j, k);
        hz[n] -=
            mu * ((ey[g.nodeIndex(i + 1, j, k)] - ey[n]) * dx - (ex[g.nodeIndex(i, j + 1, k)] - ex[n]) * dy);
      }
  if (cpml_)
    applyCpmlH();

  for (std::size_t p = 0; p < ports_.size(); ++p)
    portEOld_[p] = e_[ports_[p].component][portNode(ports_[p])];

  // E on interior samples only: samples on a wall stay zero (PEC), as do samples inside a conductor (ca = cb
  // = 0).
  for (std::size_t k = 1; k < nz; ++k)
    for (std::size_t j = 1; j < ny; ++j)
      for (std::size_t i = 0; i < nx; ++i) { // Ex
        const std::size_t n = g.nodeIndex(i, j, k);
        const T dy = invDual_[Y][j], dz = invDual_[Z][k];
        ex[n] = ca_[X][n] * ex[n] + cb_[X][n] * ((hz[n] - hz[g.nodeIndex(i, j - 1, k)]) * dy -
                                                 (hy[n] - hy[g.nodeIndex(i, j, k - 1)]) * dz);
      }
  for (std::size_t k = 1; k < nz; ++k)
    for (std::size_t j = 0; j < ny; ++j)
      for (std::size_t i = 1; i < nx; ++i) { // Ey
        const std::size_t n = g.nodeIndex(i, j, k);
        const T dx = invDual_[X][i], dz = invDual_[Z][k];
        ey[n] = ca_[Y][n] * ey[n] + cb_[Y][n] * ((hx[n] - hx[g.nodeIndex(i, j, k - 1)]) * dz -
                                                 (hz[n] - hz[g.nodeIndex(i - 1, j, k)]) * dx);
      }
  for (std::size_t k = 0; k < nz; ++k)
    for (std::size_t j = 1; j < ny; ++j)
      for (std::size_t i = 1; i < nx; ++i) { // Ez
        const std::size_t n = g.nodeIndex(i, j, k);
        const T dx = invDual_[X][i], dy = invDual_[Y][j];
        ez[n] = ca_[Z][n] * ez[n] + cb_[Z][n] * ((hy[n] - hy[g.nodeIndex(i - 1, j, k)]) * dx -
                                                 (hx[n] - hx[g.nodeIndex(i, j - 1, k)]) * dy);
      }

  if (cpml_)
    applyCpmlE();

  const double tNew = time_ + dt_;
  for (std::size_t p = 0; p < ports_.size(); ++p) {
    // Lumped port: the source half of the edge current; the resistor half sits in ca/cb (buildCoefficients).
    const LumpedPort& port = ports_[p];
    const std::size_t n = g.nodeIndex(port.i, port.j, port.k);
    const double vs = port.voltage(time_ + 0.5 * dt_);
    const double dl =
        g.cellSize(port.component, std::array<std::size_t, 3>{port.i, port.j, port.k}[port.component]);
    T& e = e_[port.component][n];
    e += cb_[port.component][n] * static_cast<T>(vs / (port.resistance * portArea(port)));
    if (port.inductance > 0) { // the inductor current drawn from the edge, then advanced with the new E
      e -= cb_[port.component][n] * static_cast<T>(portState_[p].inductorCurrent / portArea(port));
      portState_[p].inductorCurrent +=
          dt_ * dl / (2 * port.inductance) * (static_cast<double>(portEOld_[p]) + static_cast<double>(e));
    }
    portState_[p].vs = vs;
    // E now holds the new value; the old one is recovered from the update, so average the two.
    portState_[p].current = (vs - dl * 0.5 * (double(e) + double(portEOld_[p]))) / port.resistance;
  }
  for (const SoftSource& s : sources_) {
    const std::size_t n = g.nodeIndex(s.i, s.j, s.k);
    e_[s.component][n] += static_cast<T>(s.waveform(tNew));
  }
  time_ = tNew;
}

// One recursive-convolution term: psi = b*psi + a*d (kappa = 1, so the corrected derivative is d + psi).
template <class T> static inline T convolve(T& psi, T b, T a, T d) {
  psi = b * psi + a * d;
  return psi;
}

template <class T> void Solver<T>::applyCpmlH() {
  const Grid& g = grid_;
  const std::size_t nx = g.cells(X), ny = g.cells(Y), nz = g.cells(Z);
  const T mu = static_cast<T>(dt_ / kMu0);
  const auto& [ex, ey, ez] = e_;
  auto& [hx, hy, hz] = h_;
  auto& [pxy, pxz, pyz, pyx, pzx, pzy, e0, e1, e2, e3, e4, e5] = psi_;
  for (std::size_t k = 0; k < nz; ++k)
    for (std::size_t j = 0; j < ny; ++j)
      for (std::size_t i = 0; i <= nx; ++i) { // Hx
        const std::size_t n = g.nodeIndex(i, j, k);
        T sum = 0;
        if (aH_[Y][j] != T(0))
          sum +=
              convolve(pxy[n], bH_[Y][j], aH_[Y][j], (ez[g.nodeIndex(i, j + 1, k)] - ez[n]) * invCell_[Y][j]);
        if (aH_[Z][k] != T(0))
          sum -=
              convolve(pxz[n], bH_[Z][k], aH_[Z][k], (ey[g.nodeIndex(i, j, k + 1)] - ey[n]) * invCell_[Z][k]);
        hx[n] -= mu * sum;
      }
  for (std::size_t k = 0; k < nz; ++k)
    for (std::size_t j = 0; j <= ny; ++j)
      for (std::size_t i = 0; i < nx; ++i) { // Hy
        const std::size_t n = g.nodeIndex(i, j, k);
        T sum = 0;
        if (aH_[Z][k] != T(0))
          sum +=
              convolve(pyz[n], bH_[Z][k], aH_[Z][k], (ex[g.nodeIndex(i, j, k + 1)] - ex[n]) * invCell_[Z][k]);
        if (aH_[X][i] != T(0))
          sum -=
              convolve(pyx[n], bH_[X][i], aH_[X][i], (ez[g.nodeIndex(i + 1, j, k)] - ez[n]) * invCell_[X][i]);
        hy[n] -= mu * sum;
      }
  for (std::size_t k = 0; k <= nz; ++k)
    for (std::size_t j = 0; j < ny; ++j)
      for (std::size_t i = 0; i < nx; ++i) { // Hz
        const std::size_t n = g.nodeIndex(i, j, k);
        T sum = 0;
        if (aH_[X][i] != T(0))
          sum +=
              convolve(pzx[n], bH_[X][i], aH_[X][i], (ey[g.nodeIndex(i + 1, j, k)] - ey[n]) * invCell_[X][i]);
        if (aH_[Y][j] != T(0))
          sum -=
              convolve(pzy[n], bH_[Y][j], aH_[Y][j], (ex[g.nodeIndex(i, j + 1, k)] - ex[n]) * invCell_[Y][j]);
        hz[n] -= mu * sum;
      }
  (void)e0, (void)e1, (void)e2, (void)e3, (void)e4, (void)e5;
}

template <class T> void Solver<T>::applyCpmlE() {
  const Grid& g = grid_;
  const std::size_t nx = g.cells(X), ny = g.cells(Y), nz = g.cells(Z);
  auto& [ex, ey, ez] = e_;
  const auto& [hx, hy, hz] = h_;
  auto& [h0, h1, h2, h3, h4, h5, qxy, qxz, qyz, qyx, qzx, qzy] = psi_;
  for (std::size_t k = 1; k < nz; ++k)
    for (std::size_t j = 1; j < ny; ++j)
      for (std::size_t i = 0; i < nx; ++i) { // Ex
        const std::size_t n = g.nodeIndex(i, j, k);
        T sum = 0;
        if (aE_[Y][j] != T(0))
          sum +=
              convolve(qxy[n], bE_[Y][j], aE_[Y][j], (hz[n] - hz[g.nodeIndex(i, j - 1, k)]) * invDual_[Y][j]);
        if (aE_[Z][k] != T(0))
          sum -=
              convolve(qxz[n], bE_[Z][k], aE_[Z][k], (hy[n] - hy[g.nodeIndex(i, j, k - 1)]) * invDual_[Z][k]);
        ex[n] += cb_[X][n] * sum;
      }
  for (std::size_t k = 1; k < nz; ++k)
    for (std::size_t j = 0; j < ny; ++j)
      for (std::size_t i = 1; i < nx; ++i) { // Ey
        const std::size_t n = g.nodeIndex(i, j, k);
        T sum = 0;
        if (aE_[Z][k] != T(0))
          sum +=
              convolve(qyz[n], bE_[Z][k], aE_[Z][k], (hx[n] - hx[g.nodeIndex(i, j, k - 1)]) * invDual_[Z][k]);
        if (aE_[X][i] != T(0))
          sum -=
              convolve(qyx[n], bE_[X][i], aE_[X][i], (hz[n] - hz[g.nodeIndex(i - 1, j, k)]) * invDual_[X][i]);
        ey[n] += cb_[Y][n] * sum;
      }
  for (std::size_t k = 0; k < nz; ++k)
    for (std::size_t j = 1; j < ny; ++j)
      for (std::size_t i = 1; i < nx; ++i) { // Ez
        const std::size_t n = g.nodeIndex(i, j, k);
        T sum = 0;
        if (aE_[X][i] != T(0))
          sum +=
              convolve(qzx[n], bE_[X][i], aE_[X][i], (hy[n] - hy[g.nodeIndex(i - 1, j, k)]) * invDual_[X][i]);
        if (aE_[Y][j] != T(0))
          sum -=
              convolve(qzy[n], bE_[Y][j], aE_[Y][j], (hx[n] - hx[g.nodeIndex(i, j - 1, k)]) * invDual_[Y][j]);
        ez[n] += cb_[Z][n] * sum;
      }
  (void)h0, (void)h1, (void)h2, (void)h3, (void)h4, (void)h5;
}

template <class T> double Solver<T>::energy() const {
  // Each sample is weighted by the volume of the cell it belongs to (clamped at the far walls): an
  // approximation, adequate for bounded-versus-diverging checks.
  const Grid& g = grid_;
  double sum = 0;
  for (std::size_t k = 0; k < g.nodes(Z); ++k)
    for (std::size_t j = 0; j < g.nodes(Y); ++j)
      for (std::size_t i = 0; i < g.nodes(X); ++i) {
        const double vol = g.cellSize(X, std::min(i, g.cells(X) - 1)) *
                           g.cellSize(Y, std::min(j, g.cells(Y) - 1)) *
                           g.cellSize(Z, std::min(k, g.cells(Z) - 1));
        const std::size_t n = g.nodeIndex(i, j, k);
        for (int a = 0; a < 3; ++a) {
          sum += 0.5 * kEps0 * double(e_[a][n]) * double(e_[a][n]) * vol;
          sum += 0.5 * kMu0 * double(h_[a][n]) * double(h_[a][n]) * vol;
        }
      }
  return sum;
}

template <class T> std::span<const T> Solver<T>::field(Kind kind, Axis component) const {
  return kind == Kind::E ? std::span<const T>(e_[component]) : std::span<const T>(h_[component]);
}

template class Solver<double>;
template class Solver<float>;

} // namespace fdtd
