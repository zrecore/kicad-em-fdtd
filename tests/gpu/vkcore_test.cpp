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

SCENARIO("A device selector chooses the GPU a run uses", "[gpu][device]") {
  GIVEN("the Vulkan devices the loader reports") {
    const auto devices = vkcore::listDevices();
    REQUIRE_FALSE(devices.empty());
    std::size_t firstUsable = devices.size();
    for (std::size_t i = 0; i < devices.size() && firstUsable == devices.size(); ++i)
      if (vkcore::usable(devices[i]))
        firstUsable = i;
    REQUIRE(firstUsable < devices.size());

    WHEN("the UUID of a usable device is given as the selector") {
      THEN("the context runs on that device") {
        const auto& want = devices[firstUsable];
        REQUIRE(want.uuid.size() == 32);
        vkcore::Context ctx({.device = want.uuid});
        REQUIRE(std::string(ctx.properties().deviceName) == want.name);
        REQUIRE(ctx.properties().deviceID == want.deviceId);
      }
    }
    WHEN("the selector names no device") {
      THEN("the context is refused instead of falling back to another GPU") {
        REQUIRE_THROWS(vkcore::Context({.device = "ffffffff"}));
        REQUIRE_THROWS(vkcore::Context({.device = "99"}));
      }
    }
  }
}

SCENARIO("A device-resident buffer works on the staged path too", "[gpu][staged]") {
  GIVEN("a context forced onto staged buffers and the fill kernel") {
    vkcore::Context ctx({.validation = kValidation, .stagedBuffers = true});
    constexpr std::uint32_t n = 1u << 16;
    vkcore::Buffer buf(ctx, n * sizeof(float), vkcore::Residency::Device);
    REQUIRE(buf.staged());
    vkcore::ComputePipeline fill(ctx, std::string(FDTD_SHADER_DIR) + "/hello.comp.spv", 1,
                                 sizeof(std::uint32_t));
    std::fill(buf.as<float>().begin(), buf.as<float>().end(), -1.0f);
    buf.upload();
    WHEN("the kernel fills the VRAM copy and the host reads it back") {
      vkcore::Buffer* bufs[] = {&buf};
      fill.run(bufs, &n, (n + 63) / 64);
      THEN("the host view still holds the old data until download()") {
        REQUIRE(buf.as<float>()[5] == -1.0f);
        buf.download();
        std::size_t bad = 0;
        for (std::uint32_t i = 0; i < n; ++i)
          bad += buf.as<float>()[i] != 2.0f * i + 1.0f;
        REQUIRE(bad == 0);
      }
    }
  }
}
