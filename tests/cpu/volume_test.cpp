// BDD scenarios for the 3D volume view: display grid mapping, frame reconstruction and the ray marcher.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "viz/volume.hpp"

using Catch::Approx;

namespace {
// A volume of the given dims filled with `value` inside the cube [lo, hi) of voxel indices, else 0.
std::vector<float> cube(const viz::DisplayVolume& dv, int lo, int hi, float value) {
  std::vector<float> v(dv.voxels(), 0.f);
  for (std::size_t k = 0; k < dv.dims[2]; ++k)
    for (std::size_t j = 0; j < dv.dims[1]; ++j)
      for (std::size_t i = 0; i < dv.dims[0]; ++i)
        if (static_cast<int>(i) >= lo && static_cast<int>(i) < hi && static_cast<int>(j) >= lo &&
            static_cast<int>(j) < hi && static_cast<int>(k) >= lo && static_cast<int>(k) < hi)
          v[dv.index(i, j, k)] = value;
  return v;
}
viz::Rgb pixel(const std::vector<std::uint8_t>& rgb, std::uint32_t w, std::uint32_t x, std::uint32_t y) {
  const auto* p = &rgb[(static_cast<std::size_t>(y) * w + x) * 3];
  return {p[0], p[1], p[2]};
}
} // namespace

SCENARIO("The display volume samples the solver grid at the nearest node, in a chosen box") {
  GIVEN("a graded grid whose cells grow along x") {
    std::array<std::vector<double>, 3> edges;
    double x = 0;
    edges[0].push_back(x);
    for (int i = 0; i < 20; ++i)
      edges[0].push_back(x += 1e-3 * (1 + 0.1 * i)); // cells 1.0, 1.1, 1.2 ... mm
    for (int a = 1; a < 3; ++a)
      for (int i = 0; i <= 10; ++i)
        edges[a].push_back(1e-3 * i);
    const fdtd::Grid g(edges);
    WHEN("a display volume with at most 16 voxels along the longest axis covers the whole grid") {
      const auto dv = viz::makeDisplayVolume(g, 16, {});
      THEN("the longest axis has 16 voxels, the others are scaled to keep the aspect") {
        REQUIRE(dv.dims[0] == 16);
        REQUIRE(dv.dims[1] < 16);
        REQUIRE(dv.dims[1] == Approx(16.0 * g.length(fdtd::Y) / g.length(fdtd::X)).margin(1.0));
      }
      THEN("each voxel maps to the solver node nearest its centre") {
        for (std::size_t i = 0; i < dv.dims[0]; ++i) {
          const double centre =
              (static_cast<double>(i) + 0.5) / static_cast<double>(dv.dims[0]) * g.length(fdtd::X);
          const std::size_t n = dv.nodeOf[0][i];
          for (std::size_t m = 0; m < g.nodes(fdtd::X); ++m)
            REQUIRE(std::abs(g.edge(fdtd::X, n) - centre) <= std::abs(g.edge(fdtd::X, m) - centre) + 1e-12);
        }
      }
    }
    WHEN("the box is restricted to x from 5 to 10 mm") {
      viz::VolumeBox box;
      box.lo = {5e-3, 0, 0};
      box.hi = {10e-3, 10e-3, 10e-3};
      const auto dv = viz::makeDisplayVolume(g, 16, box);
      THEN("every sampled node lies inside the box (to within one cell) and extents match the box") {
        REQUIRE(dv.extent[0] == Approx(5e-3));
        for (std::size_t n : dv.nodeOf[0])
          REQUIRE(g.edge(fdtd::X, n) >= 5e-3 - 1.6e-3);
      }
    }
  }
}

SCENARIO("A volume frame is reconstructed from accumulated phasors") {
  GIVEN("a phasor accumulator over a 4 x 4 x 4 volume holding one frequency") {
    const auto grid = fdtd::Grid::uniform({4, 4, 4}, {4, 4, 4});
    const auto dv = viz::makeDisplayVolume(grid, 4, {});
    viz::PhasorAccumulator acc({1e9}, dv.voxels());
    std::vector<float> ones(dv.voxels(), 1.f);
    acc.add(0.0, 1.0, ones); // X = 1 + 0j everywhere
    THEN("phase 0 gives +1 everywhere and phase pi gives -1") {
      std::vector<float> out;
      viz::reconstruct(acc, 0, 0.0, out);
      REQUIRE(out.size() == dv.voxels());
      REQUIRE(out[5] == Approx(1.0f));
      viz::reconstruct(acc, 0, 3.14159265358979, out);
      REQUIRE(out[5] == Approx(-1.0f).margin(1e-5));
    }
  }
}

SCENARIO("The ray marcher draws signed fields with the right colours, opacity and orientation") {
  const auto grid = fdtd::Grid::uniform({16, 16, 16}, {16, 16, 16});
  const auto dv = viz::makeDisplayVolume(grid, 16, {});
  viz::RenderSpec spec;
  spec.width = 64;
  spec.height = 64;
  spec.colourScale = 1.0;
  spec.density = 6.0;
  viz::Camera cam; // default view looks at the centre
  GIVEN("an empty volume") {
    const std::vector<float> vol(dv.voxels(), 0.f);
    THEN("every pixel is the background") {
      const auto img = viz::renderVolume(dv, vol, cam, spec);
      REQUIRE(pixel(img, 64, 32, 32).r == viz::kBackground.r);
      REQUIRE(pixel(img, 64, 5, 5).b == viz::kBackground.b);
    }
  }
  GIVEN("a positive block in the middle") {
    const auto vol = cube(dv, 4, 12, 1.0f);
    const auto img = viz::renderVolume(dv, vol, cam, spec);
    THEN("the centre is warm (red above blue) and the corner is background") {
      const auto c = pixel(img, 64, 32, 32);
      REQUIRE(c.r > c.b);
      REQUIRE(c.r > viz::kBackground.r + 40);
      REQUIRE(pixel(img, 64, 1, 1).r == viz::kBackground.r);
    }
    AND_WHEN("the sign is flipped") {
      const auto neg = cube(dv, 4, 12, -1.0f);
      const auto c = pixel(viz::renderVolume(dv, neg, cam, spec), 64, 32, 32);
      THEN("the centre is cool (blue above red)") {
        REQUIRE(c.b > c.r);
      }
    }
    AND_WHEN("a thicker block is drawn with a thin, translucent setting (density 0.8)") {
      auto lowDensity = spec;
      lowDensity.density = 0.8;
      const auto thin = pixel(viz::renderVolume(dv, vol, cam, lowDensity), 64, 32, 32);
      const auto thick = pixel(viz::renderVolume(dv, cube(dv, 2, 14, 1.0f), cam, lowDensity), 64, 32, 32);
      THEN("it is more opaque, so further from the background colour") {
        REQUIRE(thick.r > thin.r);
      }
    }
    AND_WHEN("empty-space skipping is turned off") {
      auto noSkip = spec;
      noSkip.skipEmpty = false;
      const auto plain = viz::renderVolume(dv, vol, cam, noSkip);
      THEN("the image is the same within 3 levels per channel") {
        for (std::size_t i = 0; i < img.size(); ++i)
          REQUIRE(std::abs(int(img[i]) - int(plain[i])) <= 3);
      }
    }
  }
  GIVEN("a block off to one side of the volume") {
    std::vector<float> vol(dv.voxels(), 0.f);
    for (std::size_t k = 4; k < 12; ++k)
      for (std::size_t j = 4; j < 12; ++j)
        for (std::size_t i = 10; i < 15; ++i)
          vol[dv.index(i, j, k)] = 1.f;
    THEN("looking from the default camera the block appears on one side only, and rotating by 180 degrees "
         "moves it to the other") {
      const auto a = viz::renderVolume(dv, vol, cam, spec);
      viz::Camera back = cam;
      back.azimuth += 3.14159265358979;
      const auto b = viz::renderVolume(dv, vol, back, spec);
      auto warm = [&](const std::vector<std::uint8_t>& img, bool left) {
        int n = 0;
        for (std::uint32_t y = 0; y < 64; ++y)
          for (std::uint32_t x = left ? 0u : 32u; x < (left ? 32u : 64u); ++x)
            n += pixel(img, 64, x, y).r > viz::kBackground.r + 40;
        return n;
      };
      const bool aLeft = warm(a, true) > warm(a, false);
      const bool bLeft = warm(b, true) > warm(b, false);
      REQUIRE(aLeft != bLeft);
    }
  }
}

SCENARIO("A dB scale makes weak fields visible that a linear scale hides") {
  const auto grid = fdtd::Grid::uniform({16, 16, 16}, {16, 16, 16});
  const auto dv = viz::makeDisplayVolume(grid, 16, {});
  GIVEN("a block whose field is 30 dB below the colour scale") {
    const auto vol = cube(dv, 4, 12, 0.0316f);
    viz::RenderSpec linear;
    linear.width = linear.height = 64;
    linear.colourScale = 1.0;
    linear.density = 8;
    auto db = linear;
    db.logRangeDb = 40;
    THEN("the linear render shows (almost) nothing and the dB render shows the block clearly") {
      const auto a = pixel(viz::renderVolume(dv, vol, viz::Camera{}, linear), 64, 32, 32);
      const auto b = pixel(viz::renderVolume(dv, vol, viz::Camera{}, db), 64, 32, 32);
      REQUIRE(a.r < viz::kBackground.r + 15);
      REQUIRE(b.r > viz::kBackground.r + 60);
    }
    THEN("skipping empty space gives the same dB image within 3 levels") {
      auto skip = db;
      skip.skipEmpty = true;
      auto noSkip = db;
      noSkip.skipEmpty = false;
      const auto x = viz::renderVolume(dv, vol, viz::Camera{}, skip),
                 y = viz::renderVolume(dv, vol, viz::Camera{}, noSkip);
      for (std::size_t i = 0; i < x.size(); ++i)
        REQUIRE(std::abs(int(x[i]) - int(y[i])) <= 3);
    }
  }
}

SCENARIO("The camera can fit itself to where the field is active") {
  const auto grid = fdtd::Grid::uniform({32, 32, 32}, {32, 32, 32});
  const auto dv = viz::makeDisplayVolume(grid, 32, {});
  auto block = [&](int lo, int hi) {
    std::vector<float> v(dv.voxels(), 0.f);
    for (int k = lo; k < hi; ++k)
      for (int j = lo; j < hi; ++j)
        for (int i = lo; i < hi; ++i)
          v[dv.index(i, j, k)] = 1.f;
    return v;
  };
  GIVEN("activity confined to a small block in one corner and, separately, filling the whole volume") {
    viz::Camera small, large;
    viz::fitCamera(dv, block(2, 8), 1.0, 0, small);
    viz::fitCamera(dv, block(0, 32), 1.0, 0, large);
    THEN("the camera targets the corner block (off-centre, towards negative coordinates)") {
      REQUIRE(small.target[0] < -0.25);
      REQUIRE(small.target[1] < -0.25);
      REQUIRE(small.target[2] < -0.25);
    }
    THEN("it moves in much closer for the small block than for the whole volume") {
      REQUIRE(small.distance < 0.6 * large.distance);
    }
    THEN("the whole-volume fit stays centred") {
      REQUIRE(std::abs(large.target[0]) < 0.02);
    }
  }
  GIVEN("a volume with no activity") {
    viz::Camera cam;
    cam.distance = 2.0;
    viz::fitCamera(dv, std::vector<float>(dv.voxels(), 0.f), 1.0, 0, cam);
    THEN("the camera is left as it was") {
      REQUIRE(cam.distance == 2.0);
    }
  }
}

SCENARIO("The volume can be drawn on a light background with the light theme's colours") {
  const auto grid = fdtd::Grid::uniform({16, 16, 16}, {16, 16, 16});
  const auto dv = viz::makeDisplayVolume(grid, 16, {});
  viz::RenderSpec spec;
  spec.width = spec.height = 64;
  spec.colourScale = 1.0;
  spec.density = 6;
  spec.lightTheme = true;
  spec.negative = viz::kLightNegative;
  spec.positive = viz::kLightPositive;
  GIVEN("an empty volume") {
    THEN("every pixel is the light background") {
      const auto img = viz::renderVolume(dv, std::vector<float>(dv.voxels(), 0.f), viz::Camera{}, spec);
      REQUIRE(pixel(img, 64, 32, 32).r > 220);
      REQUIRE(pixel(img, 64, 3, 3).b > 220);
    }
  }
  GIVEN("a positive block and, separately, a negative block") {
    const auto pos = pixel(viz::renderVolume(dv, cube(dv, 4, 12, 1.0f), viz::Camera{}, spec), 64, 32, 32);
    const auto neg = pixel(viz::renderVolume(dv, cube(dv, 4, 12, -1.0f), viz::Camera{}, spec), 64, 32, 32);
    THEN("positive is red-dominated and negative blue-dominated, both clearly darker than the background") {
      REQUIRE(pos.r > pos.b + 40);
      REQUIRE(pos.g < 140);
      REQUIRE(neg.b > neg.r + 40);
      REQUIRE(neg.r < 140);
    }
  }
}

SCENARIO("Panning the 3D camera moves the picture with the pointer and keeps the angle and zoom") {
  const auto grid = fdtd::Grid::uniform({16, 16, 16}, {16, 16, 16});
  const auto dv = viz::makeDisplayVolume(grid, 16, {});
  const std::array<double, 3> centre{dv.origin[0] + 0.5 * dv.extent[0], dv.origin[1] + 0.5 * dv.extent[1],
                                     dv.origin[2] + 0.5 * dv.extent[2]};
  GIVEN("a camera looking at the centre of the box in a 100 x 100 pixel view") {
    viz::Camera cam;
    cam.azimuth = 1.1;
    cam.elevation = 0.4;
    float x0, y0;
    REQUIRE(viz::projectPoint(dv, cam, 100, 100, centre, x0, y0));
    REQUIRE(x0 == Catch::Approx(50.0f).margin(0.01f));
    REQUIRE(y0 == Catch::Approx(50.0f).margin(0.01f));
    WHEN("it is dragged 40 pixels right and 20 pixels up") {
      viz::Camera dragged = cam;
      viz::panCamera(dv, dragged, 40, -20, 100);
      float x1, y1;
      REQUIRE(viz::projectPoint(dv, dragged, 100, 100, centre, x1, y1));
      THEN("the scene follows the pointer by exactly that much") {
        REQUIRE(x1 == Catch::Approx(90.0f).margin(0.05f));
        REQUIRE(y1 == Catch::Approx(30.0f).margin(0.05f));
      }
      THEN("the angle and the distance are unchanged") {
        REQUIRE(dragged.azimuth == cam.azimuth);
        REQUIRE(dragged.elevation == cam.elevation);
        REQUIRE(dragged.distance == cam.distance);
      }
    }
    WHEN("it is panned and the image is rendered") {
      std::vector<float> vol(dv.voxels(), 0.f);
      for (std::size_t k = 6; k < 10; ++k)
        for (std::size_t j = 6; j < 10; ++j)
          for (std::size_t i = 6; i < 10; ++i)
            vol[dv.index(i, j, k)] = 1.f;
      viz::RenderSpec spec;
      spec.width = spec.height = 100;
      spec.colourScale = 1.0;
      spec.density = 6.0;
      viz::Camera dragged = cam;
      viz::panCamera(dv, dragged, 30, 0, 100);
      auto centroidX = [&](const viz::Camera& c) {
        const auto img = viz::renderVolume(dv, vol, c, spec);
        double sum = 0, n = 0;
        for (std::uint32_t y = 0; y < 100; ++y)
          for (std::uint32_t x = 0; x < 100; ++x)
            if (pixel(img, 100, x, y).r > viz::kBackground.r + 30) {
              sum += x;
              ++n;
            }
        return n > 0 ? sum / n : -1.0;
      };
      THEN("the rendered block moves right by about the dragged distance") {
        REQUIRE(centroidX(dragged) - centroidX(cam) == Catch::Approx(30.0).margin(2.0));
      }
    }
  }
}
