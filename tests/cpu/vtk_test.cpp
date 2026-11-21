// BDD scenarios for the VTK export of the display volume (docs/PLAN.md, Next steps 11h).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <sstream>

#include "viz/vtk_export.hpp"

using Catch::Approx;

namespace {
struct Parsed {
  std::array<std::size_t, 3> dims{};
  std::array<double, 3> origin{}, spacing{};
  std::size_t points = 0;
  std::vector<std::pair<std::string, std::vector<double>>> scalars;
  std::string title, format, dataset;
};

Parsed parse(const std::string& text) {
  Parsed p;
  std::istringstream in(text);
  std::string line, word;
  std::getline(in, line); // header
  std::getline(in, p.title);
  std::getline(in, p.format);
  std::getline(in, p.dataset);
  while (in >> word) {
    if (word == "DIMENSIONS")
      in >> p.dims[0] >> p.dims[1] >> p.dims[2];
    else if (word == "ORIGIN")
      in >> p.origin[0] >> p.origin[1] >> p.origin[2];
    else if (word == "SPACING")
      in >> p.spacing[0] >> p.spacing[1] >> p.spacing[2];
    else if (word == "POINT_DATA")
      in >> p.points;
    else if (word == "SCALARS") {
      std::string name, type, table, tname;
      int comps;
      in >> name >> type >> comps >> table >> tname;
      std::vector<double> v(p.points);
      for (double& x : v)
        in >> x;
      p.scalars.push_back({name, std::move(v)});
    }
  }
  return p;
}
} // namespace

SCENARIO("The display volume is written as legacy VTK structured points that read back exactly") {
  GIVEN("a 6 x 4 x 3 volume over a 12 x 8 x 6 mm box that starts at (1, 2, 3) mm, holding x + 2y + 3z at "
        "every voxel centre") {
    viz::DisplayVolume dv;
    dv.dims = {6, 4, 3};
    dv.origin = {1e-3, 2e-3, 3e-3};
    dv.extent = {12e-3, 8e-3, 6e-3};
    std::vector<float> f(dv.voxels());
    for (std::size_t k = 0; k < 3; ++k)
      for (std::size_t j = 0; j < 4; ++j)
        for (std::size_t i = 0; i < 6; ++i) {
          const double x = dv.origin[0] + (static_cast<double>(i) + 0.5) * dv.extent[0] / 6,
                       y = dv.origin[1] + (static_cast<double>(j) + 0.5) * dv.extent[1] / 4,
                       z = dv.origin[2] + (static_cast<double>(k) + 0.5) * dv.extent[2] / 3;
          f[dv.index(i, j, k)] = static_cast<float>((x + 2 * y + 3 * z) * 1e3); // millimetres
        }
    std::vector<float> g(dv.voxels(), 0.25f);
    WHEN("it is written with a board offset of (10, 20, 0) mm and read back") {
      std::ostringstream out;
      viz::writeVtkVolume(out, dv, {{"Ez abs", f}, {"flat", g}}, "test volume\nsecond line",
                          {10e-3, 20e-3, 0});
      const Parsed p = parse(out.str());
      THEN("the header says ASCII structured points, one title line, 6 x 4 x 3 points and 72 values") {
        REQUIRE(p.format == "ASCII");
        REQUIRE(p.dataset == "DATASET STRUCTURED_POINTS");
        REQUIRE(p.title == "test volume");
        REQUIRE(p.dims == std::array<std::size_t, 3>{6, 4, 3});
        REQUIRE(p.points == 72);
      }
      AND_THEN("the spacing is the voxel size (2, 2, 2 mm) and the origin the first voxel centre plus the "
               "offset (12, 23, 4 mm), within 1e-12 m") {
        for (int a = 0; a < 3; ++a)
          REQUIRE(p.spacing[a] == Approx(2e-3).margin(1e-12));
        REQUIRE(p.origin[0] == Approx(12e-3).margin(1e-12));
        REQUIRE(p.origin[1] == Approx(23e-3).margin(1e-12));
        REQUIRE(p.origin[2] == Approx(4e-3).margin(1e-12));
      }
      AND_THEN("every value is the coordinate sum of the point it sits at, computed from the header only "
               "(grid x, y, z in mm, with the 10 and 20 mm offset removed): within 1e-5 relative") {
        REQUIRE(p.scalars.size() == 2);
        REQUIRE(p.scalars[0].first == "Ez_abs"); // the space became an underscore
        for (std::size_t k = 0; k < 3; ++k)
          for (std::size_t j = 0; j < 4; ++j)
            for (std::size_t i = 0; i < 6; ++i) {
              const double x = (p.origin[0] + static_cast<double>(i) * p.spacing[0] - 10e-3) * 1e3,
                           y = (p.origin[1] + static_cast<double>(j) * p.spacing[1] - 20e-3) * 1e3,
                           z = (p.origin[2] + static_cast<double>(k) * p.spacing[2]) * 1e3;
              REQUIRE(p.scalars[0].second[i + 6 * (j + 4 * k)] == Approx(x + 2 * y + 3 * z).epsilon(1e-5));
            }
        REQUIRE(p.scalars[1].second[0] == Approx(0.25));
      }
    }
    WHEN("a scalar has the wrong number of values") {
      std::ostringstream out;
      THEN("the export is refused and names the scalar") {
        REQUIRE_THROWS_AS(viz::writeVtkVolume(out, dv, {{"bad", std::vector<float>(5)}}, "t"),
                          std::invalid_argument);
      }
    }
  }
}
