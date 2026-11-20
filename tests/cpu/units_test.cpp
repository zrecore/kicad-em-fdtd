#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/units.hpp"

using Catch::Approx;
using namespace fdtd::units;

SCENARIO("Board coordinates are converted to SI metres at the boundary") {
  GIVEN("a length KiCAD reports in integer nanometres") {
    const std::int64_t nm = 1'600'000; // a 1.6 mm board thickness
    WHEN("it is converted") {
      THEN("the result is 1.6e-3 metres") {
        REQUIRE(nm_to_m(nm) == Approx(1.6e-3));
      }
    }
  }
  GIVEN("a length in millimetres from a datasheet") {
    WHEN("it is converted") {
      THEN("4.25 mm is 4.25e-3 metres") {
        REQUIRE(mm_to_m(4.25) == Approx(4.25e-3));
      }
    }
  }
  GIVEN("the 2.44 GHz band centre") {
    THEN("its free-space wavelength is about 122.9 mm") {
      REQUIRE(kSpeedOfLight / ghz_to_hz(2.44) == Approx(0.1229).epsilon(1e-3));
    }
  }
}
