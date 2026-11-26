#include "fdtd/line2d.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

#include "core/units.hpp"

namespace fdtd::line2d {

namespace {

// Node coordinates along one axis from 0 to `length`: a node at every breakpoint, and between two
// neighbouring breakpoints cells of about `cell`, growing towards the middle of long intervals (GridOptions).
std::vector<double> axisNodes(double length, std::vector<double> breaks, double cell, const GridOptions& g) {
  breaks.push_back(0);
  breaks.push_back(length);
  std::sort(breaks.begin(), breaks.end());
  std::vector<double> pts;
  for (double b : breaks)
    if (pts.empty() || b - pts.back() > 1e-9 * cell)
      pts.push_back(b);
  const double growth = std::max(g.growth, 1.0), cmax = cell * std::max(g.maxCellFactor, 1.0);
  std::vector<double> nodes{pts.front()};
  for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
    const double a = pts[i], d = pts[i + 1] - a;
    std::vector<double> half; // cell sizes from the interval's ends towards its middle
    double s = cell, used = 0;
    while (used + s < d / 2 - 1e-12 * d) {
      half.push_back(s);
      used += s;
      s = std::min(s * growth, cmax);
    }
    // the middle: whatever is left, in as few equal cells as keep each at most the next size
    const double rest = d - 2 * used;
    const auto middle = static_cast<std::size_t>(std::max(1.0, std::ceil(rest / s - 1e-6)));
    std::vector<double> sizes(half.begin(), half.end());
    for (std::size_t k = 0; k < middle; ++k)
      sizes.push_back(rest / static_cast<double>(middle));
    sizes.insert(sizes.end(), half.rbegin(), half.rend());
    double at = a;
    for (std::size_t k = 0; k + 1 < sizes.size(); ++k) {
      at += sizes[k];
      nodes.push_back(at);
    }
    nodes.push_back(pts[i + 1]);
  }
  return nodes;
}

std::size_t nodeAt(const std::vector<double>& nodes, double v) {
  const auto it = std::lower_bound(nodes.begin(), nodes.end(), v - 1e-9 * (nodes.back() - nodes.front()));
  return static_cast<std::size_t>(std::min<std::ptrdiff_t>(it - nodes.begin(), nodes.size() - 1));
}

} // namespace

Raster rasterise(const CrossSection& s, double cell, bool airOnly, const GridOptions& grid) {
  if (!(s.width > 0) || !(s.height > 0) || !(cell > 0))
    throw std::invalid_argument("line2d: the section and the cell need a positive size");
  const double tol = 1e-9 * cell;
  std::vector<double> bx, by;
  for (const Conductor& c : s.conductors) {
    if (c.x0 < -tol || c.y0 < -tol || c.x1 > s.width + tol || c.y1 > s.height + tol || c.x1 < c.x0 ||
        c.y1 < c.y0)
      throw std::invalid_argument("line2d: a conductor lies outside the domain");
    bx.push_back(c.x0);
    bx.push_back(c.x1);
    by.push_back(c.y0);
    by.push_back(c.y1);
  }
  double top = 0;
  for (const Layer& l : s.layers) { // a layer interface is a grid line
    top += l.thickness;
    if (top < s.height)
      by.push_back(top);
  }
  Raster r;
  r.h = cell;
  r.xs = axisNodes(s.width, bx, cell, grid);
  r.ys = axisNodes(s.height, by, cell, grid);
  r.nx = r.xs.size();
  r.ny = r.ys.size();
  if (r.nx < 3 || r.ny < 3)
    throw std::invalid_argument("line2d: the cell is too large for the domain (fewer than 3 nodes)");
  if (r.nx * r.ny > 8'000'000)
    throw std::invalid_argument("line2d: the grid would have more than 8 million nodes; use a larger cell");
  const std::size_t cx = r.nx - 1, cy = r.ny - 1;
  std::vector<double> eps(cx * cy, 1.0); // per cell, row by row
  if (!airOnly)
    for (std::size_t j = 0; j < cy; ++j) {
      const double yc = 0.5 * (r.ys[j] + r.ys[j + 1]);
      double layerTop = 0;
      for (const Layer& l : s.layers) {
        layerTop += l.thickness;
        if (yc < layerTop) {
          for (std::size_t i = 0; i < cx; ++i)
            eps[j * cx + i] = l.epsR;
          break;
        }
      }
    }
  auto dx = [&](std::size_t i) { return r.xs[i + 1] - r.xs[i]; };
  auto dy = [&](std::size_t j) { return r.ys[j + 1] - r.ys[j]; };
  // A link carries eps (perpendicular size) / (length); the perpendicular size is half of each bordering
  // cell's.
  r.gx.assign(r.nx * r.ny, 0.0);
  r.gy.assign(r.nx * r.ny, 0.0);
  for (std::size_t j = 0; j < r.ny; ++j)
    for (std::size_t i = 0; i < r.nx; ++i) {
      if (i + 1 < r.nx) { // the link along x borders the cells below and above it
        double flux = 0;
        if (j > 0)
          flux += eps[(j - 1) * cx + i] * dy(j - 1);
        if (j + 1 < r.ny)
          flux += eps[j * cx + i] * dy(j);
        r.gx[r.at(i, j)] = 0.5 * flux / dx(i);
      }
      if (j + 1 < r.ny) { // the link along y borders the cells left and right of it
        double flux = 0;
        if (i > 0)
          flux += eps[j * cx + i - 1] * dx(i - 1);
        if (i + 1 < r.nx)
          flux += eps[j * cx + i] * dx(i);
        r.gy[r.at(i, j)] = 0.5 * flux / dy(j);
      }
    }
  r.fixed.assign(r.nx * r.ny, 0);
  r.value.assign(r.nx * r.ny, 0.0);
  auto hold = [&](std::size_t i, std::size_t j, double v) {
    r.fixed[r.at(i, j)] = 1;
    r.value[r.at(i, j)] = v;
  };
  if (s.walls.left == Wall::Ground)
    for (std::size_t j = 0; j < r.ny; ++j)
      hold(0, j, 0.0);
  if (s.walls.right == Wall::Ground)
    for (std::size_t j = 0; j < r.ny; ++j)
      hold(r.nx - 1, j, 0.0);
  if (s.walls.bottom == Wall::Ground)
    for (std::size_t i = 0; i < r.nx; ++i)
      hold(i, 0, 0.0);
  if (s.walls.top == Wall::Ground)
    for (std::size_t i = 0; i < r.nx; ++i)
      hold(i, r.ny - 1, 0.0);
  for (const Conductor& c : s.conductors) { // conductors win over the walls
    const std::size_t i0 = nodeAt(r.xs, c.x0), i1 = nodeAt(r.xs, c.x1), j0 = nodeAt(r.ys, c.y0),
                      j1 = nodeAt(r.ys, c.y1);
    for (std::size_t j = j0; j <= j1; ++j)
      for (std::size_t i = i0; i <= i1; ++i)
        hold(i, j, c.volts);
  }
  return r;
}

double energyCapacitance(const Raster& r, const std::vector<double>& phi) {
  double sum = 0;
  for (std::size_t j = 0; j < r.ny; ++j)
    for (std::size_t i = 0; i < r.nx; ++i) {
      const std::size_t k = r.at(i, j);
      if (i + 1 < r.nx) {
        const double d = phi[k] - phi[k + 1];
        sum += r.gx[k] * d * d;
      }
      if (j + 1 < r.ny) {
        const double d = phi[k] - phi[k + r.nx];
        sum += r.gy[k] * d * d;
      }
    }
  return fdtd::units::kEps0 * sum; // square cells: a link's conductance is eps_link, whatever h is
}

namespace {
// The node's new value from its four neighbours: sum(g phi) / sum(g).
inline double gaussSeidel(const Raster& r, const std::vector<double>& phi, std::size_t i, std::size_t j) {
  const std::size_t k = r.at(i, j);
  double num = 0, den = 0;
  if (i > 0) {
    const double g = r.gx[k - 1];
    num += g * phi[k - 1];
    den += g;
  }
  if (i + 1 < r.nx) {
    const double g = r.gx[k];
    num += g * phi[k + 1];
    den += g;
  }
  if (j > 0) {
    const double g = r.gy[k - r.nx];
    num += g * phi[k - r.nx];
    den += g;
  }
  if (j + 1 < r.ny) {
    const double g = r.gy[k];
    num += g * phi[k + r.nx];
    den += g;
  }
  return den > 0 ? num / den : phi[k];
}

} // namespace

Solution solveRaster(const Raster& r, const SolveOptions& options) {
  Solution out;
  out.nx = r.nx;
  out.ny = r.ny;
  out.cell = r.h;
  out.phi.assign(r.nx * r.ny, 0.0);
  for (std::size_t k = 0; k < out.phi.size(); ++k)
    if (r.fixed[k])
      out.phi[k] = r.value[k];
  std::vector<double>& phi = out.phi;
  double omega = options.omega;
  if (omega <= 0)
    omega = std::min(1.99, 2.0 / (1.0 + std::sin(M_PI / static_cast<double>(std::max(r.nx, r.ny)))));
  std::vector<double> next;
  if (options.method == Method::Jacobi)
    next = phi;
  for (out.iterations = 0; out.iterations < options.maxIterations;) {
    double change = 0;
    if (options.method == Method::Jacobi) { // every node from the previous sweep: two buffers, fully parallel
      for (std::size_t j = 0; j < r.ny; ++j)
        for (std::size_t i = 0; i < r.nx; ++i) {
          const std::size_t k = r.at(i, j);
          if (r.fixed[k])
            continue;
          next[k] = gaussSeidel(r, phi, i, j);
          change = std::max(change, std::abs(next[k] - phi[k]));
        }
      phi.swap(next);
    } else { // red-black: all nodes of one colour are independent of each other
      for (int colour = 0; colour < 2; ++colour)
        for (std::size_t j = 0; j < r.ny; ++j)
          for (std::size_t i = (j + static_cast<std::size_t>(colour)) % 2; i < r.nx; i += 2) {
            const std::size_t k = r.at(i, j);
            if (r.fixed[k])
              continue;
            const double target = gaussSeidel(r, phi, i, j);
            const double v = phi[k] + omega * (target - phi[k]);
            change = std::max(change, std::abs(v - phi[k]));
            phi[k] = v;
          }
    }
    ++out.iterations;
    out.change = change;
    if (change < options.tolerance) {
      out.converged = true;
      break;
    }
  }
  out.capacitance = energyCapacitance(r, phi);
  return out;
}

Solution solveCapacitance(const CrossSection& section, double cell, const SolveOptions& options, bool airOnly,
                          const GridOptions& grid) {
  return solveRaster(rasterise(section, cell, airOnly, grid), options);
}

namespace {
LineResult combine(const Solution& c, const Solution& c0, bool converged, std::size_t iterations) {
  LineResult r;
  r.capacitance = c.capacitance;
  r.airCapacitance = c0.capacitance;
  r.epsEff = c.capacitance / c0.capacitance;
  r.z0Ohm = 1.0 / (fdtd::units::kSpeedOfLight * std::sqrt(c.capacitance * c0.capacitance));
  r.iterations = iterations;
  r.converged = converged;
  return r;
}
} // namespace

RasterSolver cpuSolver(const SolveOptions& options) {
  return [options](const Raster& r) { return solveRaster(r, options); };
}

LineResult lineImpedance(const CrossSection& section, double cell, const RasterSolver& solver,
                         const GridOptions& grid) {
  const Solution c = solver(rasterise(section, cell, false, grid));
  const Solution c0 = solver(rasterise(section, cell, true, grid));
  return combine(c, c0, c.converged && c0.converged, std::max(c.iterations, c0.iterations));
}

LineResult lineImpedance(const CrossSection& section, double cell, const SolveOptions& options,
                         const GridOptions& grid) {
  return lineImpedance(section, cell, cpuSolver(options), grid);
}

LineResult lineImpedanceExtrapolated(const CrossSection& section, double cell, const RasterSolver& solver,
                                     const GridOptions& grid) {
  const Solution c1 = solver(rasterise(section, cell, false, grid)),
                 c2 = solver(rasterise(section, cell / 2, false, grid));
  const Solution a1 = solver(rasterise(section, cell, true, grid)),
                 a2 = solver(rasterise(section, cell / 2, true, grid));
  Solution c = c2, a = a2; // the extrapolated capacitances ride in these two
  c.capacitance = 2 * c2.capacitance - c1.capacitance;
  a.capacitance = 2 * a2.capacitance - a1.capacitance;
  const std::size_t iterations =
      std::max(std::max(c1.iterations, c2.iterations), std::max(a1.iterations, a2.iterations));
  return combine(c, a, c1.converged && c2.converged && a1.converged && a2.converged, iterations);
}

LineResult lineImpedanceExtrapolated(const CrossSection& section, double cell, const SolveOptions& options,
                                     const GridOptions& grid) {
  return lineImpedanceExtrapolated(section, cell, cpuSolver(options), grid);
}

namespace {
// Domain margins beside and above the line: a multiple of the larger of the line's own width and the
// substrate.
double margin(double lineWidth, double h, double factor) {
  return factor * std::max(lineWidth, h);
}
} // namespace

CrossSection microstripSection(double w, double h, double epsR, double thickness, double marginFactor) {
  CrossSection s;
  const double side = margin(w, h, marginFactor);
  s.width = w + 2 * side;
  s.height = h + side;
  s.layers = {{h, epsR}};
  s.conductors = {{(s.width - w) / 2, (s.width + w) / 2, h, h + thickness, 1.0}};
  return s; // the ground plane is the grounded bottom wall; the other walls are distant shields
}

CrossSection groundedCoplanarSection(double w, double gap, double h, double epsR, double thickness,
                                     bool groundLeft, bool groundRight, double marginFactor) {
  CrossSection s;
  const double side = margin(w + 2 * gap, h, marginFactor);
  s.width = w + 2 * gap + 2 * side;
  s.height = h + side;
  s.layers = {{h, epsR}};
  const double x0 = (s.width - w) / 2, x1 = (s.width + w) / 2;
  s.conductors = {{x0, x1, h, h + thickness, 1.0}};
  if (groundLeft)
    s.conductors.push_back({0, x0 - gap, h, h + thickness, 0.0});
  if (groundRight)
    s.conductors.push_back({x1 + gap, s.width, h, h + thickness, 0.0});
  return s;
}

} // namespace fdtd::line2d
