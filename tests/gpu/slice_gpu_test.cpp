// BDD scenario for the GPU slice renderer: it must draw what renderFrame (the CPU reference) draws.
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "viz_vk/gpu_slice.hpp"

SCENARIO("The GPU slice renderer draws the CPU renderer's picture", "[gpu][slice]") {
  GIVEN("a plane of a 40 x 30 x 8 cell box with a standing pattern, a copper patch and a wider canvas") {
    vkcore::Context ctx;
    const auto g = fdtd::Grid::uniform({40, 30, 8}, {0.040, 0.030, 0.008});
    const viz::Plane plane = viz::makePlane(g, fdtd::Z, 4);
    std::vector<float> re(plane.nu * plane.nv), im(plane.nu * plane.nv);
    std::vector<std::uint8_t> mask(plane.nu * plane.nv, 0);
    for (std::size_t b = 0; b < plane.nv; ++b)
      for (std::size_t a = 0; a < plane.nu; ++a) {
        const std::size_t n = a + plane.nu * b;
        re[n] = static_cast<float>(std::sin(0.45 * static_cast<double>(a)) *
                                   std::cos(0.3 * static_cast<double>(b)));
        im[n] = static_cast<float>(0.4 * std::cos(0.2 * static_cast<double>(a + b)));
        mask[n] = a >= 5 && a < 12 && b >= 8 && b < 14;
      }
    viz::PhasorAccumulator acc({2.4e9}, plane.nu * plane.nv);
    acc.load(re, im);
    viz::GpuSliceRenderer gpu(ctx, plane.nu, plane.nv);
    gpu.setPhasors(acc.re(0), acc.im(0), mask);
    for (const bool light : {false, true}) {
      WHEN(std::string("a frame is drawn on a ") + (light ? "light" : "dark") + " panel at phase 0.9 rad") {
        viz::FrameSpec spec;
        spec.canvasWidth = 400;
        spec.canvasHeight = 360; // taller than the plane: a letterbox above and below
        spec.colourScale = 0.9;
        spec.gamma = 0.5;
        spec.lightTheme = light;
        std::uint32_t w = 0, h = 0;
        const auto cpu = viz::renderFrame(g, plane, acc, 0, 0.9, spec, mask, w, h);
        const auto map = viz::makeSliceMap(g, plane, spec);
        const auto img = gpu.render(map, spec, 0.9);
        THEN("the picture has structure, and the two agree: mean channel difference below 0.5 of 255 levels "
             "and under 0.5 % of pixels off by more than 8") {
          REQUIRE(map.width == w);
          REQUIRE(map.height == h);
          REQUIRE(img.size() == cpu.size());
          double mean = 0;
          std::size_t bad = 0, lit = 0;
          for (std::size_t p = 0; p < cpu.size() / 3; ++p) {
            int worst = 0;
            for (int c = 0; c < 3; ++c) {
              const int d = std::abs(int(cpu[3 * p + c]) - int(img[3 * p + c]));
              mean += d;
              worst = std::max(worst, d);
            }
            bad += worst > 8;
            lit += cpu[3 * p] != cpu[0] || cpu[3 * p + 1] != cpu[1] || cpu[3 * p + 2] != cpu[2];
          }
          mean /= static_cast<double>(cpu.size());
          INFO("mean " << mean << ", " << bad << " pixels off by more than 8, " << lit << " lit");
          REQUIRE(lit > cpu.size() / 3 / 4);
          REQUIRE(mean < 0.5);
          REQUIRE(static_cast<double>(bad) / static_cast<double>(cpu.size() / 3) < 0.005);
        }
      }
    }
  }
}
