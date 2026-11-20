#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "vkcore/compute.hpp"

namespace {
#ifdef FDTD_VALIDATION
constexpr bool kValidation = true;
#else
constexpr bool kValidation = false;
#endif
} // namespace

SCENARIO("A compute kernel fills a buffer on the GPU", "[gpu]") {
  GIVEN("a Vulkan context (with validation when enabled) and a fill kernel") {
    vkcore::Context ctx({.validation = kValidation});
    constexpr std::uint32_t n = 1u << 20;
    vkcore::Buffer buf(ctx, n * sizeof(float));
    vkcore::ComputePipeline fill(ctx, std::string(FDTD_SHADER_DIR) + "/hello.comp.spv", 1,
                                 sizeof(std::uint32_t));
    std::fill(buf.as<float>().begin(), buf.as<float>().end(), 0.0f);

    WHEN("one dispatch covers every element") {
      vkcore::Buffer* bufs[] = {&buf};
      const auto timing = fill.run(bufs, &n, (n + 63) / 64);

      THEN("every element holds 2*i + 1") {
        const auto data = buf.as<float>();
        std::size_t bad = 0;
        for (std::uint32_t i = 0; i < n; ++i)
          bad += data[i] != 2.0f * static_cast<float>(i) + 1.0f;
        REQUIRE(bad == 0);
      }
      AND_THEN("the device reports a positive GPU time") {
        REQUIRE(timing.gpuMs > 0.0);
      }
      AND_THEN("the validation layer was active and reported no errors") {
        if (kValidation)
          REQUIRE(ctx.validationEnabled());
        REQUIRE(ctx.validationErrors() == 0);
      }
    }
  }
}

SCENARIO("The default device rule never picks an unsuitable device", "[gpu]") {
  GIVEN("the default context options") {
    vkcore::Context ctx;
    THEN("the chosen device supports Vulkan 1.2 or later and reports device-local memory") {
      REQUIRE(ctx.properties().apiVersion >= VK_API_VERSION_1_2);
      REQUIRE(ctx.deviceLocalBytes() > 0);
    }
  }
}
