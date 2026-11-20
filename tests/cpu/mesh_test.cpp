// BDD scenarios for the 1D graded mesher.
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "board/mesh.hpp"

SCENARIO("A graded mesh is fine where asked and coarse elsewhere") {
  GIVEN("a 40 mm line, 1 mm coarse cells and a 0.1 mm refinement over 10 to 12 mm") {
    const auto edges = board::gradedEdges(0, 0.040, 1e-3, {{0.010, 0.012, 0.1e-3}}, 1.3);
    THEN("the mesh spans the line exactly and increases strictly") {
      REQUIRE(edges.front() == 0.0);
      REQUIRE(edges.back() == 0.040);
      for (std::size_t i = 1; i < edges.size(); ++i)
        REQUIRE(edges[i] > edges[i - 1]);
    }
    THEN("cells inside the refinement are at most 0.1 mm and no cell exceeds 1.5 mm (the last may stretch)") {
      for (std::size_t i = 1; i < edges.size(); ++i) {
        const double h = edges[i] - edges[i - 1], mid = 0.5 * (edges[i] + edges[i - 1]);
        REQUIRE(h <= 1.5e-3 * (1 + 1e-9));
        if (mid > 0.010 && mid < 0.012)
          REQUIRE(h <= 0.1e-3 * (1 + 1e-9));
      }
    }
    THEN("neighbouring cells differ by at most 1.35 (smooth grading)") {
      for (std::size_t i = 2; i < edges.size() - 1; ++i) { // the last cell absorbs the rounding
        const double a = edges[i - 1] - edges[i - 2], b = edges[i] - edges[i - 1];
        REQUIRE(std::max(a / b, b / a) <= 1.35);
      }
    }
    THEN("it needs far fewer cells than a uniform 0.1 mm mesh (400): under 120") {
      REQUIRE(edges.size() - 1 < 120);
    }
  }
}

SCENARIO("Grid lines can be snapped onto geometry edges") {
  GIVEN("a 40 mm line with 1 mm cells and copper edges at 10.37 mm and 25.81 mm") {
    const auto base = board::gradedEdges(0, 0.040, 1e-3, {}, 1.3);
    const auto snapped = board::gradedEdges(0, 0.040, 1e-3, {}, 1.3, {10.37e-3, 25.81e-3});
    THEN("an edge lies exactly on each position") {
      for (double target : {10.37e-3, 25.81e-3}) {
        double best = 1;
        for (double e : snapped)
          best = std::min(best, std::abs(e - target));
        REQUIRE(best < 1e-12);
      }
    }
    THEN("the mesh still spans the line, increases strictly, keeps the cell count, and no cell shrinks below "
         "40 % of the original") {
      REQUIRE(snapped.front() == 0.0);
      REQUIRE(snapped.back() == 0.040);
      REQUIRE(snapped.size() == base.size());
      for (std::size_t i = 1; i < snapped.size(); ++i) {
        REQUIRE(snapped[i] > snapped[i - 1]);
        REQUIRE(snapped[i] - snapped[i - 1] >= 0.4 * (base[i] - base[i - 1]));
      }
    }
    THEN("cells far from the snapped positions are unchanged") {
      REQUIRE(snapped[2] == base[2]);
      REQUIRE(snapped[snapped.size() - 3] == base[base.size() - 3]);
    }
  }
}
