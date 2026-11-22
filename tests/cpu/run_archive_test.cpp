// BDD scenarios for the saved field data of a run (docs/PLAN.md, Next steps 16).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>

#include "app/run_archive.hpp"
#include "app/run_store.hpp"
#include "app/selection_store.hpp"
#include "fdtd/sparam.hpp"

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

SCENARIO("A project's saved runs are listed, reopened and pruned") {
  GIVEN("three run folders, each with fields, one without a manifest") {
    const fs::path root = scratch("runs");
    fs::remove_all(root);
    for (const char* id : {"run-20260101-000001", "run-20260102-000001", "run-20260103-000001"}) {
      const fs::path dir = root / id;
      fs::create_directories(dir);
      viz::PhasorAccumulator acc({2.4e9, 2.5e9}, 10);
      std::vector<float> re(20, 1e-7f), im(20, -2e-7f);
      acc.load(re, im);
      app::FarResult far;
      far.frequencyHz = 2.4e9;
      far.field.nTheta = 1;
      far.field.nPhi = 2;
      far.field.theta = {0.1};
      far.field.phi = {0.2, 0.3};
      far.field.u = {1.5, 2.5};
      far.field.peak = 2.5;
      far.figures.gain = 1.25;
      app::saveFields(dir, {{"slice_e", &acc}, {"missing", nullptr}}, {far});
      fdtd::SParam1 s;
      s.frequenciesHz = {2.4e9, 2.5e9};
      s.s11 = {{0.5, 0.0}, {0.1, 0.0}};
      std::ofstream f(dir / "s11.s1p");
      fdtd::writeTouchstone(f, s, "test");
    }
    app::RunManifest m;
    m.id = "run-20260103-000001";
    m.board = "demo";
    m.bestDb = -20;
    m.bestHz = 2.5e9;
    m.fields = true;
    app::writeManifest(root / m.id, m);
    WHEN("the runs are scanned") {
      const auto runs = app::scanRuns(root);
      THEN("they are listed newest first, with the manifest or the S11 file as the source") {
        REQUIRE(runs.size() == 3);
        REQUIRE(runs[0].id == "run-20260103-000001");
        REQUIRE(runs[0].board == "demo");
        REQUIRE(runs[1].bestDb == Catch::Approx(-20.0).margin(0.01)); // from s11.s1p: |0.1| = -20 dB
        REQUIRE(runs[1].bestHz == Catch::Approx(2.5e9));
        REQUIRE(runs[1].fields);
        REQUIRE(fs::exists(root / "index.json"));
      }
      AND_THEN("a saved quantity and the far field read back, and a missing one is null") {
        const auto acc = app::loadField(root / "run-20260101-000001", "slice_e");
        REQUIRE(acc);
        REQUIRE(acc->phasor(1, 3).real() == Catch::Approx(1e-7f).epsilon(1e-3));
        REQUIRE(app::loadField(root / "run-20260101-000001", "volume_e") == nullptr);
        const auto far = app::loadFar(root / "run-20260101-000001");
        REQUIRE(far.size() == 1);
        REQUIRE(far[0].field.u[1] == Catch::Approx(2.5));
        REQUIRE(far[0].figures.gain == Catch::Approx(1.25));
      }
    }
    WHEN("all but the newest run are pruned") {
      const auto freed = app::pruneFields(root, 1);
      THEN("their field data is gone, the S11 stays, and the newest keeps its fields") {
        REQUIRE(freed > 0);
        REQUIRE(!fs::exists(root / "run-20260101-000001" / "fields"));
        REQUIRE(fs::exists(root / "run-20260101-000001" / "s11.s1p"));
        REQUIRE(fs::exists(root / "run-20260103-000001" / "fields"));
        const auto runs = app::scanRuns(root);
        REQUIRE(runs[0].fields);
        REQUIRE(!runs[1].fields);
      }
    }
  }
}

SCENARIO("The antenna part the user picked is kept in the project's selection file") {
  GIVEN("a selection with an antenna part") {
    const fs::path file = scratch("selection_antenna.json");
    fs::remove(file);
    app::StoredSelection st;
    st.antennaRef = "ANT7";
    app::saveSelection(file, st);
    THEN("it is read back, and a file without one gives an empty pick") {
      REQUIRE(app::loadSelection(file).antennaRef == "ANT7");
      REQUIRE(app::loadSelection(scratch("does_not_exist.json")).antennaRef.empty());
    }
  }
}
