#include "viz_vk/gpu_volume.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "core/resources.hpp"
#include "shared/fdtd_params.h"
#include "viz/volume_view.hpp"
#include "vkcore/buffer.hpp"
#include "vkcore/compute.hpp"

namespace viz {
namespace {

constexpr std::size_t kMaxLut = 4096;

fdtd_gpu::vec4 v4(double x, double y, double z) {
  return {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), 0.f};
}

class GpuVolumeRenderer final : public VolumeRenderer {
public:
  GpuVolumeRenderer(vkcore::Context& ctx, const DisplayVolume& dv)
      : ctx_(ctx), dv_(dv), fieldPipe_(ctx, shader("volume_field"), 5, sizeof(std::uint32_t)),
        renderPipe_(ctx, shader("volume_render"), 5, sizeof(std::uint32_t)),
        params_(ctx, sizeof(fdtd_gpu::VolParams)), re_(ctx, dv.voxels() * sizeof(float)),
        im_(ctx, dv.voxels() * sizeof(float)), field_(ctx, dv.voxels() * sizeof(float)),
        flags_(ctx, brickCount(dv) * sizeof(std::uint32_t)), lut_(ctx, kMaxLut * 4 * sizeof(float)) {
    if (dv.dims[0] < 2 || dv.dims[1] < 2 || dv.dims[2] < 2)
      throw std::invalid_argument("the display volume needs at least 2 voxels per axis");
  }

  std::string name() const override { return std::string("GPU (") + ctx_.properties().deviceName + ")"; }

  void setPhasors(std::span<const float> re, std::span<const float> im) override {
    if (re.size() != dv_.voxels() || im.size() != dv_.voxels())
      throw std::invalid_argument("phasor size does not match the display volume");
    std::memcpy(re_.as<float>().data(), re.data(), re.size_bytes());
    std::memcpy(im_.as<float>().data(), im.data(), im.size_bytes());
  }

  std::vector<std::uint8_t> render(const Camera& cam, const RenderSpec& spec, double phase,
                                   bool magnitude) override {
    const View view = makeView(dv_, cam);
    const double tanY = std::tan(0.5 * cam.fovY);
    const std::size_t nx = dv_.dims[0], ny = dv_.dims[1], nz = dv_.dims[2];
    const double voxel = std::min({view.size.x / nx, view.size.y / ny, view.size.z / nz});
    const Rgb bg = spec.lightTheme ? Rgb{238, 238, 241} : kBackground;
    const std::size_t pixels = static_cast<std::size_t>(spec.width) * spec.height;
    if (!out_ || out_->size() < pixels * sizeof(std::uint32_t))
      out_ = std::make_unique<vkcore::Buffer>(ctx_, pixels * sizeof(std::uint32_t));

    fdtd_gpu::VolParams q{};
    q.pos = v4(view.pos.x, view.pos.y, view.pos.z);
    q.forward = v4(view.forward.x, view.forward.y, view.forward.z);
    q.right = v4(view.right.x, view.right.y, view.right.z);
    q.up = v4(view.up.x, view.up.y, view.up.z);
    q.size = v4(view.size.x, view.size.y, view.size.z);
    q.negative = v4(spec.negative.r, spec.negative.g, spec.negative.b);
    q.positive = v4(spec.positive.r, spec.positive.g, spec.positive.b);
    q.background = v4(bg.r, bg.g, bg.b);
    q.nx = static_cast<std::uint32_t>(nx);
    q.ny = static_cast<std::uint32_t>(ny);
    q.nz = static_cast<std::uint32_t>(nz);
    q.width = spec.width;
    q.height = spec.height;
    const bool useLut = spec.lut.size() >= 2;
    q.flags = (spec.lightTheme ? VOL_LIGHT : 0u) | (useLut ? VOL_LUT : 0u) |
              (spec.skipEmpty ? VOL_SKIP : 0u) | (magnitude ? VOL_MAG : 0u);
    q.lutSize = static_cast<std::uint32_t>(std::min(spec.lut.size(), kMaxLut));
    q.cosPhase = static_cast<float>(std::cos(phase));
    q.sinPhase = static_cast<float>(std::sin(phase));
    q.brickThreshold = static_cast<float>(
        spec.colourScale * (spec.logRangeDb > 0 ? std::pow(10.0, -spec.logRangeDb * (1 - spec.threshold) / 20)
                                                : spec.threshold));
    q.colourScale = static_cast<float>(spec.colourScale);
    q.density = static_cast<float>(spec.density);
    q.gamma = static_cast<float>(spec.gamma);
    q.threshold = static_cast<float>(spec.threshold);
    q.logRangeDb = static_cast<float>(spec.logRangeDb);
    q.emission = static_cast<float>(spec.emission);
    q.step = static_cast<float>(0.6 * voxel);
    q.voxel = static_cast<float>(voxel);
    q.tanY = static_cast<float>(tanY);
    q.aspect = static_cast<float>(static_cast<double>(spec.width) / spec.height);
    std::memcpy(params_.as<fdtd_gpu::VolParams>().data(), &q, sizeof q);

    auto lut = lut_.as<float>();
    for (std::size_t i = 0; i < q.lutSize && useLut; ++i) {
      lut[4 * i] = spec.lut[i].r;
      lut[4 * i + 1] = spec.lut[i].g;
      lut[4 * i + 2] = spec.lut[i].b;
      lut[4 * i + 3] = 0.f;
    }
    auto flags = flags_.as<std::uint32_t>();
    std::fill(flags.begin(), flags.end(), spec.skipEmpty ? 0u : 1u);

    const auto groups = [](std::size_t n) {
      return static_cast<std::uint32_t>(std::min<std::size_t>((n + 63) / 64, 65535));
    };
    std::uint32_t count = static_cast<std::uint32_t>(dv_.voxels());
    vkcore::Buffer* fieldBufs[] = {&params_, &re_, &im_, &field_, &flags_};
    fieldPipe_.run(fieldBufs, &count, groups(dv_.voxels()));
    count = static_cast<std::uint32_t>(pixels);
    vkcore::Buffer* renderBufs[] = {&params_, &field_, &flags_, &lut_, out_.get()};
    renderPipe_.run(renderBufs, &count, groups(pixels));

    std::vector<std::uint8_t> img(pixels * 3);
    const auto words = out_->as<std::uint32_t>();
    for (std::size_t p = 0; p < pixels; ++p) {
      img[3 * p] = static_cast<std::uint8_t>(words[p] & 0xff);
      img[3 * p + 1] = static_cast<std::uint8_t>((words[p] >> 8) & 0xff);
      img[3 * p + 2] = static_cast<std::uint8_t>((words[p] >> 16) & 0xff);
    }
    return img;
  }

private:
  static std::string shader(const char* name) {
    return core::resourceDir("shaders", FDTD_SHADER_DIR) + "/" + name + ".comp.spv";
  }
  static std::size_t brickCount(const DisplayVolume& dv) {
    constexpr std::size_t b = VOL_BRICK;
    return ((dv.dims[0] + b - 1) / b) * ((dv.dims[1] + b - 1) / b) * ((dv.dims[2] + b - 1) / b);
  }

  vkcore::Context& ctx_;
  DisplayVolume dv_;
  vkcore::ComputePipeline fieldPipe_, renderPipe_;
  vkcore::Buffer params_, re_, im_, field_, flags_, lut_;
  std::unique_ptr<vkcore::Buffer> out_;
};

} // namespace

std::unique_ptr<VolumeRenderer> makeGpuVolumeRenderer(vkcore::Context& ctx, const DisplayVolume& dv) {
  return std::make_unique<GpuVolumeRenderer>(ctx, dv);
}

std::unique_ptr<VolumeRenderer> makeVolumeRenderer(vkcore::Context* ctx, const DisplayVolume& dv,
                                                   std::string* fellBack) {
  if (ctx) {
    try {
      return makeGpuVolumeRenderer(*ctx, dv);
    } catch (const std::exception& e) {
      if (fellBack)
        *fellBack = e.what();
    }
  }
  return makeCpuVolumeRenderer(dv);
}

} // namespace viz
