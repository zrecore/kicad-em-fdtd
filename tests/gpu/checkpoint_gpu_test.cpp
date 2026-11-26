// Checkpoint and resume on the GPU (docs/checkpoint.md): a resumed run must equal the uninterrupted one bit
// for bit, on the same device. Runs under the GPU lock like the other [gpu] tests.
#include <catch2/catch_test_macros.hpp>
#include <cstring>

#include "../checkpoint_model.hpp"
#include "fdtd_vk/gpu_engine.hpp"
#include "sim/simulation.hpp"

using namespace checkpoint_test;

namespace {
using Fields = std::vector<std::vector<float>>;

Fields grabFields(sim::Engine& e) {
  Fields out;
  for (fdtd::Kind k : {fdtd::Kind::E, fdtd::Kind::H})
    for (fdtd::Axis a : {fdtd::X, fdtd::Y, fdtd::Z}) {
      const auto f = e.field(k, a);
      out.emplace_back(f.begin(), f.end());
    }
  return out;
}
} // namespace

SCENARIO("A GPU run that is checkpointed and resumed equals one left alone, bit for bit",
         "[gpu][checkpoint]") {
  GIVEN("a short run on this machine's GPU with a checkpoint at step 320") {
    vkcore::Context ctx;
    const sim::EngineFactory factory = [&](fdtd::Solver<float>& s) {
      return std::make_unique<fdtd_vk::GpuEngine>(ctx, s);
    };
    const auto model = loadModel();
    sim::RunOptions opt = options();
    sim::Checkpoint saved;
    Fields whole;
    opt.onFinish = [&](sim::Engine& e) { whole = grabFields(e); };
    opt.checkpointDue = [](std::size_t steps) { return steps == 320; };
    opt.onCheckpoint = [&](const sim::Checkpoint& cp) { saved = cp; };
    const auto base = sim::runOnePort(model, opt, factory);
    REQUIRE(base.steps == 640);
    REQUIRE(saved.steps == 320);

    WHEN("a new run on a fresh engine resumes from it") {
      sim::RunOptions again = options();
      Fields resumedFields;
      again.onFinish = [&](sim::Engine& e) { resumedFields = grabFields(e); };
      again.resume = &saved;
      const auto resumed = sim::runOnePort(model, again, factory);
      THEN("the port voltage, source, strips and S11 are identical") {
        REQUIRE(resumed.steps == base.steps);
        REQUIRE(resumed.resumedFromStep == 320);
        REQUIRE(same(resumed.portVoltage, base.portVoltage));
        REQUIRE(same(resumed.portSource, base.portSource));
        for (std::size_t k = 0; k < base.stripVoltage.size(); ++k)
          REQUIRE(same(resumed.stripVoltage[k], base.stripVoltage[k]));
        for (std::size_t i = 0; i < base.s.s11.size(); ++i)
          REQUIRE(resumed.s.s11[i] == base.s.s11[i]);
      }
      AND_THEN("all six field arrays are identical") {
        REQUIRE(resumedFields.size() == 6);
        for (std::size_t c = 0; c < 6; ++c) {
          REQUIRE(resumedFields[c].size() == whole[c].size());
          REQUIRE(std::memcmp(resumedFields[c].data(), whole[c].data(), whole[c].size() * sizeof(float)) ==
                  0);
        }
      }
    }
  }
}
