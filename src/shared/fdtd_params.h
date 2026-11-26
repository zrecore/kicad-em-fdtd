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

// Phasor accumulator (shaders/phasor_accum.comp): sample channels (a field component at a node) and N
// frequencies.
struct AccumParams {
  u32 channels, freqs;
};

// Volume renderer (shaders/volume_field.comp, shaders/volume_render.comp): everything one frame needs, in a
// storage buffer (it is larger than the 128 bytes of push constants Vulkan guarantees).
#define VOL_LIGHT 1u /* light theme */
#define VOL_LUT 2u   /* colour by |value| through the table */
#define VOL_SKIP 4u  /* skip empty bricks */
#define VOL_MAG 8u   /* magnitude, not Re{X exp(j phase)} */
#define VOL_BRICK 4u /* brick edge in voxels */
#ifdef __cplusplus
struct alignas(16) vec4 {
  float x, y, z, w;
};
#endif
struct VolParams {
  vec4 pos, forward, right, up, size;  // camera frame and box size (longest side 1)
  vec4 negative, positive, background; // colours, 0..255
  u32 nx, ny, nz, width;
  u32 height, flags, lutSize, pad0;
  float cosPhase, sinPhase, brickThreshold, colourScale;
  float density, gamma, threshold, logRangeDb;
  float emission, step, voxel, tanY;
  float aspect, pad1, pad2, pad3;
};

// 2D cross-section solve, one red-black half-sweep (shaders/line2d_rb.comp).
struct Line2dParams {
  u32 nx, ny, colour;
  float omega;
};

// Slice renderer (shaders/slice_render.comp).
#define SLICE_LIGHT 1u
#define SLICE_MASK 2u
struct SliceParams {
  vec4 negative, positive;
  u32 width, height, nu, nv;
  u32 flags, pad0, pad1, pad2;
  float cosPhase, sinPhase, colourScale, gamma;
};

#ifdef __cplusplus
static_assert(sizeof(SliceParams) == 80, "storage buffer layout must match the shader");
static_assert(sizeof(VolParams) == 224, "storage buffer layout must match the shader");
static_assert(sizeof(StepParams) == 104, "push constant layout must match the shader");
static_assert(sizeof(Line2dParams) == 16, "push constant layout must match the shader");
static_assert(sizeof(AccumParams) == 8, "push constant layout must match the shader");
} // namespace fdtd_gpu
#endif
#endif
