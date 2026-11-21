// The 3D volume renderer behind one interface: the CPU ray marcher (always available, the reference) or the
// GPU compute renderer (src/viz_vk). Both take one frequency's phasors over the display voxels, so a frame
// is a camera and a phase, and both draw the same picture within the tolerance stated in the tests.
#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "viz/volume.hpp"

namespace viz {

class VolumeRenderer {
public:
  virtual ~VolumeRenderer() = default;
  virtual std::string name() const = 0; // "CPU" or "GPU (device)", for the status line
  // The phasors X of one frequency at every display voxel (display-voxel order, DisplayVolume::index).
  virtual void setPhasors(std::span<const float> re, std::span<const float> im) = 0;
  // RGB8, spec.width x spec.height, top row first. The field drawn is Re{X exp(j phase)}, or |X| when
  // `magnitude` (the colour-table view, which has no sign and does not animate).
  virtual std::vector<std::uint8_t> render(const Camera& cam, const RenderSpec& spec, double phase,
                                           bool magnitude) = 0;
};

std::unique_ptr<VolumeRenderer> makeCpuVolumeRenderer(const DisplayVolume& dv);

} // namespace viz
