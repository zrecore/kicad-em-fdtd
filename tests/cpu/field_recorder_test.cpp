// BDD scenarios for the field data a run keeps on request (docs/ux.md, "Saved runs"; docs/PLAN.md, Next steps
// 16): the recorder shared by the window and by `emfdtd --keep-fields`.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <filesystem>
#include <mutex>

#include "app/field_recorder.hpp"
#include "app/run_store.hpp"
#include "app/settings.hpp"
#include "board/snapshot.hpp"

namespace fs = std::filesystem;

namespace {
const app::Catalog& catalog() {
  static const app::Catalog c = app::Catalog::load(std::string(FDTD_DATA_DIR) + "/radio_profiles.json");
  return c;
}

board::Snapshot loadBoard() {
  return board::parseSnapshot(R"({"format":"kicad-em-fdtd.snapshot","version":1,"board":{"name":"load"},
    "outline":[[0,0],[0.01,0],[0.01,0.01],[0,0.01]],
    "stackup":[{"name":"F.Cu","kind":"copper"},
               {"name":"core","kind":"dielectric","thickness":0.0008,"epsilon_r":4.4,"loss_tangent":0.0}],
    "copper":[{"layer":"F.Cu","owner":"","net":"A","polygon":[[0.0012,0.0012],[0.0029,0.0012],[0.0029,0.0088],[0.0012,0.0088]]},
              {"layer":"F.Cu","owner":"","net":"B","polygon":[[0.0051,0.0012],[0.0088,0.0012],[0.0088,0.0088],[0.0051,0.0088]]}],
    "vias":[],"components":[],
    "ports":[{"name":"P1","layer":"F.Cu","a":[0.003,0.004],"b":[0.005,0.004],"width":0.0022,"impedance":50},
             {"name":"load","layer":"F.Cu","a":[0.003,0.007],"b":[0.005,0.007],"width":0.0022,"impedance":100}]})");
}

app::Prepared prepared(const board::Snapshot& snap) {
  app::Settings s(catalog());
  s.set("backend", std::string("cpu"));
  s.set("boundary_distance_wavelengths", 0.05); // a small domain: the board is 10 mm and the band 2.4 GHz
  s.set("max_steps", 400.0);
  return app::prepare(snap, s, board::Selection{});
}

fs::path scratch(const std::string& name) {
  const fs::path dir = fs::path(FDTD_TEST_TMP) / "field_recorder" / name;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}
} // namespace

SCENARIO("A run that records its fields saves them, and the saved run reads back") {
  GIVEN("a 100 ohm load across a 50 ohm port, solved once plain and once with the field recorder") {
    const auto snap = loadBoard();
    auto plain = prepared(snap);
    const sim::RunResult base = sim::runOnePort(plain.model, plain.options, sim::makeCpuEngine);

    auto withFields = prepared(snap);
    app::FieldRecorder rec;
    std::mutex mu;
    rec.layout(snap, withFields);
    rec.allocate();
    rec.attach(withFields.options, withFields.model.grid, mu, nullptr);
    const sim::RunResult r = sim::runOnePort(withFields.model, withFields.options, sim::makeCpuEngine);

    THEN("recording changes the S11 by less than 1e-12 (it only reads the fields)") {
      REQUIRE(r.s.s11.size() == base.s.s11.size());
      for (std::size_t i = 0; i < r.s.s11.size(); ++i)
        REQUIRE(std::abs(r.s.s11[i] - base.s.s11[i]) < 1e-12);
    }
    WHEN("the fields are saved with a manifest and the run folder is scanned") {
      const fs::path root = scratch("saved");
      const fs::path dir = root / "run-20260101-000000";
      fs::create_directories(dir);
      rec.farResults = rec.computeFar(r, withFields.plan.portImpedanceOhm);
      rec.save(dir);
      app::RunManifest m;
      m.id = dir.filename().string();
      m.fields = true;
      app::writeManifest(dir, m);
      const auto runs = app::scanRuns(root);
      THEN("the run is listed with its fields, and every field file is there") {
        REQUIRE(runs.size() == 1);
        REQUIRE(runs[0].fields);
        for (const char* name : {"slice_e", "volume_e", "power_eh"})
          REQUIRE(fs::exists(dir / "fields" / (std::string(name) + ".f16")));
      }
      AND_THEN("the slice and the volume read back with the recorded frequencies and a non-zero field") {
        for (const char* name : {"slice_e", "volume_e", "power_eh"}) {
          const auto acc = app::loadField(dir, name);
          REQUIRE(acc != nullptr);
          REQUIRE(acc->frequencies() == rec.frequencies.size());
          double sum = 0;
          for (std::size_t v = 0; v < acc->voxels(); ++v)
            sum += std::abs(acc->phasor(acc->frequencies() / 2, v));
          REQUIRE(sum > 0);
        }
        REQUIRE(app::loadField(dir, "volume_e")->voxels() == rec.dv.voxels());
      }
      AND_THEN("the estimated size is within 10 percent plus 1 MB of the folder's real size") {
        const double real = static_cast<double>(app::folderBytes(dir / "fields"));
        const double est = static_cast<double>(app::estimateFieldBytes(snap, withFields));
        REQUIRE(est >= real * 0.9 - 1e6);
        REQUIRE(est <= real * 1.1 + 1e6);
      }
    }
  }
}
