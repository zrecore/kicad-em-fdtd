// Two-dimensional quasi-static field solve of a transmission line's cross-section: the characteristic
// impedance and effective permittivity of a microstrip, a coplanar waveguide with or without ground below it,
// strips of real thickness, one-sided grounds and any other planar section, without the limits of the closed
// forms in board/line_impedance.hpp (infinitely thin copper, symmetric side grounds, infinite substrate).
//
// Method (docs/PLAN.md, "2D cross-section solve"): the potential phi satisfies div(eps grad phi) = 0 between
// conductors held at fixed potentials. Square cells of size h; phi on the nodes, eps on the cells; the link
// between two nodes carries the mean of the permittivities of the (at most two) cells it borders, so a
// conductor sheet lies on a node row and a dielectric interface between two node rows needs no averaging
// across it. The capacitance per metre is the stored energy, C = eps0 sum over links of eps_link (phi_a -
// phi_b)^2, with the signal conductor at 1 V and the others at 0 V. Solved twice, with the dielectrics and
// with air everywhere: eps_eff = C / C0 and Z0 = 1 / (c sqrt(C C0)) (the TEM relations of a lossless line in
// an inhomogeneous dielectric). Dispersion, loss and the real part of any radiation are not in a quasi-static
// model.
//
// The iteration is the one a GPU would run: Jacobi (every node from its four neighbours of the previous
// sweep, two buffers) or red-black Gauss-Seidel with over-relaxation (two half-sweeps, each fully parallel).
// This file is the CPU reference both are compared against. Pure logic, no Vulkan.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace fdtd::line2d {

// Domain edges: Ground holds phi at 0 V (a shield or a distant boundary); Mirror makes the field cross the
// edge at right angles (zero normal derivative: a symmetry plane, or the edge of a plate wider than the
// domain).
enum class Wall { Ground, Mirror };
struct Walls {
  Wall left = Wall::Ground, right = Wall::Ground, bottom = Wall::Ground, top = Wall::Ground;
};

struct Layer {
  double thickness = 0; // metres
  double epsR = 1;
};

// A conductor rectangle held at `volts`; y0 == y1 is a zero-thickness sheet. Edges snap to the nearest node.
struct Conductor {
  double x0, x1, y0, y1;
  double volts;
};

struct CrossSection {
  double width = 0, height = 0; // the domain, metres: x from 0 to width, y from 0 to height
  std::vector<Layer> layers;    // dielectric stack from y = 0 upwards; above the last one: air
  std::vector<Conductor> conductors;
  Walls walls;
};

enum class Method { RedBlackSor, Jacobi };

struct SolveOptions {
  Method method = Method::RedBlackSor;
  double tolerance = 1e-10; // largest change of phi in one sweep (volts)
  std::size_t maxIterations = 200000;
  double omega = 0; // over-relaxation for red-black; 0 picks one from the grid size
};

struct Solution {
  std::size_t nx = 0, ny = 0; // nodes
  double cell = 0;
  std::vector<double> phi; // ny rows of nx nodes
  double capacitance = 0;  // F/m
  std::size_t iterations = 0;
  double change = 0; // largest change in the last sweep
  bool converged = false;
};

// The rasterised section, the one input every backend solves: node-based unknowns phi on nx x ny nodes (row
// by row) and the conductance g of each link, which is all a solver needs (a link carries eps times its
// perpendicular size over its length, so a graded grid changes only these numbers). The grid is a tensor
// product of node coordinates xs and ys with a grid line on every conductor edge and every layer interface,
// so widths, gaps, thicknesses and heights are exact whatever the cell. A solver sets each free
// node to sum(g phi) / sum(g) over its neighbours; held nodes keep `value`.
struct Raster {
  std::size_t nx = 0, ny = 0;
  std::vector<double> xs, ys;      // node coordinates, metres (nx and ny of them, increasing)
  double h = 0;                    // the (smallest) cell size, metres
  std::vector<double> gx, gy;      // link conductance to the node at +x / +y (0 on the last column / row)
  std::vector<std::uint8_t> fixed; // 1: phi is held at `value`
  std::vector<double> value;
  std::size_t at(std::size_t i, std::size_t j) const { return j * nx + i; }
};

// How the cells between the fixed grid lines are laid out. growth 1 (default): equal cells of about `cell`
// in each interval. growth > 1: the cell is `cell` at both ends of an interval (where a conductor edge or an
// interface is) and grows by `growth` per cell towards its middle, up to `maxCellFactor` times `cell`.
struct GridOptions {
  double growth = 1.0;
  double maxCellFactor = 1.0;
};

// Throws std::invalid_argument for a section that cannot be rasterised (no size, a conductor outside the
// domain, fewer than 3 nodes either way, more than 8 million nodes).
Raster rasterise(const CrossSection& section, double cell, bool airOnly = false,
                 const GridOptions& grid = {});

// C = eps0 sum over links of g (phi_a - phi_b)^2, per metre of line.
double energyCapacitance(const Raster& raster, const std::vector<double>& phi);

// Solves a rasterised section on the CPU (double precision); the reference the GPU solver is compared to.
Solution solveRaster(const Raster& raster, const SolveOptions& options = {});

// A backend's solve of a rasterised section (the CPU one above, or the GPU one in fdtd_vk).
using RasterSolver = std::function<Solution(const Raster&)>;

// The CPU solver as a RasterSolver, for the functions below.
RasterSolver cpuSolver(const SolveOptions& options = {});

// Rasterises the section on square cells of size `cell` and solves it. With `airOnly` every dielectric is
// replaced by air. Throws what `rasterise` throws.
Solution solveCapacitance(const CrossSection& section, double cell, const SolveOptions& options = {},
                          bool airOnly = false, const GridOptions& grid = {});

struct LineResult {
  double z0Ohm = 0, epsEff = 0;
  double capacitance = 0, airCapacitance = 0; // F/m
  std::size_t iterations = 0;                 // of the slower of the two solves
  bool converged = false;
};

// The signal conductors are those at 1 V, the others at 0 V; C is the capacitance of the 1 V conductors
// against the 0 V ones and the grounded walls.
LineResult lineImpedance(const CrossSection& section, double cell, const SolveOptions& options = {},
                         const GridOptions& grid = {});
LineResult lineImpedance(const CrossSection& section, double cell, const RasterSolver& solver,
                         const GridOptions& grid = {});

// The same, corrected for the grid. A thin conductor's edge makes the field singular there, so the
// capacitance converges only linearly with the cell (measured: a coplanar line read 80.5, 86.0, 88.6 and 89.9
// ohm at cells of 0.2, 0.1, 0.05 and 0.025 mm against 91.3 from the closed form). This solves at `cell` and
// `cell / 2` and extrapolates each capacitance as C = 2 C(h/2) - C(h), which assumes first-order convergence:
// exact for a sheet's edge, slightly off for a conductor with real thickness (a 270 degree corner converges a
// little faster). Costs about five times one solve.
LineResult lineImpedanceExtrapolated(const CrossSection& section, double cell,
                                     const SolveOptions& options = {}, const GridOptions& grid = {});
LineResult lineImpedanceExtrapolated(const CrossSection& section, double cell, const RasterSolver& solver,
                                     const GridOptions& grid = {});

// The sections the feed checks meet, inside grounded walls (a shield): the margin beside and above the line
// is `marginFactor` times the larger of the line's own width and the substrate height. The walls add
// capacitance that an open line does not have, so the impedance reads a little low; raise the factor to see
// it shrink.
CrossSection microstripSection(double w, double h, double epsR, double thickness = 0,
                               double marginFactor = 4);
CrossSection groundedCoplanarSection(double w, double gap, double h, double epsR, double thickness = 0,
                                     bool groundLeft = true, bool groundRight = true,
                                     double marginFactor = 4);

} // namespace fdtd::line2d
