// Checkpoint and resume of a run (docs/checkpoint.md): a run that is stopped, saved and resumed must give the
// same numbers, bit for bit, as the same run left alone.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstring>

#include "../checkpoint_model.hpp"
#include "sim/simulation.hpp"

using namespace checkpoint_test;

SCENARIO("A run that is checkpointed and resumed gives the same numbers as one left alone", "[checkpoint]") {
  GIVEN("a short run with a checkpoint taken at step 320") {
    const auto model = loadModel();
    sim::RunOptions opt = options();
    sim::Checkpoint saved;
    std::vector<float> ezWhole;
    opt.onFinish = [&](sim::Engine& e) {
      const auto f = e.field(fdtd::Kind::E, fdtd::Z);
      ezWhole.assign(f.begin(), f.end());
    };
    opt.checkpointDue = [](std::size_t steps) { return steps == 320; };
    opt.onCheckpoint = [&](const sim::Checkpoint& cp) { saved = cp; };
    const auto whole = sim::runOnePort(model, opt, sim::makeCpuEngine);
    REQUIRE(whole.steps == 640);
    REQUIRE(saved.steps == 320);

    WHEN("a new run resumes from that checkpoint") {
      sim::RunOptions again = options();
      std::vector<float> ezResumed;
      again.onFinish = [&](sim::Engine& e) {
        const auto f = e.field(fdtd::Kind::E, fdtd::Z);
        ezResumed.assign(f.begin(), f.end());
      };
      again.resume = &saved;
      const auto resumed = sim::runOnePort(model, again, sim::makeCpuEngine);
      THEN("the steps, port voltage, source, every strip and Ez are identical, not merely close") {
        REQUIRE(resumed.steps == whole.steps);
        REQUIRE(resumed.resumedFromStep == 320);
        REQUIRE(same(resumed.portVoltage, whole.portVoltage));
        REQUIRE(same(resumed.portSource, whole.portSource));
        REQUIRE(resumed.stripVoltage.size() == whole.stripVoltage.size());
        for (std::size_t k = 0; k < whole.stripVoltage.size(); ++k)
          REQUIRE(same(resumed.stripVoltage[k], whole.stripVoltage[k]));
        REQUIRE(ezResumed.size() == ezWhole.size());
        REQUIRE(std::memcmp(ezResumed.data(), ezWhole.data(), ezWhole.size() * sizeof(float)) == 0);
      }
      AND_THEN("S11 is identical at every frequency") {
        REQUIRE(resumed.s.s11.size() == whole.s.s11.size());
        for (std::size_t i = 0; i < whole.s.s11.size(); ++i)
          REQUIRE(resumed.s.s11[i] == whole.s.s11[i]);
      }
    }
  }
}

SCENARIO("A pause request writes a checkpoint and ends the run without a result", "[checkpoint]") {
  GIVEN("a run asked to pause at step 320") {
    const auto model = loadModel();
    sim::RunOptions opt = options();
    sim::Checkpoint saved;
    bool finished = false;
    opt.onFinish = [&](sim::Engine&) { finished = true; };
    opt.pauseRequested = [&] { return saved.steps == 0; }; // true until the first checkpoint exists
    opt.checkpointDue = [](std::size_t) { return false; };
    opt.onCheckpoint = [&](const sim::Checkpoint& cp) { saved = cp; };
    opt.shouldStop = [] { return false; };
    WHEN("it runs") {
      const auto paused = sim::runOnePort(model, opt, sim::makeCpuEngine);
      THEN("it stopped at the first quiet point, said so, and did not compute S11 or call onFinish") {
        REQUIRE(paused.paused);
        REQUIRE(paused.steps == 16);
        REQUIRE(saved.steps == 16);
        REQUIRE(paused.s.s11.empty());
        REQUIRE_FALSE(finished);
      }
    }
  }
}

SCENARIO("A checkpoint that does not fit the run is refused with a reason", "[checkpoint]") {
  GIVEN("a checkpoint of a run with CPML, and a run without it") {
    const auto model = loadModel();
    sim::RunOptions opt = options();
    opt.maxSteps = 64;
    sim::Checkpoint saved;
    opt.checkpointDue = [](std::size_t steps) { return steps == 32; };
    opt.onCheckpoint = [&](const sim::Checkpoint& cp) { saved = cp; };
    sim::runOnePort(model, opt, sim::makeCpuEngine);
    REQUIRE(saved.steps == 32);
    WHEN("the options differ (no CPML layers)") {
      sim::RunOptions other = options();
      other.cpmlCells = 0;
      other.resume = &saved;
      THEN("the resume throws and says what differs") {
        REQUIRE_THROWS_WITH(sim::runOnePort(model, other, sim::makeCpuEngine),
                            Catch::Matchers::ContainsSubstring("CPML"));
      }
    }
    WHEN("the sample interval differs") {
      sim::RunOptions other = options();
      other.sampleEvery = 8;
      other.resume = &saved;
      THEN("the resume throws") {
        REQUIRE_THROWS_WITH(sim::runOnePort(model, other, sim::makeCpuEngine),
                            Catch::Matchers::ContainsSubstring("sample interval"));
      }
    }
    WHEN("the bytes are cut short") {
      sim::Checkpoint cut = saved;
      cut.engine.resize(cut.engine.size() / 2);
      sim::RunOptions other = options();
      other.resume = &cut;
      THEN("the resume throws instead of reading past the end") {
        REQUIRE_THROWS_WITH(sim::runOnePort(model, other, sim::makeCpuEngine),
                            Catch::Matchers::ContainsSubstring("truncated"));
      }
    }
  }
}
