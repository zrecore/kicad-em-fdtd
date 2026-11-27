#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
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
// MSVC has no setenv/unsetenv; _putenv_s with an empty value removes the variable.
// A file in the test scratch folder, which the folder's first user must create: a stand-in tool that appends
// to a log in a folder that does not exist yet prints a shell error before its JSON (seen on a fresh CI
// runner).
[[maybe_unused]] std::string tmpPath(const char* name) {
  std::filesystem::create_directories(FDTD_TEST_TMP);
  return std::string(FDTD_TEST_TMP) + "/" + name;
}

[[maybe_unused]] void setEnv(const char* name, const char* value) {
#if defined(_WIN32)
  _putenv_s(name, value);
#else
  setenv(name, value, 1);
#endif
}
[[maybe_unused]] void unsetEnv(const char* name) {
#if defined(_WIN32)
  _putenv_s(name, "");
#else
  unsetenv(name);
#endif
}

[[maybe_unused]] bool waitFor(const std::function<bool()>& ok, double seconds) {
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

// Needs /bin/sh, /bin/sleep or the .sh stub tool; the real tool is tested in plugin/tests.
#if !defined(_WIN32)
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
  THEN("a command is ended within half a second when the window is closing") {
    std::atomic<bool> closing{false};
    std::thread closer([&closing] {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      closing = true;
    });
    const auto t0 = std::chrono::steady_clock::now();
    const auto o = farm::runCapture({"/bin/sleep", "30"}, 60, &closing);
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    closer.join();
    REQUIRE(o.cancelled);
    REQUIRE_FALSE(o.timedOut);
    REQUIRE(took < 0.5 + 0.2);
  }
  THEN("a command is not even started when the window is already closing") {
    const std::atomic<bool> closing{true};
    const auto o = farm::runCapture({"/bin/sleep", "30"}, 60, &closing);
    REQUIRE(o.cancelled);
  }
}
#endif // !_WIN32

// Needs /bin/sh, /bin/sleep or the .sh stub tool; the real tool is tested in plugin/tests.
#if !defined(_WIN32)
SCENARIO("The client polls the tool only while the tab is showing, and runs actions in the background",
         "[farm]") {
  GIVEN("a stand-in for the farm tool") {
    setEnv("EMFDTD_FARM_COMMAND", FDTD_FARM_STUB);
    const std::string log = tmpPath("farm_stub_calls.log");
    std::remove(log.c_str());
    setEnv("EMFDTD_FARM_STUB_LOG", log.c_str());
    farm::Client client;
    WHEN("the tab is shown") {
      client.touch();
      // Generous: the sanitizer build on a loaded CI runner starts the stand-in tool slowly (two CI runs
      // timed out at 30 s). The message says what the client saw if it still fails.
      const bool read = waitFor([&] { return client.latest().haveQueue && client.latest().haveMembers; }, 90);
      const auto seen = client.latest();
      INFO("status: " << seen.haveStatus << " '" << seen.statusError << "', queue: " << seen.haveQueue << " '"
                      << seen.queueError << "', members: " << seen.haveMembers << " '" << seen.membersError
                      << "'");
      REQUIRE(read);
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
    unsetEnv("EMFDTD_FARM_COMMAND");
    unsetEnv("EMFDTD_FARM_STUB_LOG");
  }
}
#endif // !_WIN32

// Needs /bin/sh, /bin/sleep or the .sh stub tool; the real tool is tested in plugin/tests.
#if !defined(_WIN32)
SCENARIO("A client that nobody touches does not call the tool", "[farm]") {
  GIVEN("a client that nobody touches and a stand-in tool that logs every call") {
    setEnv("EMFDTD_FARM_COMMAND", FDTD_FARM_STUB);
    const std::string log = tmpPath("farm_stub_idle.log");
    std::remove(log.c_str());
    setEnv("EMFDTD_FARM_STUB_LOG", log.c_str());
    {
      farm::Client client;
      std::this_thread::sleep_for(std::chrono::milliseconds(2200));
      THEN("the tool was never run") {
        std::ifstream in(log);
        REQUIRE_FALSE(in.good());
      }
    }
    unsetEnv("EMFDTD_FARM_COMMAND");
    unsetEnv("EMFDTD_FARM_STUB_LOG");
  }
}
#endif // !_WIN32

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
  const std::string home = tmpPath("farm_runon_home");
  std::filesystem::remove_all(home);
  setEnv("EMFDTD_FARM_HOME", home.c_str());
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
  unsetEnv("EMFDTD_FARM_HOME");
}

// Needs /bin/sh, /bin/sleep or the .sh stub tool; the real tool is tested in plugin/tests.
#if !defined(_WIN32)
SCENARIO("The window's submit and fetch run the tool and keep its answers", "[farm]") {
  setEnv("EMFDTD_FARM_COMMAND", FDTD_FARM_STUB);
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
    const std::string dest = tmpPath("farm_fetch_dest");
    std::filesystem::remove_all(dest);
    client.run({"results", "9cf37054e5064a39b9298f6ec19fe720", "--dest", dest}, "Fetch results");
    REQUIRE(waitFor([&] { return !client.action().running && !client.action().text.empty(); }, 10));
    THEN("the folder holds the S11") {
      REQUIRE(std::filesystem::exists(dest + "/s11.s1p"));
    }
  }
  unsetEnv("EMFDTD_FARM_COMMAND");
}
#endif // !_WIN32

SCENARIO("The window reads what this computer's own GPU offers the farm", "[farm]") {
  GIVEN("the tool's `local-worker status --json` output") {
    farm::Snapshot s;
    std::string error;
    WHEN("it is available but not turned on") {
      REQUIRE(farm::parseLocalStatus(
          R"({"available":true,"reason":"","enabled":false,"running":false,"connected":false,"name":"","device":"","solver":"","turn":"lock"})",
          s, error));
      THEN("the window can offer the button") {
        REQUIRE(s.local.have);
        REQUIRE(s.local.available);
        REQUIRE_FALSE(s.local.enabled);
        REQUIRE(s.local.turn == "lock");
      }
    }
    WHEN("it is running and connected") {
      REQUIRE(farm::parseLocalStatus(
          R"({"available":true,"reason":"","enabled":true,"running":true,"connected":true,"name":"mac-m1","device":"Apple M1 Pro","solver":"/x/emfdtd","turn":"lock"})",
          s, error));
      THEN("the window shows the device and its state") {
        REQUIRE(s.local.enabled);
        REQUIRE(s.local.running);
        REQUIRE(s.local.connected);
        REQUIRE(s.local.device == "Apple M1 Pro");
      }
    }
    WHEN("it cannot be offered") {
      REQUIRE(farm::parseLocalStatus(R"({"available":false,"reason":"turn on the coordinator role first"})",
                                     s, error));
      THEN("the reason is kept") {
        REQUIRE_FALSE(s.local.available);
        REQUIRE(s.local.reason == "turn on the coordinator role first");
      }
    }
    THEN("bad input is refused with a reason") {
      REQUIRE_FALSE(farm::parseLocalStatus("not json", s, error));
      REQUIRE_FALSE(error.empty());
    }
    THEN("the outcome of enable and disable becomes a sentence") {
      REQUIRE(farm::localActionWords(R"({"ok":true,"name":"mac-m1","device":"Apple M1 Pro"})") ==
              "offering Apple M1 Pro as mac-m1");
      REQUIRE(farm::localActionWords(R"({"ok":true,"was_running":true})") ==
              "stopped; its GPU is no longer offered");
      REQUIRE(farm::localActionWords(R"({"ok":false,"error":"no solver found"})") == "no solver found");
      REQUIRE(farm::localActionWords("plain text").empty());
    }
  }
}

SCENARIO("The window reads the worker agent's state, checks and log", "[farm]") {
  GIVEN("the agent's JSON as the farm tool prints it") {
    farm::Snapshot s;
    std::string error;
    WHEN("the agent is busy with a job") {
      REQUIRE(farm::parseAgentStatus(
          R"({"running":true,"pid":4242,"connected":true,"coordinator":"192.0.2.1:7420","job":{"id":"9cf37054e5064a39","label":null,"phase":"step 41200","percent":37},"device":"Apple M1 Pro","last_error":null})",
          s, error));
      THEN("the window knows the job, its phase and the device") {
        REQUIRE(s.agent.have);
        REQUIRE(s.agent.running);
        REQUIRE(s.agent.connected);
        REQUIRE(s.agent.hasJob);
        REQUIRE(s.agent.jobId == "9cf37054e5064a39");
        REQUIRE(s.agent.jobLabel.empty());
        REQUIRE(s.agent.jobPhase == "step 41200");
        REQUIRE(s.agent.jobPercent == 37);
        REQUIRE(s.agent.device == "Apple M1 Pro");
        REQUIRE(s.agent.lastError.empty());
      }
    }
    WHEN("the agent is stopped") {
      REQUIRE(farm::parseAgentStatus(
          R"({"running":false,"pid":null,"connected":false,"coordinator":null,"job":null,"device":null,"last_error":"boom"})",
          s, error));
      THEN("there is no job and the last error is kept") {
        REQUIRE_FALSE(s.agent.running);
        REQUIRE_FALSE(s.agent.hasJob);
        REQUIRE(s.agent.lastError == "boom");
      }
    }
    WHEN("the doctor report has a failing check") {
      REQUIRE(farm::parseAgentStatus(R"({"running":false})", s, error));
      REQUIRE(farm::parseAgentDoctor(
          R"({"ok":false,"checks":[{"id":"tls13","title":"Python has TLS 1.3","ok":true,"detail":"OpenSSL 3.5","fix":""},{"id":"solver","title":"The solver runs","ok":false,"detail":"missing","fix":"set --solver"}]})",
          s, error));
      THEN("the checks, the verdict and the fix are kept, and the status is not lost") {
        REQUIRE(s.agent.have);
        REQUIRE(s.agent.haveDoctor);
        REQUIRE_FALSE(s.agent.doctorOk);
        REQUIRE(s.agent.checks.size() == 2);
        REQUIRE(s.agent.checks[1].title == "The solver runs");
        REQUIRE_FALSE(s.agent.checks[1].ok);
        REQUIRE(s.agent.checks[1].fix == "set --solver");
      }
    }
    THEN("an error report or bad text is refused with a reason") {
      REQUIRE_FALSE(farm::parseAgentDoctor(R"({"error":"no saved configuration"})", s, error));
      REQUIRE(error == "no saved configuration");
      REQUIRE_FALSE(farm::parseAgentStatus("not json", s, error));
      REQUIRE_FALSE(error.empty());
    }
    THEN("the log keeps the last lines, oldest first, without blanks") {
      const auto lines = farm::lastLines("a\n\nb  \r\nc\nd\n", 3);
      REQUIRE(lines == std::vector<std::string>{"b", "c", "d"});
      REQUIRE(farm::lastLines("", 20).empty());
    }
    THEN("the outcome of start, stop and restart becomes a sentence") {
      REQUIRE(farm::agentActionWords(R"({"running":true,"pid":4242})") ==
              "the agent is running (process 4242)");
      REQUIRE(farm::agentActionWords(R"({"was_running":true,"stopped":true,"forced":false})") ==
              "the agent stopped");
      REQUIRE(farm::agentActionWords(R"({"was_running":true,"stopped":true,"forced":true})") ==
              "the agent stopped (it had to be ended by force)");
      REQUIRE(farm::agentActionWords(R"({"was_running":false,"stopped":true,"forced":false})") ==
              "the agent was not running");
      REQUIRE(farm::agentActionWords(R"({"error":"cannot start: the solver"})") ==
              "cannot start: the solver");
      REQUIRE(farm::agentActionWords("plain text").empty());
    }
  }
}
