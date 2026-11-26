// The window's side of the GPU farm (docs/farm.md, docs/ux.md "Farm screen"). The window never touches the
// network: it runs the farm tool (`emfdtd_farm.py`, Python with TLS 1.3) and reads its JSON. This file has no
// ImGui and no Vulkan; the Farm tab in apps/emfdtd/ui.cpp draws what `Client::latest()` returns.
#pragma once
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace farm {

struct Device {
  std::string worker, name, state; // state: idle, busy, offline
  double mcellsPerS = 0;
};
struct Job {
  std::string id, label, state, worker, phase, reason;
  int attempts = 0;
  int percent = -1; // -1: the solver has not reported one (an estimate when it has)
};
struct Member {
  std::string key, fingerprint, may;
  bool revoked = false;
};
struct Snapshot {
  bool haveStatus = false;
  std::string home, statusError;
  bool coordinator = false, worker = false, client = false;
  bool haveQueue = false, haveMembers = false;
  std::string queueError, membersError;
  std::vector<Device> devices;
  std::vector<Job> jobs;
  std::vector<Member> members;
};

// Parsers for the tool's `--json` output (pure, tested). Each returns false and sets `error` on bad input.
bool parseStatus(const std::string& json, Snapshot& out, std::string& error);
bool parseQueue(const std::string& json, Snapshot& out, std::string& error);
bool parseMembers(const std::string& json, Snapshot& out, std::string& error);
// "progress: 41200 steps, about 37%, 312 s": -1 if the text is not that line.
int percentFromProgressLine(const std::string& line);
// What a worker's last line means to a reader: the solver's progress line becomes "step 41200, 312 s so far",
// anything else (a phase such as "fetching the input") is shown as it is.
std::string phaseWords(const std::string& line);

struct Output {
  int code = -1;
  std::string text;
  bool timedOut = false;
};
// Runs argv (no shell), captures stdout and stderr, kills it after timeoutSeconds.
Output runCapture(const std::vector<std::string>& argv, double timeoutSeconds);

// The command that runs the farm tool: env EMFDTD_FARM_COMMAND (split on spaces; for tests and unusual
// installs), else the Python recorded when the farm was enabled (farm.json), else the plugin's venv, else
// python3, followed by <plugin>/tools/emfdtd_farm.py.
std::vector<std::string> toolCommand();
// The farm's state folder, as the tool computes it (EMFDTD_FARM_HOME overrides).
std::string farmHome();

class Child; // a running child process with a pipe to its output (POSIX)

class Client {
public:
  Client();
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // Call while the Farm tab is on screen: the poller works only for a few seconds after each call, so a
  // window that is not showing the tab does nothing.
  void touch();
  Snapshot latest() const;

  // One tool run in the background (enable, disable, cancel, revoke...). The outcome is in action().
  struct Action {
    bool running = false;
    std::string label, text;
    int code = 0;
  };
  void run(std::vector<std::string> args, std::string label);
  Action action() const;

  // An invitation for a new device: `invite` runs until the device joins or the time is up.
  struct Invite {
    bool running = false;
    std::string name, role, code, fingerprint, listening, last;
    double startedAt = 0, minutes = 10;
    int exitCode = 0;
  };
  void startInvite(const std::string& name, const std::string& role, const std::string& listen);
  void stopInvite();
  Invite invite() const;

private:
  void loop();
  void pollOnce();
  void pumpInvite();

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::thread thread_;
  std::atomic<bool> quit_{false};
  std::atomic<double> touched_{0};
  Snapshot snap_;
  Action action_;
  Invite invite_;
  std::unique_ptr<Child> inviteChild_;
  std::string inviteBuffer_;
  std::vector<std::thread> actions_;
};

} // namespace farm
