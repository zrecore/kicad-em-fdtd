// CPU reference FDTD solver: PEC outer walls, per-cell materials (permittivity, conductivity, PEC).
// Templated on the field scalar: double for analytic validation, float to compare against the GPU kernels.
//
// Time convention: after n steps time() = n*dt is the time of E; H is at (n - 1/2)*dt. energy() mixes the
// two, which gives a small bounded wobble but is exact enough to tell stable from unstable.
#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "fdtd/grid.hpp"

namespace fdtd {

struct SoftSource { // adds waveform(t) to one E component at one interior sample each step
  Axis component;
  std::size_t i, j, k;
  std::function<double(double)> waveform;
};

// A tangential E edge forced to zero (zero-thickness PEC sheet or via barrel), by axis and node index.
struct PecEdge {
  Axis axis;
  std::size_t node;
};

// Lumped resistive port across one E edge: a series voltage source `voltage(t)` and resistance R, i.e. the
// edge carries the current density (Vs - E*dl) / (R*dA) (3D lumped-element formulation [7] in docs/PLAN.md).
struct LumpedPort {
  std::size_t i, j, k; // the E sample it spans (interior, like a SoftSource)
  Axis component;
  double resistance; // ohm; infinity: no conductance (a pure capacitor)
  std::function<double(double)> voltage;
  double capacitance = 0; // farad across this edge, added as permittivity C dl / area (a lumped capacitor)
  double inductance = 0;  // henry across this edge, in parallel with R and C; 0: none (a lumped inductor)
};

// Convolution PML on all six walls (kappa = 1, polynomial sigma, linearly falling alpha) [4][5].
struct CpmlSpec {
  std::size_t cells = 8;
  double order = 3;
  double reflectionTarget = 1e-6; // theoretical normal-incidence reflection, sets sigma_max
  double alphaMax = 0.05;         // S/m, complex-frequency shift at the inner PML face
};

struct Material {
  double epsR = 1.0;
  double sigma = 0.0; // S/m
  bool pec = false;   // perfect conductor: tangential E on any edge touching this cell is zero
};

enum class Kind { E, H };

template <class T> class Solver {
public:
  // dt = courant * maxStableDt(grid), unless dtOverride > 0 (used to test instability).
  Solver(const Grid& grid, double courant, double dtOverride = 0.0);

  // table[id] describes a material; cellMaterial has one id per cell (grid.cellIndex order).
  void setMaterials(std::vector<Material> table, std::vector<std::uint8_t> cellMaterial);
  void addSource(SoftSource s);
  void forcePecEdges(std::vector<PecEdge> edges); // survive setMaterials
  void addLumpedPort(LumpedPort p);               // port indices follow the order of addition
  void enableCpml(const CpmlSpec& spec);
  void step(); // one leapfrog step: H, then E, then sources

  double dt() const { return dt_; }
  double time() const { return time_; }
  const Grid& grid() const { return grid_; }
  double energy() const;
  // Last step's port quantities: source voltage and current through the resistor (positive = out of the
  // source).
  double portSourceVoltage(std::size_t port) const { return portState_[port].vs; }
  double portCurrent(std::size_t port) const { return portState_[port].current; }
  double inductorCurrent(std::size_t port) const { return portState_[port].inductorCurrent; } // A, at n - 1/2
  std::span<const T> field(Kind kind, Axis component) const; // node-sized, see grid.hpp
  std::span<T> mutableField(Kind kind, Axis component) {     // for initial conditions
    return kind == Kind::E ? std::span<T>(e_[component]) : std::span<T>(h_[component]);
  }
  // Update coefficients and spacing tables, shared with the GPU backend (E = ca*E + cb*curl(H)).
  std::span<const T> ca(Axis a) const { return ca_[a]; }
  std::span<const T> cb(Axis a) const { return cb_[a]; }
  std::span<const T> invCell(Axis a) const { return invCell_[a]; }
  std::span<const T> invDual(Axis a) const { return invDual_[a]; }
  // CPML state shared with the GPU backend: layer count (0 = off) and recursive-convolution coefficients.
  std::size_t cpmlCells() const { return cpml_ ? cpmlCells_ : 0; }
  std::span<const T> cpmlBH(Axis a) const { return bH_[a]; }
  std::span<const T> cpmlAH(Axis a) const { return aH_[a]; }
  std::span<const T> cpmlBE(Axis a) const { return bE_[a]; }
  std::span<const T> cpmlAE(Axis a) const { return aE_[a]; }
  const std::vector<LumpedPort>& ports() const { return ports_; }
  // Transverse dual area dA and edge length dl of a port, for the GPU port kernel.
  double portArea(const LumpedPort& p) const;
  T e(Axis component, std::size_t i, std::size_t j, std::size_t k) const {
    return e_[component][grid_.nodeIndex(i, j, k)];
  }

private:
  void buildCoefficients(const std::vector<Material>& table, const std::vector<std::uint8_t>& cellMaterial);

  struct PortState {
    double vs = 0, current = 0;
    double inductorCurrent = 0; // I_L at n - 1/2: the inductor's state (docs/PLAN.md, References [29])
  };
  void checkInteriorSample(Axis component, std::size_t i, std::size_t j, std::size_t k) const;
  void applyCpmlH();
  void applyCpmlE();
  std::size_t portNode(const LumpedPort& p) const { return grid_.nodeIndex(p.i, p.j, p.k); }

  Grid grid_;
  double dt_, time_ = 0.0;
  std::vector<Material> table_;
  std::vector<std::uint8_t> cellMaterial_;
  std::vector<LumpedPort> ports_;
  std::vector<PecEdge> pecEdges_;
  std::vector<PortState> portState_;
  std::vector<T> portEOld_; // port edge E before the step's update
  bool cpml_ = false;
  std::size_t cpmlCells_ = 0;
  // CPML: per axis and index, recursive-convolution coefficients at cell centres (H) and nodes (E), plus the
  // 12 psi arrays (H: Hxy Hxz Hyz Hyx Hzx Hzy; E: Exy Exz Eyz Eyx Ezx Ezy, named by field and derivative
  // axis).
  std::array<std::vector<T>, 3> bH_, aH_, bE_, aE_;
  std::array<std::vector<T>, 12> psi_;
  std::array<std::vector<T>, 3> e_, h_;
  std::array<std::vector<T>, 3> ca_, cb_; // per E sample: E = ca*E + cb*curl(H)
  // 1/cell size and 1/dual size per axis and node, computed once (the GPU kernels use the same tables).
  std::array<std::vector<T>, 3> invCell_, invDual_;
  std::vector<SoftSource> sources_;
};

} // namespace fdtd
