#include "viz/volume_renderer.hpp"

#include <cmath>

namespace viz {
namespace {

class CpuVolumeRenderer final : public VolumeRenderer {
public:
  explicit CpuVolumeRenderer(const DisplayVolume& dv) : dv_(dv) {}
  std::string name() const override { return "CPU"; }
  void setPhasors(std::span<const float> re, std::span<const float> im) override {
    re_.assign(re.begin(), re.end());
    im_.assign(im.begin(), im.end());
  }
  std::vector<std::uint8_t> render(const Camera& cam, const RenderSpec& spec, double phase,
                                   bool magnitude) override {
    const double c = std::cos(phase), s = std::sin(phase);
    field_.resize(re_.size());
    for (std::size_t v = 0; v < field_.size(); ++v)
      field_[v] = magnitude ? std::hypot(re_[v], im_[v]) : static_cast<float>(re_[v] * c - im_[v] * s);
    return renderVolume(dv_, field_, cam, spec);
  }

private:
  DisplayVolume dv_;
  std::vector<float> re_, im_, field_;
};

} // namespace

std::unique_ptr<VolumeRenderer> makeCpuVolumeRenderer(const DisplayVolume& dv) {
  return std::make_unique<CpuVolumeRenderer>(dv);
}

} // namespace viz
