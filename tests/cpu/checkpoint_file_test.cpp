// The checkpoint file (docs/checkpoint.md): intact files round-trip, damaged ones are recognized, two
// generations alternate, and a checkpoint that does not belong to this run, device or binary is refused.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>
#include <fstream>

#include "app/checkpoint.hpp"

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;
using namespace app::checkpoint;

namespace {

fs::path scratch(const std::string& name) {
  const fs::path dir = fs::path(FDTD_TEST_TMP) / "checkpoint" / name;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

Identity identity() {
  Identity id;
  id.runHash = "0123456789abcdef0123456789abcdef";
  id.backend = "gpu";
  id.deviceUuid = "190a0000010000000000000000000000";
  id.deviceName = "Apple M1 Pro";
  id.driver = "KosmicKrisp 1.4";
  id.binaryVersion = "0.1.0";
  id.nodes = {475, 328, 128};
  id.dt = 7.75421e-14;
  id.runDir = "/results/run-1";
  return id;
}

sim::Checkpoint state(std::size_t steps) {
  sim::Checkpoint cp;
  cp.steps = steps;
  for (std::size_t i = 0; i < 1000; ++i) {
    cp.host.push_back(static_cast<std::uint8_t>(i * 7 + steps));
    cp.engine.push_back(static_cast<std::uint8_t>(i * 13 + steps));
  }
  return cp;
}

void flipByte(const fs::path& file, std::streamoff at) {
  std::fstream f(file, std::ios::in | std::ios::out | std::ios::binary);
  f.seekg(at);
  char c;
  f.get(c);
  c = static_cast<char>(c ^ 0x5A);
  f.seekp(at);
  f.put(c);
}

} // namespace

SCENARIO("CRC-32C matches the standard check value", "[checkpoint]") {
  GIVEN("the ASCII digits 123456789, the standard check input") {
    const std::string text = "123456789";
    const std::vector<std::uint8_t> bytes(text.begin(), text.end());
    THEN("the CRC-32C is exactly 0xE3069283, the published check value") {
      REQUIRE(crc32c(bytes) == 0xE3069283u);
    }
  }
}

SCENARIO("A saved checkpoint comes back exactly", "[checkpoint]") {
  GIVEN("a store with one checkpoint") {
    Store store(scratch("roundtrip"));
    store.save(identity(), state(320));
    WHEN("it is loaded as the same run on the same device") {
      const Loaded got = store.load(identity());
      THEN("the steps, both state sections and the results folder are the same") {
        REQUIRE(got.state.steps == 320);
        REQUIRE(got.state.host == state(320).host);
        REQUIRE(got.state.engine == state(320).engine);
        REQUIRE(got.identity.runDir == "/results/run-1");
      }
    }
  }
}

SCENARIO("Two generations alternate and the newer one wins", "[checkpoint]") {
  GIVEN("three saves in a row") {
    Store store(scratch("generations"));
    store.save(identity(), state(100));
    store.save(identity(), state(200));
    store.save(identity(), state(300));
    THEN("both files exist and the load gives step 300") {
      REQUIRE(fs::exists(store.path('a')));
      REQUIRE(fs::exists(store.path('b')));
      REQUIRE(store.load(identity()).state.steps == 300);
    }
    AND_WHEN("the newest file is damaged") {
      const char newest = store.load(identity()).generation;
      flipByte(store.path(newest), 600);
      THEN("the older generation is used") {
        Store reader(store.dir());
        REQUIRE(reader.load(identity()).state.steps == 200);
      }
    }
    AND_WHEN("the newest file is cut short") {
      const char newest = store.load(identity()).generation;
      fs::resize_file(store.path(newest), fs::file_size(store.path(newest)) / 2);
      THEN("the older generation is used") {
        Store reader(store.dir());
        REQUIRE(reader.load(identity()).state.steps == 200);
      }
    }
    AND_WHEN("a new store resumes and saves again") {
      Store reader(store.dir());
      REQUIRE(reader.load(identity()).state.steps == 300);
      reader.save(identity(), state(400));
      THEN("it replaced the older generation, so the newest two are 300 and 400") {
        REQUIRE(Store(store.dir()).load(identity()).state.steps == 400);
        flipByte(reader.path(reader.load(identity()).generation), 600);
        REQUIRE(Store(store.dir()).load(identity()).state.steps == 300);
      }
    }
  }
}

SCENARIO("A checkpoint that does not belong to this run is refused with the reason", "[checkpoint]") {
  GIVEN("a store with a checkpoint") {
    Store store(scratch("refused"));
    store.save(identity(), state(320));
    WHEN("the run's inputs differ") {
      Identity other = identity();
      other.runHash = "ffffffffffffffffffffffffffffffff";
      THEN("it says the inputs differ") {
        REQUIRE_THROWS_WITH(store.load(other), ContainsSubstring("other inputs"));
      }
    }
    WHEN("the device differs") {
      Identity other = identity();
      other.deviceUuid = "99cbe8b64e12b2f55c408e988415f423";
      other.deviceName = "NVIDIA GeForce RTX 4060 Ti";
      THEN("it names both devices") {
        REQUIRE_THROWS_WITH(store.load(other), ContainsSubstring("only on the device that wrote it"));
        REQUIRE_THROWS_WITH(store.load(other), ContainsSubstring("RTX 4060 Ti"));
      }
    }
    WHEN("the driver differs") {
      Identity other = identity();
      other.driver = "MoltenVK 1.4.2";
      THEN("it names the driver") {
        REQUIRE_THROWS_WITH(store.load(other), ContainsSubstring("driver"));
      }
    }
    WHEN("the binary version differs") {
      Identity other = identity();
      other.binaryVersion = "0.2.0";
      THEN("it says which versions") {
        REQUIRE_THROWS_WITH(store.load(other), ContainsSubstring("0.1.0"));
      }
    }
    WHEN("the backend differs") {
      Identity other = identity();
      other.backend = "cpu";
      THEN("it names the backends") {
        REQUIRE_THROWS_WITH(store.load(other), ContainsSubstring("backend"));
      }
    }
  }
  GIVEN("an empty folder") {
    Store store(scratch("empty"));
    THEN("the load says there is nothing to resume") {
      REQUIRE_THROWS_WITH(store.load(identity()), ContainsSubstring("is missing"));
    }
  }
  GIVEN("a folder with a file that is not a checkpoint") {
    const fs::path dir = scratch("junk");
    std::ofstream(dir / "ckpt.a") << "hello";
    Store store(dir);
    THEN("it is called damaged") {
      REQUIRE_THROWS_WITH(store.load(identity()), ContainsSubstring("is damaged"));
    }
  }
}

SCENARIO("A fresh run clears the files of an earlier one", "[checkpoint]") {
  GIVEN("a store with two generations and a stop file") {
    Store store(scratch("clear"));
    store.save(identity(), state(1));
    store.save(identity(), state(2));
    std::ofstream(store.dir() / "stop") << "stop";
    WHEN("it is cleared") {
      store.clear();
      THEN("nothing is left") {
        REQUIRE_FALSE(fs::exists(store.path('a')));
        REQUIRE_FALSE(fs::exists(store.path('b')));
        REQUIRE_FALSE(fs::exists(store.dir() / "stop"));
      }
    }
  }
}
