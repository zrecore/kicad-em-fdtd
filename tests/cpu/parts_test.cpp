// BDD scenarios for reading a part's value field.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "board/parts.hpp"

using Catch::Approx;

SCENARIO("A resistor's value field is read in the forms found on schematics") {
  struct Case {
    const char* text;
    double ohm;
  };
  GIVEN("common value fields") {
    for (const Case c : {Case{"0R", 0}, Case{"0", 0}, Case{"10R", 10}, Case{"4k7", 4700}, Case{"4.7k", 4700},
                         Case{"2R2", 2.2}, Case{"1M", 1e6}, Case{"47", 47}, Case{"220 ohm", 220},
                         Case{"0R0", 0}, Case{"10k 1%", 10e3}, Case{"100K", 1e5}, Case{"0 Ω", 0}}) {
      THEN(std::string("\"") + c.text + "\" is read to within 1e-9 relative") {
        const auto v = board::parseResistance(c.text);
        REQUIRE(v.has_value());
        REQUIRE(*v == Approx(c.ohm).margin(1e-12).epsilon(1e-9));
      }
    }
  }
  GIVEN("fields that are not a plain resistance") {
    for (const char* text : {"", "DNP", "2.2nH", "100nF", "BLM18", "k7", "1.2.3"})
      THEN(std::string("\"") + text + "\" gives no value") {
        REQUIRE(!board::parseResistance(text).has_value());
      }
  }
  GIVEN("reference designators") {
    THEN("the prefix is the leading letters, upper case") {
      REQUIRE(board::refPrefix("R12") == "R");
      REQUIRE(board::refPrefix("fb3") == "FB");
      REQUIRE(board::refPrefix("JP1") == "JP");
      REQUIRE(board::refPrefix("12") == "");
    }
  }
}

SCENARIO("The value decides whether a part is a resistor, not the reference") {
  GIVEN("value fields") {
    THEN("a mark, an ohm word or a plain zero says resistance; a bare number or a unit of another kind does "
         "not") {
      for (const char* v : {"0R", "0", "4k7", "10R", "220 ohm", "0 \u03a9", "1M"})
        REQUIRE(board::valueSaysResistance(v));
      for (const char* v : {"47", "100", "2.2nH", "1pF", "BLM18", "", "DNP"})
        REQUIRE(!board::valueSaysResistance(v));
    }
  }
}

SCENARIO("A capacitor's value field is read in the forms found on schematics") {
  struct Case {
    const char* text;
    double farad;
  };
  GIVEN("common value fields") {
    for (const Case c : {Case{"1pF", 1e-12}, Case{"100nF", 100e-9}, Case{"0.1u", 0.1e-6},
                         Case{"4p7", 4.7e-12}, Case{"2n2", 2.2e-9}, Case{"10 pF", 10e-12}, Case{"1uF", 1e-6},
                         Case{"22p", 22e-12}, Case{"0.5pF 50V", 0.5e-12}, Case{"1 \u00b5F", 1e-6}}) {
      THEN(std::string("\"") + c.text + "\" is read to within 1e-9 relative") {
        const auto v = board::parseCapacitance(c.text);
        REQUIRE(v.has_value());
        REQUIRE(*v == Approx(c.farad).epsilon(1e-9));
      }
    }
  }
  GIVEN("fields that are not a capacitance") {
    for (const char* text : {"", "100", "0R", "2.2nH", "4k7", "BLM18", "1F5", "DNP"})
      THEN(std::string("\"") + text + "\" gives no value") {
        REQUIRE(!board::parseCapacitance(text).has_value());
      }
  }
}
