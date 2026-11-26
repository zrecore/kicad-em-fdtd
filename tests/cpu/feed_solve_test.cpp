// BDD scenarios for the accurate feed-line estimate: the measured line solved as a 2D cross-section.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "board/feed_solve.hpp"
#include "board/line_impedance.hpp"

using Catch::Approx;
namespace l2 = fdtd::line2d;

namespace {
board::FeedReport line(double w, double gap, double h, double eps, double copper) {
  board::FeedReport f;
  f.ok = true;
  f.coplanar = gap > 0;
  f.widthMm = w;
  f.gapMm = gap;
  f.heightMm = h;
  f.epsR = eps;
  f.copperMm = copper;
  return f;
}
} // namespace

SCENARIO("A measured feed line is solved as a cross-section") {
  GIVEN("a 1 mm microstrip on 0.5 mm of er 4.3, a sheet of copper") {
    const auto f = line(1.0, 0, 0.5, 4.3, 0);
    const auto s = board::solveFeed(f, {}, 0.1);
    THEN("the solve agrees with the closed form within 3 %") {
      REQUIRE(s.ok);
      const double closed = board::microstripImpedance(1e-3, 0.5e-3, 4.3).z0Ohm;
      INFO("solve " << s.z0Ohm << ", closed form " << closed);
      REQUIRE(s.z0Ohm == Approx(closed).epsilon(0.03));
      REQUIRE(s.converged);
    }
  }
  GIVEN("a 0.8 mm coplanar line with 0.3 mm gaps, a sheet of copper") {
    const auto sheet = board::solveFeed(line(0.8, 0.3, 0.5, 4.3, 0), {}, 0.1);
    THEN("it agrees with the closed form within 4 %") {
      const double closed = board::groundedCoplanarImpedance(0.8e-3, 0.3e-3, 0.5e-3, 4.3).z0Ohm;
      INFO("sheet " << sheet.z0Ohm << ", closed form " << closed);
      REQUIRE(sheet.ok);
      REQUIRE(sheet.z0Ohm == Approx(closed).epsilon(0.04));
    }
  }
  GIVEN("a feed report that could not be read") {
    board::FeedReport f;
    f.why = "no ground plane";
    THEN("nothing is solved and the reason is kept") {
      const auto s = board::solveFeed(f);
      REQUIRE_FALSE(s.ok);
      REQUIRE(s.why == "no ground plane");
    }
  }
  GIVEN("the automatic cell") {
    THEN("it puts 4 cells in the narrowest feature and at least one across the copper") {
      const auto f = line(0.8, 0.3, 0.5, 4.3, 0.035);
      REQUIRE(board::feedCellMm(f) <= 0.3 / 4 + 1e-12);
      REQUIRE(board::feedCellMm(f) <= 0.035 + 1e-12);
    }
    AND_THEN("a wide line on thin copper stays cheap: under a million nodes at the finest cell") {
      const auto f = line(3.0, 0, 1.6, 4.3, 0.0175);
      const l2::Raster r =
          l2::rasterise(board::feedSection(f), board::feedCellMm(f) / 2 * 1e-3, false, board::feedGrid());
      INFO("nodes " << r.nx * r.ny);
      REQUIRE(r.nx * r.ny < 1'000'000);
    }
  }
}

SCENARIO("Real copper lowers a coplanar feed's impedance, which the closed form cannot say", "[slow]") {
  GIVEN("a 0.8 mm coplanar line with 0.3 mm gaps, as a sheet and with 35 um of copper") {
    const auto sheet = board::solveFeed(line(0.8, 0.3, 0.5, 4.3, 0));
    const auto thick = board::solveFeed(line(0.8, 0.3, 0.5, 4.3, 0.035));
    THEN("the copper lowers the impedance by 3 to 12 %, and the finding says how much copper was used") {
      REQUIRE(sheet.ok);
      REQUIRE(thick.ok);
      const double drop = 1 - thick.z0Ohm / sheet.z0Ohm;
      INFO("sheet " << sheet.z0Ohm << ", 35 um " << thick.z0Ohm);
      REQUIRE(drop > 0.03);
      REQUIRE(drop < 0.12);
      REQUIRE(thick.findings.front().find("35 um") != std::string::npos);
    }
  }
}

SCENARIO("Report: copper thickness against cell size on two coplanar feeds", "[.report]") {
  GIVEN("the IFA board's feed (0.76 wide, 0.20 gaps) and the Nordic reference feed (0.762, 0.154)") {
    struct Case {
      const char* name;
      double w, gap, h, eps, cu;
    };
    for (const Case c :
         {Case{"IFA", 0.76, 0.20, 1.44, 4.5, 0.035}, Case{"Nordic", 0.762, 0.154, 1.44, 4.5, 0.0385}})
      for (const double cell : {0.07, 0.0385 / 2, 0.035}) {
        const auto sheet = board::solveFeed(line(c.w, c.gap, c.h, c.eps, 0), {}, cell);
        const auto thick = board::solveFeed(line(c.w, c.gap, c.h, c.eps, c.cu), {}, cell);
        std::printf("%s cell %.4f: sheet %.2f ohm, copper %.4f mm -> %.2f ohm\n", c.name, cell, sheet.z0Ohm,
                    c.cu, thick.z0Ohm);
      }
  }
}

SCENARIO("Report: what 35 um of copper does to an IFA arm's line", "[.report]") {
  GIVEN(
      "a 1.21 mm line over 1.44 mm of er 4.5 (microstrip, the IFA arm's section) and the 0.46 mm feed neck") {
    for (const double w : {1.21, 0.46}) {
      const auto sheet = l2::lineImpedanceExtrapolated(l2::microstripSection(w * 1e-3, 1.44e-3, 4.5, 0),
                                                       0.02e-3, l2::SolveOptions{}, board::feedGrid());
      const auto thick = l2::lineImpedanceExtrapolated(l2::microstripSection(w * 1e-3, 1.44e-3, 4.5, 35e-6),
                                                       0.0175e-3, l2::SolveOptions{}, board::feedGrid());
      std::printf("w %.2f mm: sheet eps_eff %.4f Z0 %.2f; 35 um eps_eff %.4f Z0 %.2f; f scale sqrt(eeff "
                  "sheet/thick) %.4f\n",
                  w, sheet.epsEff, sheet.z0Ohm, thick.epsEff, thick.z0Ohm,
                  std::sqrt(sheet.epsEff / thick.epsEff));
    }
  }
}

SCENARIO("Report: OSH Park's published 50 ohm line against the 2D solve", "[.report]") {
  GIVEN(
      "15 mil on layer 1 over 7.87 mil of FR408HR (Dk 3.61), plane on layer 2; OSH Park predicts 49.8 ohm") {
    const double mil = 0.0254e-3;
    for (const double cu : {0.0, 1.8 * mil}) {
      const auto r = l2::lineImpedanceExtrapolated(l2::microstripSection(15 * mil, 7.87 * mil, 3.61, cu),
                                                   0.0125e-3, l2::SolveOptions{}, board::feedGrid());
      std::printf("copper %.1f um: Z0 %.2f ohm, eps_eff %.3f\n", cu * 1e6, r.z0Ohm, r.epsEff);
    }
  }
}

SCENARIO("Report: thick-copper shift of the validation line", "[.report]") {
  GIVEN("a 1.0 mm line on 1.6 mm of er 4.4, copper as a sheet and 0.2 mm thick (the slab acceptance run)") {
    const double mm = 1e-3;
    const auto sheet = l2::lineImpedanceExtrapolated(l2::microstripSection(1.0 * mm, 1.6 * mm, 4.4, 0),
                                                     0.04 * mm, l2::SolveOptions{}, board::feedGrid());
    const auto thick = l2::lineImpedanceExtrapolated(l2::microstripSection(1.0 * mm, 1.6 * mm, 4.4, 0.2 * mm),
                                                     0.04 * mm, l2::SolveOptions{}, board::feedGrid());
    std::printf(
        "sheet: Z0 %.2f eps_eff %.4f; 0.2 mm: Z0 %.2f eps_eff %.4f; ratios Z0 %.4f eps_eff %.4f k %.4f\n",
        sheet.z0Ohm, sheet.epsEff, thick.z0Ohm, thick.epsEff, thick.z0Ohm / sheet.z0Ohm,
        thick.epsEff / sheet.epsEff, std::sqrt(thick.epsEff / sheet.epsEff));
  }
}

SCENARIO("Report: convergence of the thick-copper shift in the 2D solve", "[.report]") {
  GIVEN("the validation line, plain solves (no extrapolation) on a graded grid at shrinking cells") {
    const double mm = 1e-3;
    for (const double cell : {0.08, 0.04, 0.02, 0.01}) {
      const auto sheet = l2::lineImpedance(l2::microstripSection(1.0 * mm, 1.6 * mm, 4.4, 0), cell * mm,
                                           l2::SolveOptions{}, board::feedGrid());
      const auto thick = l2::lineImpedance(l2::microstripSection(1.0 * mm, 1.6 * mm, 4.4, 0.2 * mm),
                                           cell * mm, l2::SolveOptions{}, board::feedGrid());
      std::printf("cell %.3f: sheet Z0 %.2f eeff %.4f; 0.2 mm Z0 %.2f eeff %.4f; ratios Z0 %.4f k %.4f\n",
                  cell, sheet.z0Ohm, sheet.epsEff, thick.z0Ohm, thick.epsEff, thick.z0Ohm / sheet.z0Ohm,
                  std::sqrt(thick.epsEff / sheet.epsEff));
    }
  }
}

SCENARIO("Report: static eps_eff and Z0 of the validation lines (2D solve, graded, extrapolated)",
         "[.report]") {
  GIVEN("microstrip on 1.6 mm of er 4.4, widths 1.0, 2.0 and 3.06 mm, a sheet and 35 um of copper") {
    const double mm = 1e-3;
    for (const double w : {1.0, 2.0, 3.06})
      for (const double cu : {0.0, 0.035}) {
        const auto r = l2::lineImpedanceExtrapolated(l2::microstripSection(w * mm, 1.6 * mm, 4.4, cu * mm),
                                                     0.04 * mm, l2::SolveOptions{}, board::feedGrid());
        std::printf("w %.2f mm copper %.3f mm: eps_eff %.4f Z0 %.2f ohm\n", w, cu, r.epsEff, r.z0Ohm);
      }
  }
}

SCENARIO("Report: wall bias of the 2D static eps_eff (w 2.0 mm, h 1.6 mm, er 4.4)", "[.report]") {
  GIVEN("the grounded walls moved away: margin factors 4, 8 and 16 of the larger of w and h") {
    const double mm = 1e-3;
    for (const double margin : {4.0, 8.0, 16.0}) {
      const auto r = l2::lineImpedanceExtrapolated(l2::microstripSection(2.0 * mm, 1.6 * mm, 4.4, 0, margin),
                                                   0.05 * mm, l2::SolveOptions{}, board::feedGrid());
      std::printf("margin %.0f: eps_eff %.4f Z0 %.2f ohm\n", margin, r.epsEff, r.z0Ohm);
    }
  }
}

SCENARIO("Report: microstrip closed form against the 2D solve over width ratio and permittivity",
         "[.report]") {
  GIVEN("a zero-thickness strip, walls 16 line-widths (or substrate heights) away, extrapolated graded "
        "solves") {
    const double mm = 1e-3, h = 1.0;
    for (const double er : {2.2, 4.4, 9.8})
      for (const double u : {0.1, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0}) {
        const double cell = std::min(u, 1.0) * h / 14.0;
        const auto r = l2::lineImpedanceExtrapolated(l2::microstripSection(u * h * mm, h * mm, er, 0, 16),
                                                     cell * mm, l2::SolveOptions{}, board::feedGrid());
        const auto cf = board::microstripImpedance(u * h * mm, h * mm, er);
        std::printf("er %.1f u %.2f: 2D eeff %.4f Z0 %.3f | closed form eeff %.4f Z0 %.3f | diff eeff %+.2f "
                    "%% Z0 %+.2f %%\n",
                    er, u, r.epsEff, r.z0Ohm, cf.epsEff, cf.z0Ohm, 100 * (cf.epsEff / r.epsEff - 1),
                    100 * (cf.z0Ohm / r.z0Ohm - 1));
      }
  }
}

SCENARIO("Report: conductor-backed coplanar closed form against the 2D solve", "[.report]") {
  GIVEN("a zero-thickness strip with semi-infinite ground beside it and a plane below, walls 16 substrate "
        "heights above") {
    const double mm = 1e-3, h = 1.0;
    for (const double er : {4.4, 9.8})
      for (const double u : {0.5, 1.0})
        for (const double g : {0.25, 0.5, 1.0}) {
          const double w = u * h, gap = g * w;
          const double cell = std::min(gap, std::min(w, h)) / 8.0;
          const auto r = l2::lineImpedanceExtrapolated(
              l2::groundedCoplanarSection(w * mm, gap * mm, h * mm, er, 0, true, true, 16), cell * mm,
              l2::SolveOptions{}, board::feedGrid());
          const auto cf = board::groundedCoplanarImpedance(w * mm, gap * mm, h * mm, er);
          std::printf("cbcpw er %.1f w/h %.2f gap/w %.2f: 2D eeff %.4f Z0 %.3f | closed form eeff %.4f Z0 "
                      "%.3f | diff eeff %+.2f %% Z0 %+.2f %%\n",
                      er, u, g, r.epsEff, r.z0Ohm, cf.epsEff, cf.z0Ohm, 100 * (cf.epsEff / r.epsEff - 1),
                      100 * (cf.z0Ohm / r.z0Ohm - 1));
        }
  }
}

SCENARIO("Report: what a solder mask does to a line's effective permittivity (the TI board's antenna)",
         "[.report]") {
  GIVEN("a 1.0 mm and a 0.5 mm line on 1.112 mm of er 4.5 over ground, and a 1.0 mm line over the same board "
        "floating 2 mm above ground, covered by 0, 20 and 40 um of mask of er 3.3 and 4.2 (assumed values)") {
    const double mm = 1e-3;
    struct Case {
      const char* name;
      double w, air; // strip width, air gap under the board (mm)
    };
    for (const Case c :
         {Case{"1.0 mm on 1.112 mm over ground", 1.0, 0}, Case{"0.5 mm on 1.112 mm over ground", 0.5, 0},
          Case{"1.0 mm on the board, ground 2 mm below it", 1.0, 2.0}}) {
      double base = 0;
      for (const double tm : {0.0, 0.020, 0.040}) {
        for (const double em : {3.3, 4.2}) {
          if (tm == 0 && em != 3.3)
            continue;
          l2::CrossSection s = l2::microstripSection(c.w * mm, 1.112 * mm, 4.5, 0, 8);
          const double h = c.air * mm + 1.112 * mm;
          s.layers.clear();
          if (c.air > 0)
            s.layers.push_back({c.air * mm, 1.0});
          s.layers.push_back({1.112 * mm, 4.5});
          if (tm > 0)
            s.layers.push_back({tm * mm, em});
          s.conductors[0].y0 = s.conductors[0].y1 = h;
          s.height = h + (s.height - 1.112 * mm);
          const auto r = l2::lineImpedanceExtrapolated(s, 0.025 * mm, l2::SolveOptions{}, board::feedGrid());
          if (tm == 0)
            base = r.epsEff;
          std::printf("%s: mask %2.0f um er %.1f: eps_eff %.4f, Z0 %.2f, frequency scale %.4f\n", c.name,
                      tm * 1e3, em, r.epsEff, r.z0Ohm, std::sqrt(base / r.epsEff));
        }
      }
    }
  }
}
