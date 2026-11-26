// BDD scenarios for the 2D cross-section solve (docs/PLAN.md, "2D cross-section solve"): the CPU reference
// that a GPU kernel is compared against, and the physics it must reproduce where a closed form exists.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdio>
#include <stdexcept>

#include "board/line_impedance.hpp"
#include "core/units.hpp"
#include "fdtd/line2d.hpp"

using Catch::Approx;
namespace l2 = fdtd::line2d;
using fdtd::units::kEps0;

namespace {
constexpr double mm = 1e-3;

// two plates 1 mm apart, as wide as the domain, mirror walls at the sides: a parallel-plate capacitor with no
// fringe
l2::CrossSection parallelPlate(double epsR) {
  l2::CrossSection s;
  s.width = 10 * mm;
  s.height = 1 * mm;
  s.walls = {l2::Wall::Mirror, l2::Wall::Mirror, l2::Wall::Ground, l2::Wall::Ground};
  s.layers = {{1 * mm, epsR}};
  s.conductors = {{0, 10 * mm, 0, 0, 0.0}, {0, 10 * mm, 1 * mm, 1 * mm, 1.0}};
  return s;
}
} // namespace

SCENARIO("The solver reproduces a parallel-plate capacitor exactly") {
  GIVEN("two plates 10 mm wide and 1 mm apart over a dielectric of er 4, with no fringing field") {
    const auto s = parallelPlate(4.0);
    WHEN("the capacitance is solved") {
      const auto sol = l2::solveCapacitance(s, 0.1 * mm);
      THEN("it converges and equals eps0 er w / d (the field is uniform, so the grid has no error)") {
        REQUIRE(sol.converged);
        REQUIRE(sol.capacitance == Approx(kEps0 * 4.0 * 10.0).epsilon(1e-6));
      }
    }
    WHEN("every dielectric is replaced by air") {
      const auto sol = l2::solveCapacitance(s, 0.1 * mm, {}, true);
      THEN("the capacitance is eps0 w / d") {
        REQUIRE(sol.capacitance == Approx(kEps0 * 10.0).epsilon(1e-6));
      }
    }
  }
}

SCENARIO("The two iterations a GPU would run give the same answer") {
  GIVEN("a microstrip section solved with red-black over-relaxation and with Jacobi") {
    const auto s = l2::microstripSection(1.0 * mm, 0.5 * mm, 4.4);
    l2::SolveOptions rb, jac;
    rb.tolerance = jac.tolerance = 1e-8;
    jac.maxIterations = 2000000;
    jac.method = l2::Method::Jacobi;
    const auto a = l2::solveCapacitance(s, 0.1 * mm, rb);
    const auto b = l2::solveCapacitance(s, 0.1 * mm, jac);
    THEN("both converge to the same capacitance within 1e-5") {
      REQUIRE(a.converged);
      REQUIRE(b.converged);
      REQUIRE(a.capacitance == Approx(b.capacitance).epsilon(1e-5));
    }
    AND_THEN("over-relaxation needs far fewer sweeps") {
      REQUIRE(a.iterations * 5 < b.iterations);
    }
  }
}

SCENARIO("A microstrip's impedance matches the closed form that was checked against the FDTD solver",
         "[slow]") {
  GIVEN("a 3.06 mm line on 1.6 mm of er 4.4 (the usual 50 ohm line), solved at 0.2 mm and 0.1 mm cells") {
    const auto r = l2::lineImpedanceExtrapolated(l2::microstripSection(3.06 * mm, 1.6 * mm, 4.4), 0.2 * mm);
    const auto cf = board::microstripImpedance(3.06 * mm, 1.6 * mm, 4.4);
    THEN("it converged and the impedance is within 2 percent of the closed form, the permittivity within 3") {
      REQUIRE(r.converged);
      REQUIRE(r.z0Ohm == Approx(cf.z0Ohm).epsilon(0.02));
      REQUIRE(r.epsEff == Approx(cf.epsEff).epsilon(0.03));
    }
  }
  GIVEN("a narrow 1.0 mm line on the same board inside walls eight line-widths away") {
    const auto r =
        l2::lineImpedanceExtrapolated(l2::microstripSection(1.0 * mm, 1.6 * mm, 4.4, 0, 8), 0.2 * mm);
    THEN("it is within 2 percent of the closed form's 87.5 ohm") {
      REQUIRE(r.z0Ohm == Approx(87.5).epsilon(0.02));
    }
  }
}

SCENARIO("The grid error of a thin conductor's edge is first order, and the extrapolation removes it",
         "[slow]") {
  GIVEN("an air coplanar line, 2 mm wide with 0.4 mm gaps, a ground plane 4 mm below") {
    const double w = 2 * mm, gap = 0.4 * mm, h = 4 * mm;
    const auto section = l2::groundedCoplanarSection(w, gap, h, 1.0);
    const auto cf = board::groundedCoplanarImpedance(w, gap, h, 1.0);
    const auto coarse = l2::lineImpedance(section, 0.2 * mm), mid = l2::lineImpedance(section, 0.1 * mm);
    THEN("the plain solve reads low and rises as the cell halves, the error roughly halving each time") {
      REQUIRE(coarse.converged);
      REQUIRE(mid.converged);
      REQUIRE(coarse.z0Ohm < mid.z0Ohm);
      REQUIRE(mid.z0Ohm < cf.z0Ohm);
      const double e1 = cf.z0Ohm - coarse.z0Ohm, e2 = cf.z0Ohm - mid.z0Ohm;
      REQUIRE(e1 / e2 == Approx(2.0).margin(0.5));
    }
    AND_THEN("the extrapolated value is within 3 percent of the closed form and the permittivity is 1") {
      const auto r = l2::lineImpedanceExtrapolated(section, 0.2 * mm);
      REQUIRE(r.converged);
      REQUIRE(r.z0Ohm == Approx(cf.z0Ohm).epsilon(0.03));
      REQUIRE(r.epsEff == Approx(1.0).epsilon(1e-6));
    }
  }
}

SCENARIO("A coplanar waveguide with ground below follows the conformal-mapping form where that form holds",
         "[slow]") {
  GIVEN("a 0.8 mm line with 0.3 mm gaps on 1.6 mm of er 4.4 (a sheet of copper), 0.1 mm and 0.05 mm cells") {
    const auto r = l2::lineImpedanceExtrapolated(
        l2::groundedCoplanarSection(0.8 * mm, 0.3 * mm, 1.6 * mm, 4.4), 0.1 * mm);
    const auto cf = board::groundedCoplanarImpedance(0.8 * mm, 0.3 * mm, 1.6 * mm, 4.4);
    THEN("it is within 2 percent of the closed form, impedance and permittivity") {
      REQUIRE(r.converged);
      REQUIRE(r.z0Ohm == Approx(cf.z0Ohm).epsilon(0.02));
      REQUIRE(r.epsEff == Approx(cf.epsEff).epsilon(0.02));
    }
  }
  GIVEN("the same line with 0.5 mm gaps") {
    const auto r = l2::lineImpedanceExtrapolated(
        l2::groundedCoplanarSection(0.8 * mm, 0.5 * mm, 1.6 * mm, 4.4), 0.1 * mm);
    const auto cf = board::groundedCoplanarImpedance(0.8 * mm, 0.5 * mm, 1.6 * mm, 4.4);
    THEN("it is within 3 percent of the closed form (73.3 ohm)") {
      REQUIRE(r.z0Ohm == Approx(cf.z0Ohm).epsilon(0.03));
    }
  }
}

SCENARIO("The solver handles what the closed forms cannot", "[slow]") {
  const double w = 0.8 * mm, gap = 0.3 * mm, h = 1.6 * mm;
  const auto base = l2::lineImpedanceExtrapolated(l2::groundedCoplanarSection(w, gap, h, 4.4), 0.1 * mm);
  GIVEN("the same line with ground beside it on one side only") {
    const auto r =
        l2::lineImpedanceExtrapolated(l2::groundedCoplanarSection(w, gap, h, 4.4, 0, true, false), 0.1 * mm);
    THEN("the impedance is more than 10 percent higher: there is less capacitance to the missing side") {
      REQUIRE(r.converged);
      REQUIRE(r.z0Ohm > base.z0Ohm * 1.10);
    }
  }
  GIVEN("a strip with 0.05 mm of copper instead of a sheet") {
    const auto r =
        l2::lineImpedanceExtrapolated(l2::groundedCoplanarSection(w, gap, h, 4.4, 0.05 * mm), 0.05 * mm);
    const auto sheet = l2::lineImpedanceExtrapolated(l2::groundedCoplanarSection(w, gap, h, 4.4), 0.05 * mm);
    THEN("the impedance is more than 5 percent lower: the strip's sides add capacitance to the ground beside "
         "it") {
      REQUIRE(r.converged);
      REQUIRE(r.z0Ohm < sheet.z0Ohm * 0.95);
    }
  }
}

SCENARIO("Grid lines fall on every conductor edge, so a gap that is not a whole number of cells is exact") {
  GIVEN("the Nordic reference feed: 0.762 mm wide, 0.154 mm gaps, 1.44 mm of er 4.5, 38.5 um of copper") {
    const auto section = l2::groundedCoplanarSection(0.762 * mm, 0.154 * mm, 1.44 * mm, 4.5, 0.0385 * mm);
    WHEN("it is rasterised with a cell that does not divide the gap") {
      const l2::Raster r = l2::rasterise(section, 0.07 * mm);
      THEN("a node column lies on each conductor edge: the gap between two nodes is the gap, to 1e-9 m") {
        REQUIRE(r.xs.size() == r.nx);
        const double x0 = (section.width - 0.762 * mm) / 2;
        auto has = [&](double x) {
          for (double v : r.xs)
            if (std::fabs(v - x) < 1e-9 * mm)
              return true;
          return false;
        };
        REQUIRE(has(x0));
        REQUIRE(has(x0 - 0.154 * mm));
        REQUIRE(has(x0 + 0.762 * mm));
        REQUIRE(has(x0 + 0.762 * mm + 0.154 * mm));
        auto hasY = [&](double y) {
          for (double v : r.ys)
            if (std::fabs(v - y) < 1e-9 * mm)
              return true;
          return false;
        };
        REQUIRE(hasY(1.44 * mm));
        REQUIRE(hasY(1.44 * mm + 0.0385 * mm));
      }
    }
  }
}

SCENARIO("The feed's impedance no longer swings with the cell it is solved on", "[slow]") {
  GIVEN("the Nordic reference feed as a sheet and with 38.5 um of copper") {
    for (const double cu : {0.0, 0.0385}) {
      const auto section = l2::groundedCoplanarSection(0.762 * mm, 0.154 * mm, 1.44 * mm, 4.5, cu * mm);
      double lo = 1e9, hi = 0;
      for (const double cell : {0.07, 0.035, 0.0192}) {
        const double z = l2::lineImpedanceExtrapolated(section, cell * mm).z0Ohm;
        lo = std::min(lo, z);
        hi = std::max(hi, z);
      }
      THEN(std::string("the three cells agree within 3 percent, ") +
           (cu > 0 ? "with copper" : "as a sheet")) {
        INFO("range " << lo << " to " << hi);
        REQUIRE((hi - lo) / lo < 0.03);
      }
    }
  }
}

SCENARIO("A graded grid gives the uniform grid's answer on far fewer nodes", "[slow]") {
  GIVEN("a 0.8 mm coplanar line with 0.3 mm gaps and 35 um of copper on 1.6 mm of er 4.4") {
    const auto section = l2::groundedCoplanarSection(0.8 * mm, 0.3 * mm, 1.6 * mm, 4.4, 0.035 * mm);
    const double cell = 0.035 * mm;
    l2::GridOptions graded;
    graded.growth = 1.3;
    graded.maxCellFactor = 8;
    const l2::Raster uniform = l2::rasterise(section, cell);
    const l2::Raster grown = l2::rasterise(section, cell, false, graded);
    THEN("it has under a third of the nodes, and its capacitance agrees within 1.5 percent (the coarse far "
         "field costs about 0.8)") {
      INFO("nodes " << uniform.nx * uniform.ny << " against " << grown.nx * grown.ny);
      REQUIRE(grown.nx * grown.ny * 3 < uniform.nx * uniform.ny);
      const auto a = l2::solveRaster(uniform), b = l2::solveRaster(grown);
      REQUIRE(a.converged);
      REQUIRE(b.converged);
      REQUIRE(b.capacitance == Approx(a.capacitance).epsilon(0.015));
    }
    AND_THEN("the graded extrapolated impedance is within 1 percent of the uniform one") {
      const auto u = l2::lineImpedanceExtrapolated(section, 0.07 * mm);
      const auto g = l2::lineImpedanceExtrapolated(section, 0.07 * mm, l2::SolveOptions{}, graded);
      INFO("uniform " << u.z0Ohm << ", graded " << g.z0Ohm);
      REQUIRE(g.z0Ohm == Approx(u.z0Ohm).epsilon(0.01));
    }
  }
}

SCENARIO("A section that cannot be rasterised is refused") {
  GIVEN("a conductor outside the domain") {
    auto s = parallelPlate(4.0);
    s.conductors.push_back({0, 20 * mm, 0.5 * mm, 0.5 * mm, 1.0});
    THEN("the solve throws") {
      REQUIRE_THROWS_AS(l2::solveCapacitance(s, 0.1 * mm), std::invalid_argument);
    }
  }
  GIVEN("a cell as large as the domain") {
    THEN("the solve throws") {
      REQUIRE_THROWS_AS(l2::solveCapacitance(parallelPlate(4.0), 5 * mm), std::invalid_argument);
    }
  }
}

// Not a check: the numbers that explain the FDTD runs in docs/PLAN.md ("Closed-form line impedance checked
// against the solver") and the feed row's closed form. Run with `tests_cpu "[.report]"` from a release build.
SCENARIO("Report: the quasi-static impedance of the lines the FDTD validation used, and a vendor RF line",
         "[.report]") {
  struct Row {
    const char* name;
    l2::CrossSection s;
    double cell;
  };
  const Row rows[] = {
      {"microstrip 3.06 mm", l2::microstripSection(3.06 * mm, 1.6 * mm, 4.4, 0, 8), 0.1 * mm},
      {"microstrip 1.0 mm", l2::microstripSection(1.0 * mm, 1.6 * mm, 4.4, 0, 8), 0.1 * mm},
      {"coplanar 0.8 mm, gap 0.3 mm", l2::groundedCoplanarSection(0.8 * mm, 0.3 * mm, 1.6 * mm, 4.4),
       0.05 * mm},
      {"coplanar 0.8 mm, gap 0.5 mm", l2::groundedCoplanarSection(0.8 * mm, 0.5 * mm, 1.6 * mm, 4.4),
       0.05 * mm},
      {"0.762 mm, gap 0.154 mm, 1.44 mm, er 4.5, sheet",
       l2::groundedCoplanarSection(0.762 * mm, 0.154 * mm, 1.44 * mm, 4.5), 0.077 * mm},
      {"same with 0.0385 mm of copper",
       l2::groundedCoplanarSection(0.77 * mm, 0.154 * mm, 1.44 * mm, 4.5, 0.0385 * mm), 0.0385 * mm},
  };
  for (const Row& r : rows) {
    const auto z = l2::lineImpedanceExtrapolated(r.s, r.cell);
    std::printf("%-50s cell %.3f mm: Z0 %.2f ohm, eps_eff %.3f (%zu sweeps, %s)\n", r.name, r.cell * 1e3,
                z.z0Ohm, z.epsEff, z.iterations, z.converged ? "converged" : "NOT converged");
  }
  SUCCEED();
}

SCENARIO("Report: graded against uniform grid", "[.report]") {
  GIVEN("the 0.8 mm coplanar line with 35 um of copper") {
    const auto section = l2::groundedCoplanarSection(0.8 * mm, 0.3 * mm, 1.6 * mm, 4.4, 0.035 * mm);
    l2::GridOptions graded;
    graded.growth = 1.3;
    graded.maxCellFactor = 8;
    const auto u = l2::lineImpedanceExtrapolated(section, 0.07 * mm);
    const auto g = l2::lineImpedanceExtrapolated(section, 0.07 * mm, l2::SolveOptions{}, graded);
    std::printf("uniform %.3f ohm, graded %.3f ohm, sweeps %zu and %zu\n", u.z0Ohm, g.z0Ohm, u.iterations,
                g.iterations);
  }
}
