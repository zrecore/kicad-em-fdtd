#include "app/farm_client.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

#include "core/resources.hpp"

#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace farm {
namespace {

double nowSeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string str(const nlohmann::json& j, const char* key) {
  const auto it = j.find(key);
  return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

double num(const nlohmann::json& j, const char* key, double fallback = 0) {
  const auto it = j.find(key);
  return it != j.end() && it->is_number() ? it->get<double>() : fallback;
}

std::vector<std::string> split(const std::string& text) {
  std::vector<std::string> out;
  std::istringstream in(text);
  for (std::string w; in >> w;)
    out.push_back(w);
  return out;
}

} // namespace

// ---- parsing
// -----------------------------------------------------------------------------------------------

bool parseStatus(const std::string& json, Snapshot& out, std::string& error) {
  try {
    const auto j = nlohmann::json::parse(json);
    out.home = str(j, "home");
    const auto& e = j.at("enabled");
    out.coordinator = e.value("coordinator", false);
    out.worker = e.value("worker", false);
    out.client = e.value("client", false);
    out.haveStatus = true;
    out.statusError.clear();
    return true;
  } catch (const std::exception& ex) {
    error = std::string("the farm tool's status was not understood: ") + ex.what();
    return false;
  }
}

bool parseQueue(const std::string& json, Snapshot& out, std::string& error) {
  try {
    const auto j = nlohmann::json::parse(json);
    std::vector<Device> devices;
    for (const auto& d : j.at("devices"))
      devices.push_back({str(d, "worker"), str(d, "name"), str(d, "state"), num(d, "mcells_per_s")});
    std::vector<Job> jobs;
    for (const auto& r : j.at("jobs")) {
      Job job;
      job.id = str(r, "job");
      job.label = str(r, "label");
      job.state = str(r, "state");
      job.worker = str(r, "worker");
      job.phase = phaseWords(str(r, "progress"));
      job.reason = str(r, "reason");
      job.attempts = static_cast<int>(num(r, "attempts"));
      job.percent =
          r.contains("percent") && r["percent"].is_number() ? static_cast<int>(num(r, "percent")) : -1;
      jobs.push_back(std::move(job));
    }
    out.devices = std::move(devices);
    out.jobs = std::move(jobs);
    out.haveQueue = true;
    out.queueError.clear();
    return true;
  } catch (const std::exception& ex) {
    error = std::string("the queue was not understood: ") + ex.what();
    return false;
  }
}

bool parseMembers(const std::string& json, Snapshot& out, std::string& error) {
  try {
    const auto j = nlohmann::json::parse(json);
    std::vector<Member> members;
    for (const auto& m : j.at("members")) {
      Member member;
      member.key = str(m, "key");
      member.fingerprint = str(m, "fingerprint");
      member.revoked = m.value("revoked", false);
      std::string may;
      if (m.contains("may") && m["may"].is_array())
        for (const auto& a : m["may"])
          may += (may.empty() ? "" : ", ") + a.get<std::string>();
      member.may = may;
      members.push_back(std::move(member));
    }
    out.members = std::move(members);
    out.haveMembers = true;
    out.membersError.clear();
    return true;
  } catch (const std::exception& ex) {
    error = std::string("the member list was not understood: ") + ex.what();
    return false;
  }
}

int percentFromProgressLine(const std::string& line) {
  static const std::regex re(R"(^progress: \d+ steps, about (\d{1,3})%)");
  std::smatch m;
  if (!std::regex_search(line, m, re))
    return -1;
  return std::min(100, std::stoi(m[1].str()));
}

std::string phaseWords(const std::string& line) {
  static const std::regex re(R"(^progress: (\d+) steps, about \d{1,3}%, (\d+) s)");
  std::smatch m;
  if (std::regex_search(line, m, re))
    return "step " + m[1].str() + ", " + m[2].str() + " s so far";
  return line;
}

// ---- where things are
// --------------------------------------------------------------------------------------

std::string farmHome() {
  if (const char* h = std::getenv("EMFDTD_FARM_HOME"); h && *h)
    return h;
#if defined(_WIN32)
  const char* appdata = std::getenv("APPDATA");
  return (fs::path(appdata ? appdata : ".") / "emfdtd" / "farm").string();
#elif defined(__APPLE__)
  const char* home = std::getenv("HOME");
  return (fs::path(home ? home : ".") / "Library" / "Application Support" / "emfdtd" / "farm").string();
#else
  if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x)
    return (fs::path(x) / "emfdtd" / "farm").string();
  const char* home = std::getenv("HOME");
  return (fs::path(home ? home : ".") / ".config" / "emfdtd" / "farm").string();
#endif
}

std::vector<std::string> toolCommand() {
  if (const char* c = std::getenv("EMFDTD_FARM_COMMAND"); c && *c)
    return split(c);
  const fs::path plugin = core::resourceDir("plugin", FDTD_PLUGIN_DIR);
  std::string python;
  if (const char* p = std::getenv("EMFDTD_PYTHON"); p && *p) {
    python = p;
  } else {
    try {
      std::ifstream in(fs::path(farmHome()) / "farm.json");
      if (in) {
        const auto j = nlohmann::json::parse(in);
        python = str(j, "python");
      }
    } catch (...) {
    }
    std::error_code ec;
    if (python.empty() || !fs::exists(python, ec)) {
      const fs::path venv = plugin / ".venv" / "bin" / "python";
      python = fs::exists(venv, ec) ? venv.string() : "python3";
    }
  }
  return {python, (plugin / "tools" / "emfdtd_farm.py").string()};
}

// ---- child processes (POSIX; the Windows build is not wired up yet) ------------------------------------

#if !defined(_WIN32)
class Child {
public:
  static std::unique_ptr<Child> spawn(const std::vector<std::string>& argv, std::string& error) {
    int fds[2];
    if (pipe(fds) != 0) {
      error = std::strerror(errno);
      return nullptr;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    std::vector<char*> args;
    for (const auto& a : argv)
      args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, args[0], &fa, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (rc != 0) {
      close(fds[0]);
      error = std::string("cannot start ") + argv[0] + ": " + std::strerror(rc);
      return nullptr;
    }
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    auto c = std::unique_ptr<Child>(new Child);
    c->pid_ = pid;
    c->fd_ = fds[0];
    return c;
  }
  ~Child() {
    terminate();
    if (fd_ >= 0)
      close(fd_);
  }
  // Appends what is available within timeoutMs. Returns false at end of output.
  bool read(std::string& out, int timeoutMs) {
    if (fd_ < 0)
      return false;
    pollfd p{fd_, POLLIN, 0};
    if (poll(&p, 1, timeoutMs) <= 0)
      return true;
    char buf[4096];
    const ssize_t n = ::read(fd_, buf, sizeof buf);
    if (n > 0) {
      out.append(buf, static_cast<std::size_t>(n));
      return true;
    }
    if (n < 0 && (errno == EAGAIN || errno == EINTR))
      return true;
    return false;
  }
  void terminate() {
    if (pid_ > 0 && !reaped_) {
      kill(pid_, SIGTERM);
      for (int i = 0; i < 50 && !reaped_; ++i) {
        if (waitpid(pid_, &status_, WNOHANG) == pid_)
          reaped_ = true;
        else
          usleep(20000);
      }
      if (!reaped_) {
        kill(pid_, SIGKILL);
        waitpid(pid_, &status_, 0);
        reaped_ = true;
      }
    }
  }
  int wait() {
    if (!reaped_) {
      waitpid(pid_, &status_, 0);
      reaped_ = true;
    }
    return WIFEXITED(status_) ? WEXITSTATUS(status_) : -1;
  }

private:
  Child() = default;
  pid_t pid_ = 0;
  int fd_ = -1;
  int status_ = 0;
  bool reaped_ = false;
};

Output runCapture(const std::vector<std::string>& argv, double timeoutSeconds) {
  Output out;
  std::string error;
  auto child = Child::spawn(argv, error);
  if (!child) {
    out.text = error;
    return out;
  }
  const double deadline = nowSeconds() + timeoutSeconds;
  while (child->read(out.text, 100)) {
    if (nowSeconds() > deadline) {
      out.timedOut = true;
      child->terminate();
      out.code = -1;
      return out;
    }
  }
  out.code = child->wait();
  return out;
}
#else
class Child {};
Output runCapture(const std::vector<std::string>&, double) {
  Output out;
  out.text = "the farm panel is not wired up on Windows yet: use the emfdtd_farm.py command line";
  return out;
}
#endif

// ---- the client
// --------------------------------------------------------------------------------------------

Client::Client() : thread_([this] { loop(); }) {}

Client::~Client() {
  quit_ = true;
  cv_.notify_all();
  if (thread_.joinable())
    thread_.join();
  for (auto& t : actions_)
    if (t.joinable())
      t.join();
  stopInvite();
}

void Client::touch() {
  touched_ = nowSeconds();
  cv_.notify_all();
}

Snapshot Client::latest() const {
  std::lock_guard<std::mutex> lock(mu_);
  return snap_;
}

Client::Action Client::action() const {
  std::lock_guard<std::mutex> lock(mu_);
  return action_;
}

Client::Invite Client::invite() const {
  std::lock_guard<std::mutex> lock(mu_);
  return invite_;
}

void Client::loop() {
  while (!quit_) {
    if (nowSeconds() - touched_ < 4.0)
      pollOnce();
    pumpInvite();
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait_for(lock, std::chrono::milliseconds(1500));
  }
}

void Client::pollOnce() {
  auto command = toolCommand();
  auto call = [&](std::vector<std::string> args, double timeout) {
    auto argv = command;
    argv.insert(argv.end(), args.begin(), args.end());
    return runCapture(argv, timeout);
  };
  Snapshot next = latest();
  const Output st = call({"status", "--json"}, 10);
  std::string error;
  if (st.code == 0 && parseStatus(st.text, next, error)) {
    // fine
  } else {
    next.haveStatus = false;
    next.statusError = st.timedOut ? "the farm tool did not answer" : (error.empty() ? st.text : error);
  }
  if (next.haveStatus && next.client) {
    const Output q = call({"queue", "--json"}, 15);
    std::string qerr;
    if (q.code == 0 && parseQueue(q.text, next, qerr)) {
    } else {
      next.haveQueue = false;
      next.queueError = q.timedOut ? "the coordinator did not answer" : (qerr.empty() ? q.text : qerr);
    }
  } else {
    next.haveQueue = false;
    next.devices.clear();
    next.jobs.clear();
  }
  if (next.haveStatus && next.coordinator) {
    const Output m = call({"members", "--json"}, 10);
    std::string merr;
    if (m.code == 0 && parseMembers(m.text, next, merr)) {
    } else {
      next.haveMembers = false;
      next.membersError = m.timedOut ? "the farm tool did not answer" : (merr.empty() ? m.text : merr);
    }
  } else {
    next.haveMembers = false;
    next.members.clear();
  }
  std::lock_guard<std::mutex> lock(mu_);
  snap_ = std::move(next);
}

void Client::run(std::vector<std::string> args, std::string label) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (action_.running)
      return;
    action_ = {true, label, {}, 0};
  }
  actions_.emplace_back([this, args = std::move(args), label = std::move(label)] {
    auto argv = toolCommand();
    argv.insert(argv.end(), args.begin(), args.end());
    const Output o = runCapture(argv, 60);
    {
      std::lock_guard<std::mutex> lock(mu_);
      action_.running = false;
      action_.code = o.timedOut ? -1 : o.code;
      action_.text = o.timedOut ? "the farm tool did not finish in time" : o.text;
    }
    touch();
  });
}

void Client::startInvite(const std::string& name, const std::string& role, const std::string& listen) {
  stopInvite();
  auto argv = toolCommand();
  for (const char* a : {"invite"})
    argv.push_back(a);
  argv.push_back(name);
  argv.push_back("--role");
  argv.push_back(role);
  argv.push_back("--listen");
  argv.push_back(listen);
  std::string error;
  auto child = Child::spawn(argv, error);
  std::lock_guard<std::mutex> lock(mu_);
  invite_ = {};
  invite_.name = name;
  invite_.role = role;
  invite_.startedAt = nowSeconds();
  if (!child) {
    invite_.last = error;
    invite_.exitCode = -1;
    return;
  }
  inviteChild_ = std::move(child);
  inviteBuffer_.clear();
  invite_.running = true;
}

void Client::stopInvite() {
  std::unique_ptr<Child> child;
  {
    std::lock_guard<std::mutex> lock(mu_);
    child = std::move(inviteChild_);
    invite_.running = false;
  }
  child.reset(); // terminates it
}

void Client::pumpInvite() {
  std::unique_lock<std::mutex> lock(mu_);
  if (!inviteChild_)
    return;
  std::string chunk;
  Child* c = inviteChild_.get();
  lock.unlock();
  const bool alive = c->read(chunk, 20);
  lock.lock();
  if (!inviteChild_)
    return;
  inviteBuffer_ += chunk;
  std::size_t nl;
  while ((nl = inviteBuffer_.find('\n')) != std::string::npos) {
    const std::string line = inviteBuffer_.substr(0, nl);
    inviteBuffer_.erase(0, nl + 1);
    auto after = [&](const char* tag) -> std::string {
      const auto at = line.find(tag);
      if (at == std::string::npos)
        return {};
      std::string v = line.substr(at + std::strlen(tag));
      v.erase(0, v.find_first_not_of(' '));
      return v;
    };
    if (auto v = after("code:"); !v.empty())
      invite_.code = v;
    else if (auto f = after("fingerprint:"); !f.empty())
      invite_.fingerprint = f;
    else if (auto l = after("listening on"); !l.empty())
      invite_.listening = l;
    invite_.last = line;
  }
  if (!alive) {
    invite_.running = false;
    invite_.exitCode = inviteChild_->wait();
    inviteChild_.reset();
  }
}

} // namespace farm
