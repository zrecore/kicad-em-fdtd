// BDD scenarios for the threshold isosurface (docs/PLAN.md, Next steps 11h).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <sstream>

#include "viz/isosurface.hpp"

using Catch::Approx;

namespace {
viz::DisplayVolume cubeVolume(std::size_t n, double sizeMetres) {
  viz::DisplayVolume dv;
  dv.dims = {n, n, n};
  dv.origin = {0, 0, 0};
  dv.extent = {sizeMetres, sizeMetres, sizeMetres};
  return dv;
}
std::array<double, 3> centreOf(const viz::DisplayVolume& dv, std::size_t i, std::size_t j, std::size_t k) {
  return {(static_cast<double>(i) + 0.5) * dv.extent[0] / static_cast<double>(dv.dims[0]),
          (static_cast<double>(j) + 0.5) * dv.extent[1] / static_cast<double>(dv.dims[1]),
          (static_cast<double>(k) + 0.5) * dv.extent[2] / static_cast<double>(dv.dims[2])};
}
} // namespace

SCENARIO("A plane through a linear field is found exactly") {
  GIVEN("a 20 x 20 x 20 volume of 20 mm in which the field is z (in metres)") {
    const auto dv = cubeVolume(20, 20e-3);
    std::vector<float> f(dv.voxels());
    for (std::size_t k = 0; k < 20; ++k)
      for (std::size_t j = 0; j < 20; ++j)
        for (std::size_t i = 0; i < 20; ++i)
          f[dv.index(i, j, k)] = static_cast<float>(centreOf(dv, i, j, k)[2]);
    WHEN("the level is 7.3 mm") {
      const auto mesh = viz::isosurface(dv, f, 7.3e-3f);
      THEN("every vertex has z = 7.3 mm within 1e-8 m") {
        REQUIRE(!mesh.empty());
        for (const auto& t : mesh)
          for (const auto& v : t.v)
            REQUIRE(v[2] == Approx(7.3e-3).margin(1e-8));
      }
      AND_THEN("the area is the plane's area between the outermost voxel centres, 19 mm x 19 mm, within 1e-6 "
               "relative") {
        REQUIRE(viz::surfaceArea(mesh) == Approx(19e-3 * 19e-3).epsilon(1e-6));
      }
    }
    WHEN("the level is above every value") {
      THEN("there is no surface") {
        REQUIRE(viz::isosurface(dv, f, 1.0f).empty());
        REQUIRE(viz::isosurface(dv, f, -1.0f).empty());
      }
    }
  }
}

SCENARIO("A sphere is found with the right radius and area") {
  GIVEN("a 48 voxel volume of 48 mm in which the field is 10 mm minus the distance from the centre") {
    const auto dv = cubeVolume(48, 48e-3);
    std::vector<float> f(dv.voxels());
    for (std::size_t k = 0; k < 48; ++k)
      for (std::size_t j = 0; j < 48; ++j)
        for (std::size_t i = 0; i < 48; ++i) {
          const auto p = centreOf(dv, i, j, k);
          f[dv.index(i, j, k)] =
              static_cast<float>(10e-3 - std::sqrt(std::pow(p[0] - 24e-3, 2) + std::pow(p[1] - 24e-3, 2) +
                                                   std::pow(p[2] - 24e-3, 2)));
        }
    WHEN("the level is 0 (the sphere of radius 10 mm)") {
      const auto mesh = viz::isosurface(dv, f, 0.0f);
      THEN("every vertex is within 0.05 mm (a twentieth of a voxel) of the radius") {
        REQUIRE(!mesh.empty());
        double worst = 0;
        for (const auto& t : mesh)
          for (const auto& v : t.v)
            worst = std::max(worst, std::abs(std::sqrt(std::pow(v[0] - 24e-3, 2) + std::pow(v[1] - 24e-3, 2) +
                                                       std::pow(v[2] - 24e-3, 2)) -
                                             10e-3));
        INFO("worst radial error " << worst);
        REQUIRE(worst < 0.05e-3);
      }
      AND_THEN("the area is 4 pi r^2 within 2 %") {
        REQUIRE(viz::surfaceArea(mesh) == Approx(4 * std::numbers::pi * 1e-4).epsilon(0.02));
      }
    }
  }
}

SCENARIO("A mesh is written as Wavefront OBJ") {
  GIVEN("one triangle") {
    viz::Triangle t{{{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}}};
    std::ostringstream out;
    viz::writeObj(out, {t}, {10, 0, 0});
    THEN("it has three vertices, offset by the shift, and one face with indices 1 2 3") {
      const std::string s = out.str();
      REQUIRE(s.find("v 10 0 0\n") != std::string::npos);
      REQUIRE(s.find("v 11 0 0\n") != std::string::npos);
      REQUIRE(s.find("v 10 1 0\n") != std::string::npos);
      REQUIRE(s.find("f 1 2 3\n") != std::string::npos);
    }
  }
}
