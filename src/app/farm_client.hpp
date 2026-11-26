// The window's side of the GPU farm (docs/farm.md, docs/ux.md "Farm screen"). The window never touches the
// network: it runs the farm tool (`emfdtd_farm.py`, Python with TLS 1.3) and reads its JSON. This file has no
// ImGui and no Vulkan; the Farm tab in apps/emfdtd/ui.cpp draws what `Client::latest()` returns.
#pragma once
#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "board/parts.hpp"
#include "board/voxelize.hpp"

namespace farm {

struct Device {
  std::string worker, name, state; // state: idle, busy, offline
  double mcellsPerS = 0;
};
struct Job {
  std::string id, label, state, worker, phase, reason;
  int attempts = 0;
  int percent = -1;   // -1: the solver has not reported one (an estimate when it has)
  double started = 0; // when the current attempt began running (seconds since 1970); 0 if not running
};
struct Member {
  std::string key, fingerprint, may;
  bool revoked = false;
};
// This computer's own GPU offered to its farm (`local-worker status --json`, docs/ux.md "This computer's
// GPU").
struct LocalGpu {
  bool have = false;      // the status was read
  bool available = false; // this machine can offer its GPU; if not, `reason` says why
  bool enabled = false, running = false, connected = false;
  std::string reason, name, device, solver, turn, error; // turn: lock, coordinator, none
};
// The worker agent on this computer (`agent status|doctor|logs`, docs/ux.md "Agent section").
struct AgentCheck {
  std::string id, title, detail, fix;
  bool ok = false;
};
struct AgentState {
  bool have = false; // `agent status` was read
  bool running = false, connected = false;
  std::string coordinator, device, lastError, error;
  bool hasJob = false;
  std::string jobId, jobLabel, jobPhase;
  int jobPercent = -1;
  bool haveDoctor = false, doctorOk = false;
  std::string doctorError;
  std::vector<AgentCheck> checks;
  std::vector<std::string> log; // the last lines of the agent's log (only read while the Log is open)
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
  LocalGpu local;
  AgentState agent;
};

// Parsers for the tool's `--json` output (pure, tested). Each returns false and sets `error` on bad input.
bool parseStatus(const std::string& json, Snapshot& out, std::string& error);
bool parseQueue(const std::string& json, Snapshot& out, std::string& error);
bool parseMembers(const std::string& json, Snapshot& out, std::string& error);
bool parseLocalStatus(const std::string& json, Snapshot& out, std::string& error);
// What `local-worker enable|disable --json` said, as a sentence for the window; "" if the text is not JSON.
std::string localActionWords(const std::string& json);
// `agent status --json` and `agent doctor --json` (doctor exits 1 when a check fails but still prints the
// report: parse it whatever the exit code). Each keeps the other parts of `out.agent`.
bool parseAgentStatus(const std::string& json, Snapshot& out, std::string& error);
bool parseAgentDoctor(const std::string& json, Snapshot& out, std::string& error);
// The last `n` non-empty lines of `agent logs` output, oldest first.
std::vector<std::string> lastLines(const std::string& text, std::size_t n);
// What `agent start|stop|restart --json` said, as a sentence; the error text for {"error": ...}; "" if the
// text is not JSON.
std::string agentActionWords(const std::string& json);
// "progress: 41200 steps, about 37%, 312 s": -1 if the text is not that line.
int percentFromProgressLine(const std::string& line);
// What a worker's last line means to a reader: the solver's progress line becomes "step 41200, 312 s so far",
// anything else (a phase such as "fetching the input") is shown as it is.
std::string phaseWords(const std::string& line);

// What the window sends when a run goes to the farm: the snapshot it already has, the settings that differ
// from the defaults, and the treatments the user chose. The farm carries exactly these (jobs.py validates
// them); the window never builds a command line.
struct RunRequest {
  std::string snapshot, label, profile, variant;
  std::vector<std::pair<std::string, std::string>> sets;   // settings changed from default: key, text
  std::vector<std::array<double, 5>> refineMm;             // x0, y0, x1, y1, cell
  std::vector<std::pair<std::string, std::string>> parts;  // REF, treatment ("open", "short", "r600,c0.2p")
  std::vector<std::pair<std::string, std::string>> bodies; // REF, treatment ("ignore", "dielectric:3.5")
  std::vector<std::string> nets;
};
// The arguments after the tool command: `submit SNAPSHOT --profile ... --set K=V ... --json`.
std::vector<std::string> submitArgs(const RunRequest& r);
// 1e-7 -> "100n", 600 -> "600", 2e-13 -> "200f": the notation --part values are written in.
std::string siValue(double v);
std::string partTreatmentText(const board::PartTreatment& t);
std::string bodyTreatmentText(const board::BodyTreatment& t);
// Why this selection cannot go to the farm yet ("" if it can). The farm carries net selections, part and body
// treatments and refine boxes; it does not carry a hand-picked component list.
std::string farmBlockedReason(const board::Selection& s);
// The job ids in `submit --json` output.
bool parseSubmitIds(const std::string& json, std::vector<std::string>& ids, std::string& error);
// "running 6 min", "running 45 s", "running 1 h 12 min"; "" when started is 0 or in the future.
std::string runningFor(double startedEpoch, double nowEpoch);
// Folder name for a job fetched into <project>/em-fdtd/results/: "run-farm-" + the first 8 characters of the
// id.
std::string fetchFolderName(const std::string& jobId);
// Is the client role on (read from farm.json, no process run)? And where a run goes, remembered in the farm
// folder (window.json), never in a board's sidecar: "gpu" (default) or "farm".
bool clientEnabled();
std::string loadRunOn();
void saveRunOn(const std::string& value);

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
  // What the Agent section has open: the doctor checks are read while the agent is stopped or "Show checks"
  // is open, the log only while "Log" is open (each is a tool run). Set every frame the section is drawn.
  void watchAgent(bool checksOpen, bool logOpen) {
    agentChecksOpen_ = checksOpen;
    agentLogOpen_ = logOpen;
  }

  // One tool run in the background (enable, disable, cancel, revoke...). The outcome is in action().
  struct Action {
    bool running = false;
    std::string label, text;
    int code = 0;
  };
  void run(std::vector<std::string> args, std::string label, double timeoutSeconds = 60);
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
  std::atomic<bool> agentChecksOpen_{false}, agentLogOpen_{false};
  Snapshot snap_;
  Action action_;
  Invite invite_;
  std::unique_ptr<Child> inviteChild_;
  std::string inviteBuffer_;
  std::vector<std::thread> actions_;
};

} // namespace farm
