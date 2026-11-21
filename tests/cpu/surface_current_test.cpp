// BDD scenario for surface current on copper (item 11b in docs/PLAN.md).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <numbers>

#include "fdtd/surface_current.hpp"
#include "sim/simulation.hpp"

using namespace fdtd;

SCENARIO("Surface current on a shorted strip equals the current the port delivers", "[surface][slow]") {
  GIVEN("a 30 mm x 16 mm strip 1 mm over a ground sheet, shorted to it at the far end and fed by 5 parallel "
        "lumped edges (50 ohm in all, the two border edges feeding half a cell each) at the near end; 0.1 "
        "wavelength long at 1 GHz, so the current "
        "is nearly the same along it") {
    const Grid grid = Grid::uniform({44, 24, 8}, {0.044, 0.024, 0.008});
    Solver<double> s(grid, 0.95);
    constexpr std::size_t kI0 = 6, kLen = 30, kJ0 = 4, kJ1 = 20, kGround = 3, kStrip = 4;
    std::vector<PecEdge> pec;
    for (std::size_t j = 0; j <= grid.cells(Y); ++j)
      for (std::size_t i = 0; i < grid.cells(X); ++i)
        pec.push_back({X, grid.nodeIndex(i, j, kGround)});
    for (std::size_t j = 0; j < grid.cells(Y); ++j)
      for (std::size_t i = 0; i <= grid.cells(X); ++i)
        pec.push_back({Y, grid.nodeIndex(i, j, kGround)});
    const std::size_t stripStart = pec.size();
    for (std::size_t j = kJ0; j <= kJ1; ++j)
      for (std::size_t i = kI0; i < kI0 + kLen; ++i)
        pec.push_back({X, grid.nodeIndex(i, j, kStrip)});
    for (std::size_t j = kJ0; j < kJ1; ++j)
      for (std::size_t i = kI0; i <= kI0 + kLen; ++i)
        pec.push_back({Y, grid.nodeIndex(i, j, kStrip)});
    for (std::size_t j = kJ0; j <= kJ1; ++j) // the short: a vertical sheet of forced Ez edges
      pec.push_back({Z, grid.nodeIndex(kI0 + kLen, j, kGround)});
    s.forcePecEdges(pec);
    const auto pulse = sim::bandPulse(0.4e9, 1.6e9);
    const std::size_t rows = kJ1 - kJ0 + 1;
    // each edge feeds its share of the width: 1 cell inside the strip, half a cell on the two border rows, so
    // its resistance is 50 ohm times the strip width over that share
    for (std::size_t j = kJ0; j <= kJ1; ++j) {
      const double share = (j == kJ0 || j == kJ1) ? 0.5 : 1.0;
      s.addLumpedPort({kI0, j, kGround, Z, 50.0 * static_cast<double>(kJ1 - kJ0) / share, pulse});
    }
    const auto list =
        surfaceCurrentList(grid, {pec.begin() + static_cast<std::ptrdiff_t>(stripStart), pec.end()});

    WHEN("the pulse is run in and the phasors at 1 GHz are accumulated (every step)") {
      const double f = 1e9;
      std::vector<std::complex<double>> h(list.channels.size());
      std::complex<double> iPort = 0;
      for (int n = 0; n < 4000; ++n) {
        s.step();
        const double t = s.time(), dt = s.dt();
        const auto pe = std::polar(dt, -2 * std::numbers::pi * f * t);
        const auto ph = std::polar(dt, -2 * std::numbers::pi * f * (t - 0.5 * dt));
        for (std::size_t p = 0; p < rows; ++p)
          iPort += s.portCurrent(p) * pe;
        for (std::size_t c = 0; c < h.size(); ++c)
          h[c] += double(s.field(Kind::H, list.channels[c].component)[list.channels[c].node]) * ph;
      }
      auto through = [&](std::size_t col) { // current through the cut at x edge column `col`
        std::complex<double> sum = 0;
        for (std::size_t c = 0; c < list.cells.size(); ++c)
          if (list.cells[c].direction == X && list.cells[c].i == col)
            sum += surfaceCurrent(list.cells[c], h[2 * c], h[2 * c + 1]) * list.cells[c].crossWidth;
        return sum;
      };
      THEN("the current through the first cut is within 8 % of the port current in magnitude (the current "
           "crowds at a strip's edges, which a staircase cannot resolve: the shortfall measured 28 % at 4 "
           "cells wide, 14 % at 8, 9 % at 12, 5.6 % at 16) and 0.1 rad "
           "in phase, up to the sign of the port convention") {
        REQUIRE(std::abs(iPort) > 0);
        const auto cut = through(kI0 + 1);
        CHECK(std::abs(cut) / std::abs(iPort) == Catch::Approx(1.0).margin(0.08));
        CHECK(std::abs(std::abs(std::arg(cut / iPort)) - std::numbers::pi) < 0.1);
      }
    }
  }
}
