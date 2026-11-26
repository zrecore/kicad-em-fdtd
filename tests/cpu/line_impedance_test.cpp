// BDD scenarios for the closed-form feed line impedance (docs/PLAN.md, "Closed-form line impedance checked
// against the solver").
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include "board/line_impedance.hpp"

using Catch::Approx;

SCENARIO("A microstrip's impedance follows the closed form, as checked against the FDTD solver") {
  GIVEN("a 3.06 mm line on 1.6 mm of er 4.4, the usual 50 ohm line") {
    const auto z = board::microstripImpedance(3.06e-3, 1.6e-3, 4.4);
    THEN("it is 50 ohm within 3 percent and its effective permittivity is near 3.3") {
      REQUIRE(z.z0Ohm == Approx(50.0).epsilon(0.03));
      REQUIRE(z.epsEff == Approx(3.33).margin(0.1));
    }
  }
  GIVEN("a narrow 1.0 mm line on the same board") {
    const auto z = board::microstripImpedance(1.0e-3, 1.6e-3, 4.4);
    THEN("it is about 87.5 ohm (the solver read 87.0 once its air was resolved)") {
      REQUIRE(z.z0Ohm == Approx(87.5).epsilon(0.02));
    }
  }
  GIVEN("a wider line") {
    THEN("the impedance falls as the line widens") {
      REQUIRE(board::microstripImpedance(5e-3, 1.6e-3, 4.4).z0Ohm <
              board::microstripImpedance(3e-3, 1.6e-3, 4.4).z0Ohm);
    }
  }
}

SCENARIO("A coplanar waveguide with ground follows the conformal-mapping closed form") {
  GIVEN("the air coplanar line with k = 1/sqrt(2) and no ground reachable (a very tall substrate)") {
    const double w = 1e-3, gap = 0.5e-3 * (std::sqrt(2.0) - 1.0);
    const auto z = board::groundedCoplanarImpedance(w, gap, 100.0, 1.0);
    THEN("it is 30 pi ohm, the exact value for K/K' = 1") {
      REQUIRE(z.z0Ohm == Approx(30.0 * M_PI).epsilon(0.005));
    }
  }
  GIVEN("a 0.762 mm line with 0.154 mm gaps over 1.44 mm of er 4.5 (a vendor reference RF line)") {
    const auto z = board::groundedCoplanarImpedance(0.762e-3, 0.154e-3, 1.44e-3, 4.5);
    THEN("it matches scripts/antenna/lines.py: 54.6 ohm") {
      REQUIRE(z.z0Ohm == Approx(54.6).epsilon(0.005));
    }
  }
  GIVEN("the same line with a wider gap") {
    THEN("the impedance rises with the gap") {
      REQUIRE(board::groundedCoplanarImpedance(0.762e-3, 0.3e-3, 1.44e-3, 4.5).z0Ohm >
              board::groundedCoplanarImpedance(0.762e-3, 0.154e-3, 1.44e-3, 4.5).z0Ohm);
    }
  }
}

SCENARIO("The elliptic integral ratio is exact where it is known") {
  THEN("K/K' is 1 at k = 1/sqrt(2)") {
    REQUIRE(board::ellipticRatio(1.0 / std::sqrt(2.0)) == Approx(1.0).epsilon(1e-9));
  }
}
