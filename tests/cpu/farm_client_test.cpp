#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
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
    const auto o = farm::runCapture({"/bin/sh", "-c", "echo hello; echo oops 1>&2; exit 3"}, 30);
    REQUIRE(o.code == 3);
    REQUIRE_THAT(o.text, ContainsSubstring("hello"));
    REQUIRE_THAT(o.text, ContainsSubstring("oops"));
  }
  THEN("an argument with shell characters is one argument") {
    const auto o = farm::runCapture({"/bin/echo", "a;b $HOME `x`"}, 30);
    REQUIRE(o.text == "a;b $HOME `x`\n");
  }
  THEN("a command that does not exist reports why") {
    const auto o = farm::runCapture({"/no/such/program"}, 30);
    REQUIRE(o.code == -1);
    REQUIRE_FALSE(o.text.empty());
  }
  THEN("a command that runs too long is killed at the timeout") {
    const auto t0 = std::chrono::steady_clock::now();
    const auto o = farm::runCapture({"/bin/sleep", "30"}, 0.4);
    REQUIRE(o.timedOut);
    REQUIRE(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 15.0);
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
      REQUIRE(waitFor([&] { return client.latest().haveQueue && client.latest().haveMembers; }, 30));
      THEN("roles, devices, jobs and members are read, with the queue only for a client") {
        const auto s = client.latest();
        REQUIRE(s.coordinator);
        REQUIRE(s.client);
        REQUIRE_FALSE(s.worker);
        REQUIRE(s.devices.size() == 2);
        REQUIRE(s.jobs.size() == 4);
        REQUIRE(s.jobs[0].percent == 37);
        REQUIRE(s.jobs[3].reason.find("gave up") != std::string::npos);
        REQUIRE(s.members.size() == 2);
      }
    }
    WHEN("an action is started") {
      client.run({"cancel", "9cf37054e5064a39b9298f6ec19fe720"}, "Cancel");
      REQUIRE(waitFor([&] { return !client.action().running && !client.action().text.empty(); }, 30));
      THEN("its output and exit code are kept") {
        REQUIRE(client.action().code == 0);
        REQUIRE_THAT(client.action().text, ContainsSubstring("stub: cancel 9cf37054"));
      }
    }
    WHEN("an invitation is started") {
      client.startInvite("new-box", "worker", "192.0.2.1");
      REQUIRE(waitFor([&] { return !client.invite().fingerprint.empty(); }, 30));
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

SCENARIO("A run that goes to the farm is described by the window's settings and treatments", "[farm]") {
  GIVEN("values written the way the --part option reads them") {
    THEN("engineering notation, never an exponent") {
      REQUIRE(farm::siValue(1e-7) == "100n");
      REQUIRE(farm::siValue(600) == "600");
      REQUIRE(farm::siValue(2e-13) == "0.2p");
      REQUIRE(farm::siValue(4700) == "4.7k");
      REQUIRE(farm::siValue(0) == "0");
    }
  }
  GIVEN("part and body treatments") {
    board::PartTreatment values;
    values.state = board::PartTreatment::State::Values;
    values.r = 600;
    values.c = 0.2e-12;
    board::PartTreatment open;
    open.state = board::PartTreatment::State::Open;
    board::PartTreatment fit;
    fit.fitDnp = true;
    board::BodyTreatment metal{board::BodyKind::Metal}, ignore{board::BodyKind::Ignore},
        diel{board::BodyKind::Dielectric, 3.5, 0.0}, lossy{board::BodyKind::Dielectric, 3.5, 0.02};
    THEN("they become the strings the tool and the worker accept") {
      REQUIRE(farm::partTreatmentText(values) == "r600,c0.2p");
      REQUIRE(farm::partTreatmentText(open) == "open");
      REQUIRE(farm::partTreatmentText(fit) == "fit");
      REQUIRE(farm::partTreatmentText(board::PartTreatment{}).empty()); // Auto: nothing to send
      REQUIRE(farm::bodyTreatmentText(metal) == "metal");
      REQUIRE(farm::bodyTreatmentText(ignore) == "ignore");
      REQUIRE(farm::bodyTreatmentText(diel) == "dielectric:3.5");
      REQUIRE(farm::bodyTreatmentText(lossy) == "dielectric:3.5:0.02");
    }
  }
  GIVEN("a request with settings, a refine box, treatments and nets") {
    farm::RunRequest r;
    r.snapshot = "/p/board.json";
    r.label = "my board";
    r.profile = "bluetooth";
    r.variant = "";
    r.sets = {{"mesh_preset", "medium"}, {"decay_threshold", "0.001"}};
    r.refineMm = {{53.5, 119, 56.5, 124, 0.06}};
    r.parts = {{"R5", "short"}};
    r.bodies = {{"MASK", "dielectric:1.8"}};
    r.nets = {"RF", "GND"};
    THEN("the arguments are exactly what `submit` takes, with --json last") {
      const std::vector<std::string> want = {"submit",    "/p/board.json",
                                             "--profile", "bluetooth",
                                             "--set",     "mesh_preset=medium",
                                             "--set",     "decay_threshold=0.001",
                                             "--refine",  "53.5",
                                             "119",       "56.5",
                                             "124",       "0.06",
                                             "--part",    "R5=short",
                                             "--body",    "MASK=dielectric:1.8",
                                             "--nets",    "RF,GND",
                                             "--label",   "my board",
                                             "--json"};
      REQUIRE(farm::submitArgs(r) == want);
    }
    THEN("a variant joins the profile") {
      r.variant = "2g4";
      REQUIRE(farm::submitArgs(r)[3] == "bluetooth:2g4");
    }
  }
  GIVEN("selections") {
    board::Selection all;
    board::Selection picked;
    picked.all = false;
    board::Selection nets;
    nets.all = false;
    nets.netFilter = true;
    THEN("a hand-picked component list cannot go; everything else can") {
      REQUIRE(farm::farmBlockedReason(all).empty());
      REQUIRE(farm::farmBlockedReason(nets).empty());
      REQUIRE_FALSE(farm::farmBlockedReason(picked).empty());
    }
  }
  THEN("the job ids come out of the submit answer, and a bad answer is refused") {
    std::vector<std::string> ids;
    std::string error;
    REQUIRE(farm::parseSubmitIds(R"({"jobs":["aaaa","bbbb"]})", ids, error));
    REQUIRE(ids.size() == 2);
    REQUIRE_FALSE(farm::parseSubmitIds(R"({"jobs":[]})", ids, error));
    REQUIRE_FALSE(farm::parseSubmitIds("oops", ids, error));
  }
}

SCENARIO("Running jobs show how long they have run, and fetched results get a stable folder", "[farm]") {
  THEN("seconds, minutes, hours") {
    REQUIRE(farm::runningFor(1000, 1045) == "running 45 s");
    REQUIRE(farm::runningFor(1000, 1372) == "running 6 min");
    REQUIRE(farm::runningFor(1000, 1000 + 3600 + 12 * 60 + 5) == "running 1 h 12 min");
    REQUIRE(farm::runningFor(0, 1000).empty());
    REQUIRE(farm::runningFor(2000, 1000).empty()); // a clock that is behind: say nothing
  }
  THEN("the folder is named after the first eight characters of the job id") {
    REQUIRE(farm::fetchFolderName("9cf37054e5064a39b9298f6ec19fe720") == "run-farm-9cf37054");
  }
  THEN("a queue row carries the start time when the job has one") {
    farm::Snapshot s;
    std::string error;
    REQUIRE(farm::parseQueue(
        R"({"devices":[],"jobs":[{"job":"a","label":"x","state":"running","attempts":1,"worker":"w","progress":"","percent":null,"started":1795670206.5,"reason":""},
                               {"job":"b","label":"y","state":"queued","attempts":0,"worker":null,"progress":"","percent":null,"started":null,"reason":""}]})",
        s, error));
    REQUIRE(s.jobs[0].started == 1795670206.5);
    REQUIRE(s.jobs[1].started == 0);
  }
}

SCENARIO("Where a run goes is remembered in the farm folder, and the client role is read from a file",
         "[farm]") {
  const std::string home = std::string(FDTD_TEST_TMP) + "/farm_runon_home";
  std::filesystem::remove_all(home);
  setenv("EMFDTD_FARM_HOME", home.c_str(), 1);
  THEN("the default is this GPU and the client role is off") {
    REQUIRE(farm::loadRunOn() == "gpu");
    REQUIRE_FALSE(farm::clientEnabled());
  }
  THEN("the choice survives, and anything else means this GPU") {
    farm::saveRunOn("farm");
    REQUIRE(farm::loadRunOn() == "farm");
    farm::saveRunOn("somewhere else");
    REQUIRE(farm::loadRunOn() == "gpu");
  }
  THEN("the choice never touches farm.json, where the role switches live") {
    std::filesystem::create_directories(home);
    std::ofstream(home + "/farm.json") << R"({"enabled":{"coordinator":false,"worker":false,"client":true}})";
    farm::saveRunOn("farm");
    REQUIRE(farm::clientEnabled());
    std::ifstream in(home + "/farm.json");
    std::string text((std::istreambuf_iterator<char>(in)), {});
    REQUIRE(text.find("run_on") == std::string::npos);
  }
  unsetenv("EMFDTD_FARM_HOME");
}

SCENARIO("The window's submit and fetch run the tool and keep its answers", "[farm]") {
  setenv("EMFDTD_FARM_COMMAND", FDTD_FARM_STUB, 1);
  farm::Client client;
  WHEN("a run is submitted") {
    farm::RunRequest r;
    r.snapshot = "/p/board.json";
    r.profile = "wifi";
    client.run(farm::submitArgs(r), "Run on the farm");
    REQUIRE(waitFor([&] { return !client.action().running && !client.action().text.empty(); }, 10));
    THEN("the job id is in the output") {
      std::vector<std::string> ids;
      std::string error;
      REQUIRE(
          farm::parseSubmitIds(client.action().text.substr(0, client.action().text.find('\n')), ids, error));
      REQUIRE(ids.front() == "bbbb1111cccc2222dddd3333eeee4444");
    }
  }
  WHEN("results are fetched into a folder") {
    const std::string dest = std::string(FDTD_TEST_TMP) + "/farm_fetch_dest";
    std::filesystem::remove_all(dest);
    client.run({"results", "9cf37054e5064a39b9298f6ec19fe720", "--dest", dest}, "Fetch results");
    REQUIRE(waitFor([&] { return !client.action().running && !client.action().text.empty(); }, 10));
    THEN("the folder holds the S11") {
      REQUIRE(std::filesystem::exists(dest + "/s11.s1p"));
    }
  }
  unsetenv("EMFDTD_FARM_COMMAND");
}
