// Parameters shared by the C++ host code and the GLSL kernels (one definition, included by both).
#ifndef FDTD_PARAMS_H
#define FDTD_PARAMS_H
#ifdef __cplusplus
#include <cstdint>
namespace fdtd_gpu {
using u32 = std::uint32_t;
#else
#define u32 uint
#endif

// Phases of one leapfrog step, in dispatch order: H, CPML-H per axis, E, CPML-E per axis, ports, counter.
#define FDTD_PHASE_H 0
#define FDTD_PHASE_E 1
#define FDTD_PHASE_CPML_H 2 /* + derivative axis 0..2 */
#define FDTD_PHASE_CPML_E 5 /* + derivative axis 0..2 */
#define FDTD_PHASE_PORTS 8
#define FDTD_PHASE_COUNTER 9
#ifdef __cplusplus
constexpr u32 kPhaseH = FDTD_PHASE_H, kPhaseE = FDTD_PHASE_E, kPhaseCpmlH = FDTD_PHASE_CPML_H,
              kPhaseCpmlE = FDTD_PHASE_CPML_E, kPhasePorts = FDTD_PHASE_PORTS,
              kPhaseCounter = FDTD_PHASE_COUNTER;
#endif

struct StepParams {
  u32 nxn, nyn, nzn;                          // node counts per axis (cells + 1)
  u32 phase;                                  // FDTD_PHASE_*
  float dtOverMu;                             // dt / mu0
  u32 icx, icy, icz;                          // offsets of the 1/cell-size tables in the tables buffer
  u32 idx, idy, idz;                          // offsets of the 1/dual-size tables in the tables buffer
  u32 pmlCells;                               // CPML layers per wall (0: none)
  u32 pmlBH[3], pmlAH[3], pmlBE[3], pmlAE[3]; // CPML coefficient table offsets (H at cells, E at nodes)
  u32 portCount, maxSteps; // lumped ports, and the capacity of the per-submit waveform/log tables
};

#ifdef __cplusplus
static_assert(sizeof(StepParams) == 104, "push constant layout must match the shader");
} // namespace fdtd_gpu
#endif
#endif
