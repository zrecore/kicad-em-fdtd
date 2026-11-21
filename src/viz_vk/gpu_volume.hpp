// GPU volume renderer: phasor reconstruction, empty-brick flags and the ray march as compute passes
// (shaders/volume_render.comp), reading the result back as RGB8 for the window's CPU compositor.
#pragma once
#include <memory>
#include <string>

#include "viz/volume_renderer.hpp"
#include "vkcore/context.hpp"

namespace viz {

// Throws if the pipeline cannot be built (the caller falls back to the CPU renderer).
std::unique_ptr<VolumeRenderer> makeGpuVolumeRenderer(vkcore::Context& ctx, const DisplayVolume& dv);

// The GPU renderer when `ctx` is given and works, else the CPU one. `fellBack` (optional) receives the reason
// a given context was not used.
std::unique_ptr<VolumeRenderer> makeVolumeRenderer(vkcore::Context* ctx, const DisplayVolume& dv,
                                                   std::string* fellBack = nullptr);

} // namespace viz
