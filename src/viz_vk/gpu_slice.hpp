// GPU slice renderer: the per-pixel shading of renderFrame (src/viz/slice.cpp) as a compute pass. The pixel
// to node mapping (SliceMap) stays on the CPU; the result is read back as RGB8 for the window's compositor.
#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "viz/slice.hpp"
#include "vkcore/context.hpp"

namespace viz {

class GpuSliceRenderer {
public:
  // A plane of nu x nv nodes. Throws if the pipeline cannot be built (the caller keeps the CPU renderer).
  GpuSliceRenderer(vkcore::Context& ctx, std::size_t nu, std::size_t nv);
  ~GpuSliceRenderer();
  GpuSliceRenderer(const GpuSliceRenderer&) = delete;
  GpuSliceRenderer& operator=(const GpuSliceRenderer&) = delete;

  // One frequency's phasors over the plane nodes (u fastest). `mask` (nu*nv bytes, may be empty) marks
  // copper.
  void setPhasors(std::span<const float> re, std::span<const float> im,
                  const std::vector<std::uint8_t>& mask);
  // RGB8, map.width x map.height, top row first: the same picture as renderFrame for the same inputs.
  std::vector<std::uint8_t> render(const SliceMap& map, const FrameSpec& spec, double phi);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace viz
