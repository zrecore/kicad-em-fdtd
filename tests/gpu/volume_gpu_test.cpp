// BDD scenarios for the GPU volume renderer: it must draw the CPU ray marcher's picture (src/viz/volume.cpp
// is the reference), from phasors, in every colour mode.
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "viz_vk/gpu_volume.hpp"

namespace {

#ifdef FDTD_VALIDATION
constexpr bool kValidation = true;
#else
constexpr bool kValidation = false;
#endif

// Two Gaussian lobes of opposite sign with phase structure, and empty space around them, so the empty-brick
// skip, both signs, the dB scale and the phase all matter.
struct Scene {
  viz::DisplayVolume dv;
  std::vector<float> re, im;
};
Scene makeScene() {
  Scene s;
  s.dv.dims = {48, 40, 32};
  s.dv.origin = {0, 0, 0};
  s.dv.extent = {0.048, 0.040, 0.032};
  s.re.assign(s.dv.voxels(), 0.f);
  s.im.assign(s.dv.voxels(), 0.f);
  for (std::size_t k = 0; k < 32; ++k)
    for (std::size_t j = 0; j < 40; ++j)
      for (std::size_t i = 0; i < 48; ++i) {
        auto lobe = [&](double cx, double cy, double cz, double w) {
          const double dx = i - cx, dy = j - cy, dz = k - cz;
          return std::exp(-(dx * dx + dy * dy + dz * dz) / (2 * w * w));
        };
        const std::size_t v = s.dv.index(i, j, k);
        s.re[v] = static_cast<float>(lobe(14, 18, 14, 4.0) - 0.7 * lobe(34, 22, 18, 3.0));
        s.im[v] = static_cast<float>(0.5 * lobe(24, 12, 10, 5.0));
      }
  return s;
}

struct Diff {
  double mean = 0;     // mean absolute channel difference, in 8-bit levels
  double fraction = 0; // share of pixels with any channel off by more than 8 levels
  double lit = 0;      // share of pixels that differ from the background in the CPU image
};
Diff compare(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
  REQUIRE(a.size() == b.size());
  Diff d;
  std::size_t bad = 0, lit = 0;
  for (std::size_t p = 0; p < a.size() / 3; ++p) {
    int worst = 0;
    for (int c = 0; c < 3; ++c) {
      const int x = a[3 * p + c], y = b[3 * p + c];
      d.mean += std::abs(x - y);
      worst = std::max(worst, std::abs(x - y));
    }
    bad += worst > 8;
    lit += (a[3 * p] != a[0] || a[3 * p + 1] != a[1] || a[3 * p + 2] != a[2]);
  }
  d.mean /= static_cast<double>(a.size());
  d.fraction = static_cast<double>(bad) / static_cast<double>(a.size() / 3);
  d.lit = static_cast<double>(lit) / static_cast<double>(a.size() / 3);
  return d;
}

viz::RenderSpec spec() {
  viz::RenderSpec rs;
  rs.width = 320;
  rs.height = 240;
  rs.colourScale = 0.8;
  rs.logRangeDb = 28;
  rs.threshold = 0.08;
  rs.emission = 1.6;
  return rs;
}

} // namespace

SCENARIO("The GPU volume renderer draws the CPU ray marcher's picture", "[gpu][volume]") {
  GIVEN("two lobes of opposite sign in an empty box, as phasors, and a camera looking at them") {
    vkcore::ContextOptions co;
    co.validation = kValidation;
    vkcore::Context ctx(co);
    const Scene s = makeScene();
    auto cpu = viz::makeCpuVolumeRenderer(s.dv);
    auto gpu = viz::makeGpuVolumeRenderer(ctx, s.dv);
    cpu->setPhasors(s.re, s.im);
    gpu->setPhasors(s.re, s.im);
    viz::Camera cam;
    cam.azimuth = 0.9;
    cam.elevation = 0.5;
    cam.distance = 2.2;
    struct Mode {
      const char* name;
      bool light, magnitude, lut;
    };
    for (const Mode m :
         {Mode{"signed on dark", false, false, false}, Mode{"signed on light", true, false, false},
          Mode{"magnitude through a colour table", false, true, true}}) {
      WHEN(std::string("the frame is drawn as ") + m.name + " at phase 0.7 rad") {
        auto rs = spec();
        rs.lightTheme = m.light;
        if (m.lut)
          for (int i = 0; i < 16; ++i)
            rs.lut.push_back({static_cast<std::uint8_t>(15 * i), static_cast<std::uint8_t>(255 - 12 * i),
                              static_cast<std::uint8_t>(40 + 10 * i)});
        const auto a = cpu->render(cam, rs, 0.7, m.magnitude);
        const auto b = gpu->render(cam, rs, 0.7, m.magnitude);
        const Diff d = compare(a, b);
        INFO("mean " << d.mean << " levels, " << 100 * d.fraction << " % of pixels off by more than 8, "
                     << 100 * d.lit << " % lit");
        THEN("the picture has structure, and the two renderers agree: mean channel difference below 0.5 "
             "levels of 255 and under 0.5 % of pixels off by more than 8 levels") {
          REQUIRE(d.lit > 0.05); // not two empty frames agreeing
          REQUIRE(d.mean < 0.5);
          REQUIRE(d.fraction < 0.005);
        }
      }
    }
    WHEN("the phase moves by half a period") {
      auto rs = spec();
      const auto a = gpu->render(cam, rs, 0.3, false);
      const auto b = gpu->render(cam, rs, 0.3 + 3.14159265, false);
      THEN("the signed picture changes (the two lobes swap sign), so the phase is applied") {
        REQUIRE(compare(a, b).mean > 1.0);
      }
    }
    WHEN("the field is empty") {
      const std::vector<float> zero(s.dv.voxels(), 0.f);
      gpu->setPhasors(zero, zero);
      const auto img = gpu->render(cam, spec(), 0.0, false);
      THEN("every pixel is the background colour, exactly") {
        for (std::size_t p = 0; p < img.size() / 3; ++p) {
          REQUIRE(img[3 * p] == viz::kBackground.r);
          REQUIRE(img[3 * p + 1] == viz::kBackground.g);
          REQUIRE(img[3 * p + 2] == viz::kBackground.b);
        }
      }
    }
  }
}
