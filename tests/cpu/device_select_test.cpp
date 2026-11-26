#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <stdexcept>
#include <vector>

#include "vkcore/device_select.hpp"

using Catch::Matchers::ContainsSubstring;
using vkcore::DeviceInfo;

namespace {
DeviceInfo make(const char* name, const char* uuidHex, bool discrete = true, std::uint32_t api = 0x402000) {
  DeviceInfo d;
  d.name = name;
  d.uuid = uuidHex;
  d.discrete = discrete;
  d.apiVersion = api;
  return d;
}
} // namespace

SCENARIO("A device selector picks one GPU without ambiguity", "[device]") {
  GIVEN("two identical cards and one older integrated GPU") {
    const std::vector<DeviceInfo> devices = {make("RTX 4060 Ti", "aabbccdd11223344"),
                                             make("RTX 4060 Ti", "aabb99ee55667788"),
                                             make("Radeon Pro 555X", "0011223344556677", false, 0x401046)};
    WHEN("the selector is an index into the usable devices") {
      THEN("index 1 is the second usable card, and the Vulkan 1.1 device is not counted") {
        REQUIRE(vkcore::selectDevice(devices, "1") == 1);
        REQUIRE_THROWS_AS(vkcore::selectDevice(devices, "2"), std::runtime_error);
      }
    }
    WHEN("the selector is a UUID prefix") {
      THEN("a unique prefix picks that card even if the cards have the same name") {
        REQUIRE(vkcore::selectDevice(devices, "aabbcc") == 0);
        REQUIRE(vkcore::selectDevice(devices, "AABB99") == 1);
      }
      THEN("a prefix shared by two cards is refused, naming the choice") {
        REQUIRE_THROWS_WITH(vkcore::selectDevice(devices, "aabb"), ContainsSubstring("ambiguous"));
      }
      THEN("a prefix shorter than four hex digits, an unknown one and an empty one are refused") {
        REQUIRE_THROWS_AS(vkcore::selectDevice(devices, "aa"), std::runtime_error);
        REQUIRE_THROWS_AS(vkcore::selectDevice(devices, "ffff"), std::runtime_error);
        REQUIRE_THROWS_AS(vkcore::selectDevice(devices, ""), std::runtime_error);
      }
      THEN("a device below Vulkan 1.2 cannot be selected by UUID either") {
        REQUIRE_THROWS_AS(vkcore::selectDevice(devices, "001122"), std::runtime_error);
      }
    }
    WHEN("the selector is text that is neither an index nor hex") {
      THEN("it is refused") {
        REQUIRE_THROWS_AS(vkcore::selectDevice(devices, "gpu0"), std::runtime_error);
        REQUIRE_THROWS_AS(vkcore::selectDevice(devices, "-1"), std::runtime_error);
      }
    }
  }
}

SCENARIO("The default device is the newest usable one", "[device]") {
  auto dev = [](const char* name, std::uint32_t api, bool discrete, std::uint64_t mem) {
    DeviceInfo d;
    d.name = name;
    d.uuid = "00";
    d.apiVersion = api;
    d.discrete = discrete;
    d.deviceLocalBytes = mem;
    return d;
  };
  constexpr std::uint32_t v11 = 0x401000, v12 = 0x402000, v14 = 0x404000;
  GIVEN("one Apple GPU offered through two drivers (MoltenVK Vulkan 1.2 with more memory, KosmicKrisp 1.4)") {
    const std::vector<DeviceInfo> d = {dev("M1 Pro (MoltenVK)", v12, false, 16u << 20),
                                       dev("M1 Pro (KosmicKrisp)", v14, false, 12u << 20)};
    THEN("the newer driver wins even with less device memory") {
      REQUIRE(vkcore::defaultDevice(d) == 1);
    }
  }
  GIVEN("an RTX and an older discrete Radeon, and an integrated GPU with a newer API") {
    const std::vector<DeviceInfo> d = {dev("Radeon", v12, true, 4u << 20),
                                       dev("integrated", v14, false, 32u << 20),
                                       dev("RTX", v14, true, 16u << 20)};
    THEN("a discrete GPU comes first, then the newest API") {
      REQUIRE(vkcore::defaultDevice(d) == 2);
    }
  }
  GIVEN("equal API versions") {
    const std::vector<DeviceInfo> d = {dev("a", v12, true, 4u << 20), dev("b", v12, true, 8u << 20)};
    THEN("the one with more device memory wins") {
      REQUIRE(vkcore::defaultDevice(d) == 1);
    }
  }
  GIVEN("only a Vulkan 1.1 device") {
    THEN("there is no default and the message says so") {
      REQUIRE_THROWS_WITH(vkcore::defaultDevice({dev("old", v11, true, 1)}),
                          ContainsSubstring("no suitable Vulkan device"));
    }
  }
}
