// BDD scenarios for choosing the volume renderer: the GPU one is optional, the CPU one always works.
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "viz_vk/gpu_volume.hpp"

SCENARIO("With no GPU context the volume view uses the CPU renderer and draws the same picture") {
  GIVEN("a small field as phasors and no Vulkan context") {
    viz::DisplayVolume dv;
    dv.dims = {16, 12, 10};
    dv.extent = {0.016, 0.012, 0.010};
    std::vector<float> re(dv.voxels(), 0.f), im(dv.voxels(), 0.f);
    re[dv.index(8, 6, 5)] = 1.f;
    im[dv.index(8, 6, 5)] = 0.5f;
    WHEN("a renderer is asked for") {
      std::string why;
      auto r = viz::makeVolumeRenderer(nullptr, dv, &why);
      THEN("it is the CPU renderer, and a frame equals renderVolume on Re{X exp(j phase)} exactly") {
        REQUIRE(r->name() == "CPU");
        r->setPhasors(re, im);
        viz::RenderSpec rs;
        rs.width = 64;
        rs.height = 48;
        rs.colourScale = 1.0;
        const double phase = 0.4;
        std::vector<float> field(dv.voxels());
        for (std::size_t v = 0; v < field.size(); ++v)
          field[v] = static_cast<float>(re[v] * std::cos(phase) - im[v] * std::sin(phase));
        viz::Camera cam;
        REQUIRE(r->render(cam, rs, phase, false) == viz::renderVolume(dv, field, cam, rs));
      }
    }
  }
}

SCENARIO("A colour-map volume view animates with the phase, and its peak-magnitude view does not") {
  GIVEN("a small field as phasors, a colour table, and the CPU renderer") {
    viz::DisplayVolume dv;
    dv.dims = {16, 12, 10};
    dv.extent = {0.016, 0.012, 0.010};
    std::vector<float> re(dv.voxels(), 0.f), im(dv.voxels(), 0.f);
    for (std::size_t i = 6; i < 10; ++i) {
      re[dv.index(i, 6, 5)] = 1.f;
      im[dv.index(i, 6, 5)] = 0.3f;
    }
    auto r = viz::makeVolumeRenderer(nullptr, dv, nullptr);
    r->setPhasors(re, im);
    viz::RenderSpec rs;
    rs.width = 64;
    rs.height = 48;
    rs.colourScale = 1.0;
    rs.lut = {{10, 0, 60}, {120, 40, 140}, {250, 230, 40}};
    viz::Camera cam;
    WHEN("it is drawn at two phases") {
      THEN("the instantaneous colour-map view differs between them, and the peak-magnitude view does not") {
        REQUIRE(r->render(cam, rs, 0.0, false) != r->render(cam, rs, 1.0, false));
        REQUIRE(r->render(cam, rs, 0.0, true) == r->render(cam, rs, 1.0, true));
      }
    }
  }
}
