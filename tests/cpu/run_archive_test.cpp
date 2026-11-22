// BDD scenarios for the saved field data of a run (docs/PLAN.md, Next steps 16).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>

#include "app/run_archive.hpp"

namespace fs = std::filesystem;

namespace {
fs::path scratch(const std::string& name) {
  const char* base = std::getenv("FDTD_TEST_TMP");
  const fs::path dir = (base ? fs::path(base) : fs::temp_directory_path()) / "run_archive";
  fs::create_directories(dir);
  return dir / name;
}
} // namespace

SCENARIO("Phasors are saved in half precision and read back") {
  GIVEN("three frequencies over 1000 voxels, with values around 1e-8 (solver units are tiny)") {
    viz::PhasorAccumulator acc({2.4e9, 2.44e9, 2.48e9}, 1000);
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0.0f, 1e-8f);
    std::vector<float> re(3000), im(3000);
    for (std::size_t i = 0; i < 3000; ++i) {
      re[i] = nd(rng);
      im[i] = nd(rng);
    }
    for (std::size_t i = 2000; i < 3000; ++i)
      re[i] = im[i] = 0.0f; // the third frequency saw no field at all
    acc.load(re, im);
    const auto file = scratch("roundtrip.f16");
    app::savePhasors(file, acc);
    const auto back = app::loadPhasors(file);
    THEN("the frequencies and size are kept") {
      REQUIRE(back->frequencies() == 3);
      REQUIRE(back->voxels() == 1000);
      REQUIRE(back->frequencyHz(1) == Catch::Approx(2.44e9));
    }
    AND_THEN("every value is within 0.1 % of the largest value at its frequency") {
      for (std::size_t k = 0; k < 2; ++k) {
        float peak = 0;
        for (std::size_t v = 0; v < 1000; ++v)
          peak = std::max({peak, std::abs(acc.phasor(k, v).real()), std::abs(acc.phasor(k, v).imag())});
        for (std::size_t v = 0; v < 1000; ++v) {
          REQUIRE(std::abs(back->phasor(k, v).real() - acc.phasor(k, v).real()) <= 1e-3f * peak);
          REQUIRE(std::abs(back->phasor(k, v).imag() - acc.phasor(k, v).imag()) <= 1e-3f * peak);
        }
      }
    }
    AND_THEN("a frequency without field stays exactly zero") {
      for (std::size_t v = 0; v < 1000; ++v)
        REQUIRE(back->phasor(2, v) == std::complex<float>(0.0f, 0.0f));
    }
    AND_THEN("the file is about a quarter of the size of the float arrays") {
      REQUIRE(fs::file_size(file) < 3000 * 2 * sizeof(float) / 2 + 200);
    }
  }
}

SCENARIO("A damaged phasor file is refused, not half-read") {
  GIVEN("a saved file cut short, and a file that is not ours") {
    viz::PhasorAccumulator acc({2.4e9}, 100);
    const auto file = scratch("cut.f16");
    app::savePhasors(file, acc);
    fs::resize_file(file, fs::file_size(file) - 10);
    const auto other = scratch("other.f16");
    std::ofstream(other) << "not a phasor file at all, but long enough to read a header from";
    THEN("loading either throws") {
      REQUIRE_THROWS(app::loadPhasors(file));
      REQUIRE_THROWS(app::loadPhasors(other));
      REQUIRE_THROWS(app::loadPhasors(scratch("missing.f16")));
    }
  }
}
