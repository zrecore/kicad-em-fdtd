#include "viz_vk/gpu_slice.hpp"

#include <cmath>
#include <cstring>
#include <stdexcept>

#include "core/resources.hpp"
#include "shared/fdtd_params.h"
#include "vkcore/buffer.hpp"
#include "vkcore/compute.hpp"

namespace viz {

struct GpuSliceRenderer::Impl {
  Impl(vkcore::Context& c, std::size_t nu_, std::size_t nv_)
      : ctx(c), nu(nu_), nv(nv_),
        pipe(c, core::resourceDir("shaders", FDTD_SHADER_DIR) + "/slice_render.comp.spv", 6,
             sizeof(std::uint32_t)),
        params(c, sizeof(fdtd_gpu::SliceParams)), re(c, nu_ * nv_ * sizeof(float)),
        im(c, nu_ * nv_ * sizeof(float)), mask(c, nu_ * nv_ * sizeof(std::uint32_t)) {}
  vkcore::Context& ctx;
  std::size_t nu, nv;
  vkcore::ComputePipeline pipe;
  vkcore::Buffer params, re, im, mask;
  std::unique_ptr<vkcore::Buffer> tables, out;
  bool hasMask = false;
};

GpuSliceRenderer::GpuSliceRenderer(vkcore::Context& ctx, std::size_t nu, std::size_t nv)
    : impl_(std::make_unique<Impl>(ctx, nu, nv)) {
  if (nu < 2 || nv < 2)
    throw std::invalid_argument("a slice needs at least 2 nodes per side");
}
GpuSliceRenderer::~GpuSliceRenderer() = default;

void GpuSliceRenderer::setPhasors(std::span<const float> re, std::span<const float> im,
                                  const std::vector<std::uint8_t>& mask) {
  Impl& s = *impl_;
  if (re.size() != s.nu * s.nv || im.size() != re.size())
    throw std::invalid_argument("phasor size does not match the plane");
  std::memcpy(s.re.as<float>().data(), re.data(), re.size_bytes());
  std::memcpy(s.im.as<float>().data(), im.data(), im.size_bytes());
  s.hasMask = !mask.empty();
  auto m = s.mask.as<std::uint32_t>();
  for (std::size_t n = 0; n < m.size(); ++n)
    m[n] = s.hasMask ? mask[n] : 0u;
}

std::vector<std::uint8_t> GpuSliceRenderer::render(const SliceMap& map, const FrameSpec& spec, double phi) {
  Impl& s = *impl_;
  const std::size_t pixels = static_cast<std::size_t>(map.width) * map.height;
  if (pixels == 0)
    return {};
  const std::size_t tableBytes = (map.width + map.height) * 4 * sizeof(std::uint32_t);
  if (!s.tables || s.tables->size() < tableBytes)
    s.tables = std::make_unique<vkcore::Buffer>(s.ctx, tableBytes);
  if (!s.out || s.out->size() < pixels * sizeof(std::uint32_t))
    s.out = std::make_unique<vkcore::Buffer>(s.ctx, pixels * sizeof(std::uint32_t));

  auto tab = s.tables->as<std::uint32_t>();
  auto put = [&](std::size_t at, long node, std::size_t lo, double t) {
    tab[4 * at] = node < 0 ? 0xffffffffu : static_cast<std::uint32_t>(node);
    tab[4 * at + 1] = static_cast<std::uint32_t>(lo);
    const float tf = static_cast<float>(t);
    std::memcpy(&tab[4 * at + 2], &tf, sizeof tf);
    tab[4 * at + 3] = 0;
  };
  for (std::uint32_t x = 0; x < map.width; ++x)
    put(x, map.colNode[x], map.colLo[x], map.colT[x]);
  for (std::uint32_t y = 0; y < map.height; ++y)
    put(map.width + y, map.rowNode[y], map.rowLo[y], map.rowT[y]);

  fdtd_gpu::SliceParams q{};
  q.negative = {static_cast<float>(spec.negative.r), static_cast<float>(spec.negative.g),
                static_cast<float>(spec.negative.b), 0.f};
  q.positive = {static_cast<float>(spec.positive.r), static_cast<float>(spec.positive.g),
                static_cast<float>(spec.positive.b), 0.f};
  q.width = map.width;
  q.height = map.height;
  q.nu = static_cast<std::uint32_t>(s.nu);
  q.nv = static_cast<std::uint32_t>(s.nv);
  q.flags = (spec.lightTheme ? SLICE_LIGHT : 0u) | (s.hasMask ? SLICE_MASK : 0u);
  q.cosPhase = static_cast<float>(std::cos(phi));
  q.sinPhase = static_cast<float>(std::sin(phi));
  q.colourScale = static_cast<float>(spec.colourScale);
  q.gamma = static_cast<float>(spec.gamma);
  std::memcpy(s.params.as<fdtd_gpu::SliceParams>().data(), &q, sizeof q);

  vkcore::Buffer* bufs[] = {&s.params, &s.re, &s.im, &s.mask, s.tables.get(), s.out.get()};
  const std::uint32_t count = static_cast<std::uint32_t>(pixels);
  s.pipe.run(bufs, &count, static_cast<std::uint32_t>(std::min<std::size_t>((pixels + 63) / 64, 65535)));

  std::vector<std::uint8_t> img(pixels * 3);
  const auto words = s.out->as<std::uint32_t>();
  for (std::size_t p = 0; p < pixels; ++p) {
    img[3 * p] = static_cast<std::uint8_t>(words[p] & 0xff);
    img[3 * p + 1] = static_cast<std::uint8_t>((words[p] >> 8) & 0xff);
    img[3 * p + 2] = static_cast<std::uint8_t>((words[p] >> 16) & 0xff);
  }
  return img;
}

} // namespace viz
