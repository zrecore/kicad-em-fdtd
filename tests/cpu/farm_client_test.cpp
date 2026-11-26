#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <thread>

#include "app/farm_client.hpp"

using Catch::Matchers::ContainsSubstring;

namespace {
bool waitFor(const std::function<bool()>& ok, double seconds) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
  while (std::chrono::steady_clock::now() < end) {
    if (ok())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return ok();
}
} // namespace

SCENARIO("The window reads the farm tool's JSON", "[farm]") {
  GIVEN("a status, a queue and a member list as the tool prints them") {
    farm::Snapshot s;
    std::string error;
    THEN("the roles come from the status") {
      REQUIRE(farm::parseStatus(
          R"({"home":"/h","enabled":{"coordinator":true,"worker":false,"client":true}})", s, error));
      REQUIRE(s.haveStatus);
      REQUIRE(s.coordinator);
      REQUIRE_FALSE(s.worker);
      REQUIRE(s.client);
      REQUIRE(s.home == "/h");
    }
    THEN("jobs carry their percentage, or -1 when the solver gave none") {
      REQUIRE(farm::parseQueue(
          R"({"devices":[{"worker":"w","name":"RTX","state":"busy","mcells_per_s":1800.5}],
              "jobs":[{"job":"a","label":"x","state":"running","attempts":1,"worker":"w","progress":"p","percent":37,"reason":""},
                      {"job":"b","label":"y","state":"queued","attempts":0,"worker":null,"progress":"","percent":null,"reason":""}]})",
          s, error));
      REQUIRE(s.devices.size() == 1);
      REQUIRE(s.devices[0].mcellsPerS == 1800.5);
      REQUIRE(s.jobs.size() == 2);
      REQUIRE(s.jobs[0].percent == 37);
      REQUIRE(s.jobs[1].percent == -1);
      REQUIRE(s.jobs[1].worker.empty());
    }
    THEN("members list what each may do") {
      REQUIRE(farm::parseMembers(
          R"({"members":[{"key":"client:a","fingerprint":"ff","may":["submit","cancel"],"revoked":true}]})",
          s, error));
      REQUIRE(s.members.size() == 1);
      REQUIRE(s.members[0].may == "submit, cancel");
      REQUIRE(s.members[0].revoked);
    }
    THEN("bad input is refused with a reason, never a crash") {
      REQUIRE_FALSE(farm::parseStatus("not json", s, error));
      REQUIRE_FALSE(error.empty());
      REQUIRE_FALSE(farm::parseQueue(R"({"jobs":5})", s, error));
      REQUIRE_FALSE(farm::parseMembers("{}", s, error));
    }
    THEN("the solver's progress line gives a percentage") {
      REQUIRE(farm::percentFromProgressLine("progress: 41200 steps, about 37%, 312 s") == 37);
      REQUIRE(farm::percentFromProgressLine("progress: 5 steps, about 100%, 1 s") == 100);
      REQUIRE(farm::percentFromProgressLine("starting the solver") == -1);
      REQUIRE(farm::phaseWords("progress: 41200 steps, about 37%, 312 s") == "step 41200, 312 s so far");
      REQUIRE(farm::phaseWords("fetching the input") == "fetching the input");
    }
  }
}

SCENARIO("The window runs the farm tool without a shell and never waits for ever", "[farm]") {
  THEN("output is captured and the exit code returned") {
    const auto o = farm::runCapture({"/bin/sh", "-c", "echo hello; echo oops 1>&2; exit 3"}, 5);
    REQUIRE(o.code == 3);
    REQUIRE_THAT(o.text, ContainsSubstring("hello"));
    REQUIRE_THAT(o.text, ContainsSubstring("oops"));
  }
  THEN("an argument with shell characters is one argument") {
    const auto o = farm::runCapture({"/bin/echo", "a;b $HOME `x`"}, 5);
    REQUIRE(o.text == "a;b $HOME `x`\n");
  }
  THEN("a command that does not exist reports why") {
    const auto o = farm::runCapture({"/no/such/program"}, 5);
    REQUIRE(o.code == -1);
    REQUIRE_FALSE(o.text.empty());
  }
  THEN("a command that runs too long is killed at the timeout") {
    const auto t0 = std::chrono::steady_clock::now();
    const auto o = farm::runCapture({"/bin/sleep", "30"}, 0.4);
    REQUIRE(o.timedOut);
    REQUIRE(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 5.0);
  }
}

SCENARIO("The client polls the tool only while the tab is showing, and runs actions in the background",
         "[farm]") {
  GIVEN("a stand-in for the farm tool") {
    setenv("EMFDTD_FARM_COMMAND", FDTD_FARM_STUB, 1);
    const std::string log = std::string(FDTD_TEST_TMP) + "/farm_stub_calls.log";
    std::remove(log.c_str());
    setenv("EMFDTD_FARM_STUB_LOG", log.c_str(), 1);
    farm::Client client;
    WHEN("the tab is shown") {
      client.touch();
      REQUIRE(waitFor([&] { return client.latest().haveQueue && client.latest().haveMembers; }, 10));
      THEN("roles, devices, jobs and members are read, with the queue only for a client") {
        const auto s = client.latest();
        REQUIRE(s.coordinator);
        REQUIRE(s.client);
        REQUIRE_FALSE(s.worker);
        REQUIRE(s.devices.size() == 2);
        REQUIRE(s.jobs.size() == 3);
        REQUIRE(s.jobs[0].percent == 37);
        REQUIRE(s.jobs[2].reason.find("gave up") != std::string::npos);
        REQUIRE(s.members.size() == 2);
      }
    }
    WHEN("an action is started") {
      client.run({"cancel", "9cf37054e5064a39b9298f6ec19fe720"}, "Cancel");
      REQUIRE(waitFor([&] { return !client.action().running && !client.action().text.empty(); }, 10));
      THEN("its output and exit code are kept") {
        REQUIRE(client.action().code == 0);
        REQUIRE_THAT(client.action().text, ContainsSubstring("stub: cancel 9cf37054"));
      }
    }
    WHEN("an invitation is started") {
      client.startInvite("new-box", "worker", "192.0.2.1");
      REQUIRE(waitFor([&] { return !client.invite().fingerprint.empty(); }, 10));
      THEN("the code and fingerprint are shown, and stopping it ends the child") {
        const auto i = client.invite();
        REQUIRE(i.running);
        REQUIRE(i.code == "ABCD-EFGH-JKLM");
        REQUIRE(i.fingerprint == "1111-2222-3333-4444");
        REQUIRE_THAT(i.listening, ContainsSubstring("192.0.2.1:7421"));
        client.stopInvite();
        REQUIRE_FALSE(client.invite().running);
      }
    }
    unsetenv("EMFDTD_FARM_COMMAND");
    unsetenv("EMFDTD_FARM_STUB_LOG");
  }
}

SCENARIO("A client that nobody touches does not call the tool", "[farm]") {
  setenv("EMFDTD_FARM_COMMAND", FDTD_FARM_STUB, 1);
  const std::string log = std::string(FDTD_TEST_TMP) + "/farm_stub_idle.log";
  std::remove(log.c_str());
  setenv("EMFDTD_FARM_STUB_LOG", log.c_str(), 1);
  {
    farm::Client client;
    std::this_thread::sleep_for(std::chrono::milliseconds(2200));
    std::ifstream in(log);
    REQUIRE_FALSE(in.good()); // the tool was never run
  }
  unsetenv("EMFDTD_FARM_COMMAND");
  unsetenv("EMFDTD_FARM_STUB_LOG");
}
