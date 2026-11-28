// emfdtd: board snapshot -> S11 (Touchstone) and an animated field view. Everything numeric comes from the
// radio profiles (data/radio_profiles.json) and the settings; nothing about bands or cells is fixed here.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <process.h>
#define getpid _getpid
#define dup _dup
#define dup2 _dup2
#define fdopen _fdopen
#else
#include <unistd.h>
#endif

#include "app/checkpoint.hpp"
#include "app/cli_json.hpp"
#include "app/field_recorder.hpp"
#include "app/paths.hpp"
#include "app/phasor_feed.hpp"
#include "app/prepare.hpp"
#include "app/settings.hpp"
#include "app/sweep.hpp"
#include "board/geometry_override.hpp"
#include "board/netwalk.hpp"
#include "board/parts.hpp"
#include "board/refine_suggest.hpp"
#include "board/snapshot.hpp"
#include "board/voxelize.hpp"
#include "core/resources.hpp"
#include "fdtd_vk/gpu_engine.hpp"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "sim/simulation.hpp"
#include "ui.hpp"
#include "version.hpp"
#include "viz/gpu_ui.hpp"
#include "viz/png.hpp"
#include "viz/screen_reader.hpp"
#include "viz/slice.hpp"
#include "viz/window.hpp"

#include <GLFW/glfw3.h>
#include <csignal>

namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& path) {
  std::ifstream f(path);
  if (!f)
    throw std::runtime_error("cannot open " + path.string());
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Set by SIGTERM when --checkpoint-dir is given: the run writes a checkpoint and ends (exit status 75).
std::atomic<bool> gPauseSignal{false};
extern "C" void onPauseSignal(int) {
  gPauseSignal.store(true);
}

void usage() {
  std::fprintf(
      stderr,
      "usage: emfdtd SNAPSHOT.json [options]\n"
      "  --open-run DIR           show a saved run (a folder under em-fdtd/results) without solving (--ui or "
      "--ui-screenshot)\n"
      "  --prune-runs N           delete the field data of all but the newest N saved runs, keep their S11, "
      "and exit\n"
      "  --project-dir DIR        the KiCAD project folder; results go to DIR/em-fdtd/results/run-<time>/\n"
      "                           (default: the folder holding SNAPSHOT.json)\n"
      "  --profile ID[:VARIANT]   radio profile: wifi, bluetooth, zigbee, lwmesh, uwb, custom (default "
      "wifi)\n"
      "  --set KEY=VALUE          change any setting (repeatable); --list-settings shows them\n"
      "  --version                print the version (semver) and exit\n"
      "  --list-settings          print every setting with its default, tier and help, then exit\n"
      "  --include REF[,REF...]   model only these footprints (default: the whole board)\n"
      "  --body-label REF=TEXT    note printed with that body (for example how well it matches a datasheet)\n"
      "  --body REF=ignore|metal|dielectric:EPS[:TAND]  a part's body treatment (repeatable)\n"
      "  --refine X0 Y0 X1 Y1 MM  extra fine-cell box in board coordinates (mm); repeatable\n"
      "  --refine-gaps CELLS      refine every narrow gap between two nets near a port to CELLS cells across "
      "it\n"
      "                           (a dry run lists the suggestions that are not applied)\n"
      "  --device SEL             GPU to use: index or UUID prefix from --list-devices (also env "
      "EMFDTD_DEVICE)\n"
      "  --list-devices           (first argument) list the Vulkan devices with UUID and driver, then exit\n"
      "  --cpu | --gpu            force a backend (default: setting 'backend', auto = GPU if present)\n"
      "  --view                   open a window and animate the field while the solve runs\n"
      "  --frames N               write N animation frames into the run folder\n"
      "  --dry-run                prepare and print the plan and estimate, do not solve\n"
      "  --keep-fields            keep the field data of the run (3D and slice views, surface current, "
      "power\n"
      "                           flow, far field) so `--open-run` can show it; costs time and disk (see\n"
      "                           the estimate's field_bytes in --dry-run --json); off unless asked\n"
      "  --checkpoint-dir DIR     write checkpoints of the run into DIR (docs/checkpoint.md); a file "
      "DIR/stop or\n"
      "                           SIGTERM writes one and ends the run with exit status 75 (not with --ui, "
      "--view,\n"
      "                           --frames or --sweep)\n"
      "  --checkpoint-every SEC   seconds between checkpoints (default 300)\n"
      "  --resume                 continue the run saved in --checkpoint-dir (same machine, device, driver, "
      "binary\n"
      "                           and inputs, or it is refused)\n"
      "  --ui-frames N            with --ui: report the frame cost over N window frames, then exit\n"
      "  --cpu-ui                 with --ui: draw the UI on the CPU instead of the GPU\n"
      "  --ui                     open the solver window on its first screen (preview and settings)\n"
      "  --autorun                with --ui: start the run as soon as the model is ready\n"
      "  --ui-a11y-dump FILE      with --ui-screenshot: write what a screen reader is told; exit 1 if a "
      "control is unnamed\n"
      "  --ui-screenshot FILE.png [--ui-size WxH] [--ui-query TEXT] [--ui-expand GROUP] [--ui-layers "
      "field,nofield,current]  render that screen "
      "to an image\n"
      "Settings saved for this board in DIR/em-fdtd/settings.json are loaded first; command-line values "
      "win.\n");
}

const char* tierName(app::Tier t) {
  switch (t) {
  case app::Tier::Essential:
    return "essential";
  case app::Tier::Common:
    return "common";
  case app::Tier::Advanced:
    return "advanced";
  default:
    return "expert";
  }
}

void listSettings(const app::Settings& s) {
  for (const auto& d : s.defs()) {
    std::string value;
    if (d.kind == app::Kind::Number) {
      char buf[48];
      std::snprintf(buf, sizeof buf, "%g", s.number(d.key));
      value = buf;
    } else {
      value = s.text(d.key);
    }
    std::printf("%-30s %-10s %s%s%s  [%s] %s\n", d.key.c_str(), tierName(d.tier), value.c_str(),
                d.unit.empty() ? "" : " ", d.unit.c_str(), d.group.c_str(), d.help.c_str());
  }
}

// Animates the accumulated phasors in a window until it is closed. Keys: space pause, left/right frequency,
// up/down colour scale, escape quit. Reads `phasors` under the mutex the solver thread also takes.
void runViewer(const fdtd::Grid& g, const viz::Plane& plane, const viz::PhasorAccumulator& phasors,
               std::mutex& phasorMutex, const std::vector<std::uint8_t>& mask, const std::atomic<bool>& done,
               std::atomic<bool>& stop, const sim::RunResult& result) {
  viz::Window window("kicad-em-fdtd", 900, 700);
  std::size_t k = phasors.frequencies() / 2;
  double phase = 0, scaleFactor = 1;
  bool paused = false, jumped = false;
  auto last = std::chrono::steady_clock::now();
  while (!window.shouldClose() && !stop) {
    window.pollEvents();
    for (int key : window.takeKeys()) {
      if (key == GLFW_KEY_SPACE)
        paused = !paused;
      else if (key == GLFW_KEY_LEFT && k > 0)
        --k;
      else if (key == GLFW_KEY_RIGHT && k + 1 < phasors.frequencies())
        ++k;
      else if (key == GLFW_KEY_UP)
        scaleFactor /= 1.25;
      else if (key == GLFW_KEY_DOWN)
        scaleFactor *= 1.25;
      else if (key == GLFW_KEY_ESCAPE)
        stop = true;
    }
    if (done && !jumped) { // the solve finished: jump to the S11 minimum, where the antenna radiates best
      jumped = true;
      std::size_t best = 0;
      for (std::size_t i = 1; i < result.s.s11.size(); ++i)
        if (std::abs(result.s.s11[i]) < std::abs(result.s.s11[best]))
          best = i;
      for (std::size_t i = 0; i < phasors.frequencies(); ++i)
        if (std::abs(phasors.frequencyHz(i) - result.s.frequenciesHz[best]) <
            std::abs(phasors.frequencyHz(k) - result.s.frequenciesHz[best]))
          k = i;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!paused)
      phase += 2 * 3.14159265358979 * 0.5 * std::chrono::duration<double>(now - last).count();
    last = now;

    const VkExtent2D ex = window.extent();
    if (ex.width == 0 || ex.height == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
    viz::FrameSpec spec;
    spec.canvasWidth = ex.width;
    spec.canvasHeight = ex.height;
    std::uint32_t w = 0, h = 0;
    std::vector<std::uint8_t> rgb;
    {
      std::lock_guard<std::mutex> lock(phasorMutex);
      double peak = 0;
      for (std::size_t v = 0; v < phasors.voxels(); ++v)
        peak = std::max(peak, static_cast<double>(std::abs(phasors.phasor(k, v))));
      spec.colourScale = std::max(peak * scaleFactor, 1e-30);
      rgb = viz::renderFrame(g, plane, phasors, k, phase, spec, mask, w, h);
    }
    window.present(rgb, w, h);
    char title[160];
    std::snprintf(title, sizeof title, "kicad-em-fdtd  Ez  %.3f GHz  %s%s", phasors.frequencyHz(k) * 1e-9,
                  done ? "solve done" : "solving...", paused ? "  [paused]" : "");
    window.setTitle(title);
  }
}

} // namespace

void listDevices() {
  const auto devices = vkcore::listDevices();
  std::size_t usableIndex = 0;
  for (const auto& d : devices) {
    const bool ok = vkcore::usable(d);
    std::printf("%s  %s  uuid %s  %s  %.1f GiB  vulkan %u.%u  driver %s%s\n",
                ok ? std::to_string(usableIndex++).c_str() : "-", d.name.c_str(),
                d.uuid.empty() ? "unknown" : d.uuid.c_str(), d.discrete ? "discrete" : "integrated/other",
                static_cast<double>(d.deviceLocalBytes) / (1024.0 * 1024.0 * 1024.0),
                VK_API_VERSION_MAJOR(d.apiVersion), VK_API_VERSION_MINOR(d.apiVersion),
                d.driver.empty() ? "unknown" : d.driver.c_str(),
                ok ? "" : "  (below Vulkan 1.2: not usable)");
  }
  if (devices.empty())
    std::printf("no Vulkan devices found\n");
}

// --json: stdout carries exactly one JSON document. The human text of a run (printf all over this file) is
// moved to stderr by pointing file descriptor 1 at stderr; the document goes to a copy of the real stdout.
FILE* jsonOut = nullptr;

void startJson() {
  std::fflush(stdout);
  jsonOut = fdopen(dup(1), "w");
  dup2(2, 1);
}

void emitJson(const std::string& document) {
  std::fputs(document.c_str(), jsonOut ? jsonOut : stdout);
  std::fputc('\n', jsonOut ? jsonOut : stdout);
  std::fflush(jsonOut ? jsonOut : stdout);
}

app::clijson::DeviceRow deviceRow(const vkcore::DeviceInfo& d, std::optional<int> index) {
  app::clijson::DeviceRow r;
  r.index = index;
  r.name = d.name;
  r.uuid = d.uuid;
  r.driver = d.driver;
  r.memoryBytes = d.deviceLocalBytes;
  r.vulkanMajor = VK_API_VERSION_MAJOR(d.apiVersion);
  r.vulkanMinor = VK_API_VERSION_MINOR(d.apiVersion);
  r.discrete = d.discrete;
  r.usable = vkcore::usable(d);
  return r;
}

std::string devicesJson() {
  std::vector<app::clijson::DeviceRow> rows;
  int usableIndex = 0;
  for (const auto& d : vkcore::listDevices())
    rows.push_back(deviceRow(d, vkcore::usable(d) ? std::optional<int>(usableIndex++) : std::nullopt));
  return app::clijson::devicesList(FDTD_VERSION, rows);
}

int main(int argc, char** argv) {
  // Progress lines should appear even if the run is killed. The MSVC runtime rejects a zero buffer size with
  // _IOLBF (it aborts) and treats _IOLBF as full buffering, so there stdout is unbuffered.
#ifdef _WIN32
  std::setvbuf(stdout, nullptr, _IONBF, 0);
#else
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
#endif
  bool jsonMode = false, wantS11 = false;
  for (int i = 1; i < argc; ++i) {
    jsonMode = jsonMode || std::string(argv[i]) == "--json";
    wantS11 = wantS11 || std::string(argv[i]) == "--s11";
  }
  if (jsonMode)
    startJson();
  // Every failure in --json mode is a document on stdout too, so a caller never has to read stderr.
  auto fail = [&](int code, const std::string& message) {
    if (jsonMode)
      emitJson(app::clijson::error(FDTD_VERSION, message));
    return code;
  };
  try {
    if (argc < 2) {
      usage();
      return fail(2, "no arguments; usage is on stderr");
    }
    if (std::string(argv[1]) == "--run-info") { // a saved run folder as JSON: no solve, no GPU, no window
      if (argc < 3) {
        usage();
        return fail(2, "--run-info needs a run folder");
      }
      try {
        emitJson(app::clijson::runInfo(FDTD_VERSION, argv[2], wantS11));
        return 0;
      } catch (const std::exception& e) {
        emitJson(app::clijson::error(FDTD_VERSION, e.what()));
        return 1;
      }
    }
    if (std::string(argv[1]) == "--version") {
      std::printf("emfdtd %s\n", FDTD_VERSION);
      return 0;
    }
    if (std::string(argv[1]) == "--list-devices") {
      if (jsonMode)
        emitJson(devicesJson());
      else
        listDevices();
      return 0;
    }
    const app::Catalog catalog =
        app::Catalog::load(core::resourceDir("data", FDTD_DATA_DIR) + "/radio_profiles.json");
    app::Settings settings(catalog);
    if (std::string(argv[1]) == "--list-settings") {
      if (jsonMode)
        emitJson(app::clijson::settingsList(FDTD_VERSION, settings));
      else
        listSettings(settings);
      return 0;
    }
    const fs::path snapshotPath = argv[1];
    fs::path projectDir = fs::absolute(snapshotPath).parent_path();
    std::string profileArg;
    std::vector<std::pair<std::string, std::string>> sets;
    std::vector<board::RefineBox> refine;
    double refineGaps = 0; // cells across each narrow gap near a port; 0: only list the suggestions
    board::Selection selection;
    std::set<std::string> pickedNets, extraGround;
    app::SweepSpec sweepSpec;
    std::string sweepValuesText, probesText;
    bool exportAfterRun = false, clickWanted = false;
    float clickX = 0, clickY = 0;
    std::vector<board::GeometryOverride> geomOverrides;
    int frameCount = 0;
    std::string checkpointDir;
    double checkpointEvery = 300;
    bool resumeRun = false, keepFields = false;
    bool view = false, dryRun = false, uiMode = false, autorun = false;
    std::string shotFile, shotQuery, shotExpand, shotTab;
    bool shotRun = false, showSettings = false, hideControls = false;
    int benchFrames = 0;
    int windowFrames = 0; // --ui-frames: measure this many window frames, then exit
    bool softUi = false;  // --cpu-ui: draw the UI with the CPU rasteriser
    std::string shotTheme, a11yDump;
    std::string layersFlag;
    int legendsFlag = -1; // -1 default, 0 collapsed, 1 expanded
    std::uint32_t shotW = 1280, shotH = 800;
    std::string backendFlag, openRunDir, farmResultDir;
    // Which GPU (index or UUID prefix from --list-devices). Machine-specific, so a flag and an environment
    // variable, not a setting. A named device that cannot be used is an error, never a silent CPU fallback.
    std::string deviceSel = std::getenv("EMFDTD_DEVICE") ? std::getenv("EMFDTD_DEVICE") : "";
    int pruneKeep = -1;
    for (int i = 2; i < argc; ++i) {
      const std::string a = argv[i];
      auto next = [&]() -> const char* {
        if (i + 1 >= argc)
          throw std::runtime_error("missing value after " + a);
        return argv[++i];
      };
      if (a == "--project-dir")
        projectDir = next();
      else if (a == "--profile")
        profileArg = next();
      else if (a == "--set") {
        const std::string kv = next();
        const auto eq = kv.find('=');
        if (eq == std::string::npos)
          throw std::runtime_error("--set needs KEY=VALUE, got '" + kv + "'");
        sets.push_back({kv.substr(0, eq), kv.substr(eq + 1)});
      } else if (a == "--include") {
        selection.all = false;
        std::stringstream ss(next());
        for (std::string ref; std::getline(ss, ref, ',');)
          selection.owners.insert(ref);
      } else if (a ==
                 "--sweep") { // KEY=a,b,c or KEY=start:stop:count: one run per value, S11 curves overlaid
        const std::string kv = next();
        const auto eq = kv.find('=');
        if (eq == std::string::npos)
          throw std::runtime_error("--sweep needs KEY=a,b,c or KEY=start:stop:count, got '" + kv + "'");
        sweepSpec.key = kv.substr(0, eq);
        sweepValuesText = kv.substr(eq + 1);
        sweepSpec.values = app::parseSweepValues(sweepValuesText);
      } else if (a == "--probes") { // x,y,z;x,y,z in mm (board coordinates, z above the bottom copper)
        probesText = next();
      } else if (a == "--ui-click") { // X,Y in window pixels, after the tab is selected
        std::sscanf(next(), "%f,%f", &clickX, &clickY);
        clickWanted = true;
      } else if (a == "--ui-export") { // after the run: write the 3D volume for ParaView (and the isosurface)
        exportAfterRun = true;
      } else if (a == "--geom") { // NET.x=MM or NET.y=MM: stretch a net's copper (solver side only)
        geomOverrides.push_back(board::parseGeometryOverride(next()));
      } else if (a ==
                 "--nets") { // net-driven selection: the parts and nets they reach follow (board/netwalk.hpp)
        std::stringstream ss(next());
        for (std::string n; std::getline(ss, n, ',');)
          pickedNets.insert(n);
      } else if (a == "--ground-nets") { // added to the ground and plane nets guessed from names and areas
        std::stringstream ss(next());
        for (std::string n; std::getline(ss, n, ',');)
          extraGround.insert(n);
      } else if (a ==
                 "--part") { // REF=open | short | fit | r600,l100n,c0.2p (a two-terminal part's treatment)
        const std::string kv = next();
        const auto eq = kv.find('=');
        if (eq == std::string::npos)
          throw std::runtime_error("--part needs REF=open|short|fit|r<ohm>,l<henry>,c<farad>, got '" + kv +
                                   "'");
        board::PartTreatment t;
        std::stringstream ss(kv.substr(eq + 1));
        for (std::string tok; std::getline(ss, tok, ',');) {
          if (tok == "open")
            t.state = board::PartTreatment::State::Open;
          else if (tok == "short")
            t.state = board::PartTreatment::State::Short;
          else if (tok == "fit")
            t.fitDnp = true;
          else if (tok.size() > 1 && (tok[0] == 'r' || tok[0] == 'l' || tok[0] == 'c')) {
            const std::string v = tok.substr(1);
            const auto x = tok[0] == 'r'   ? board::parseResistance(v)
                           : tok[0] == 'l' ? board::parseInductance(v + (v.back() == 'H' ? "" : "H"))
                                           : board::parseCapacitance(v);
            if (!x)
              throw std::runtime_error("--part: cannot read '" + tok + "'");
            t.state = board::PartTreatment::State::Values;
            (tok[0] == 'r' ? t.r : tok[0] == 'l' ? t.l : t.c) = x;
          } else
            throw std::runtime_error("--part: unknown '" + tok + "'");
        }
        selection.parts[kv.substr(0, eq)] = t;
      } else if (a == "--body") { // REF=ignore | metal | dielectric:EPS (a part's body treatment)
        const std::string kv = next();
        const auto eq = kv.find('=');
        if (eq == std::string::npos)
          throw std::runtime_error("--body needs REF=ignore|metal|dielectric:EPS[:TAND], got '" + kv + "'");
        const std::string what = kv.substr(eq + 1);
        board::BodyTreatment t;
        if (what == "ignore")
          t.kind = board::BodyKind::Ignore;
        else if (what == "metal")
          t.kind = board::BodyKind::Metal;
        else if (what.rfind("dielectric:", 0) == 0) {
          t.kind = board::BodyKind::Dielectric;
          t.epsilonR = std::atof(what.c_str() + 11);
          if (const auto colon = what.find(':', 11); colon != std::string::npos) {
            t.lossTangent = std::atof(what.c_str() + colon + 1);
            if (!(t.lossTangent >= 0.0))
              throw std::runtime_error("--body: loss tangent must not be negative, got '" + what + "'");
          }
          if (!(t.epsilonR >= 1.0))
            throw std::runtime_error("--body: permittivity must be at least 1, got '" + what + "'");
        } else
          throw std::runtime_error("--body needs REF=ignore|metal|dielectric:EPS[:TAND], got '" + kv + "'");
        t.label = selection.bodies[kv.substr(0, eq)].label; // keep a label given earlier
        selection.bodies[kv.substr(0, eq)] = t;
      } else if (a == "--body-label") { // REF=TEXT: how well that body model matches a datasheet (run notes)
        const std::string kv = next();
        const auto eq = kv.find('=');
        if (eq == std::string::npos)
          throw std::runtime_error("--body-label needs REF=TEXT, got '" + kv + "'");
        selection.bodies[kv.substr(0, eq)].label = kv.substr(eq + 1);
      } else if (a == "--refine-gaps") {
        refineGaps = std::atof(next());
        if (refineGaps < 2)
          throw std::runtime_error("--refine-gaps needs a number of cells across the gap, at least 2");
      } else if (a == "--refine") {
        double v[5];
        for (double& x : v)
          x = std::atof(next()) * 1e-3;
        refine.push_back({v[0], v[1], v[2], v[3], v[4]});
      } else if (a == "--device")
        deviceSel = next();
      else if (a == "--cpu" || a == "--gpu")
        backendFlag = a.substr(2);
      else if (a == "--view")
        view = true;
      else if (a == "--frames")
        frameCount = std::atoi(next());
      else if (a == "--dry-run")
        dryRun = true;
      else if (a == "--keep-fields")
        keepFields = true;
      else if (a == "--checkpoint-dir")
        checkpointDir = next();
      else if (a == "--checkpoint-every")
        checkpointEvery = std::atof(next());
      else if (a == "--resume")
        resumeRun = true;
      else if (a == "--ui")
        uiMode = true;
      else if (a == "--autorun")
        autorun = true;
      else if (a == "--ui-screenshot")
        shotFile = next();
      else if (a == "--ui-size") {
        std::sscanf(next(), "%ux%u", &shotW, &shotH);
      } else if (a == "--ui-query")
        shotQuery = next();
      else if (a == "--ui-expand")
        shotExpand = next();
      else if (a == "--ui-tab")
        shotTab = next();
      else if (a == "--ui-run")
        shotRun = true;
      else if (a ==
               "--open-run") // show a saved run (its folder) without solving; needs --ui or --ui-screenshot
        openRunDir = next();
      else if (a == "--ui-farm-result") // show the S11 of a fetched farm job (screenshots)
        farmResultDir = next();
      else if (a == "--prune-runs") // delete the field data of all but the newest N saved runs, then exit
        pruneKeep = std::atoi(next());
      else if (a == "--ui-a11y-dump")
        a11yDump = next();
      else if (a == "--ui-theme")
        shotTheme = next();
      else if (a == "--ui-layers")
        layersFlag = next();
      else if (a == "--ui-legends")
        legendsFlag = std::string(next()) == "open" ? 1 : 0;
      else if (a == "--ui-frames")
        windowFrames = std::atoi(next());
      else if (a == "--cpu-ui")
        softUi = true;
      else if (a == "--ui-bench")
        benchFrames = std::atoi(next());
      else if (a == "--ui-show-settings")
        showSettings = true;
      else if (a == "--ui-hide-controls")
        hideControls = true;
      else if (a == "--json")
        ; // read before the loop: the document goes to stdout, everything else to stderr
      else {
        usage();
        return fail(2, "unknown option " + a + "; usage is on stderr");
      }
    }
    if (jsonMode && (uiMode || !shotFile.empty() || view || !sweepSpec.key.empty() || pruneKeep >= 0))
      return fail(2,
                  "--json covers --list-settings, --list-devices, --run-info, --dry-run and a headless run, "
                  "not the window, --view, --sweep or --prune-runs");

    if (keepFields && (uiMode || !shotFile.empty() || !sweepSpec.key.empty() || pruneKeep >= 0))
      return fail(2, "--keep-fields is for a headless run: the window keeps its field data by the setting "
                     "'save_fields', and a sweep does not keep fields");
    if (resumeRun && checkpointDir.empty())
      return fail(2, "--resume needs --checkpoint-dir DIR (the folder the checkpoints are in)");
    if (!checkpointDir.empty() && (uiMode || !shotFile.empty() || view || frameCount > 0 ||
                                   !sweepSpec.key.empty() || pruneKeep >= 0 || dryRun))
      return fail(2, "--checkpoint-dir is for a headless run without animation views: the window, --view, "
                     "--frames, --sweep and --dry-run keep phasor lists or do not solve, which a checkpoint "
                     "does not cover");
    if (!checkpointDir.empty() && !(checkpointEvery > 0))
      return fail(2, "--checkpoint-every needs a number of seconds above 0");

    const app::ProjectPaths paths(projectDir);
    if (pruneKeep >= 0) {
      const auto freed = app::pruneFields(paths.resultsRoot, static_cast<std::size_t>(pruneKeep));
      std::printf("removed the field data of all but the newest %d runs: %.1f MB freed (S11 and summaries "
                  "kept)\n",
                  pruneKeep, static_cast<double>(freed) / 1e6);
      return 0;
    }
    if (fs::exists(paths.settingsFile)) { // this board's saved settings, then the command line on top
      std::vector<std::string> problems;
      settings.fromJson(slurp(paths.settingsFile), problems);
      for (const auto& p : problems)
        std::printf("settings file: %s\n", p.c_str());
    }
    if (!profileArg.empty()) {
      const auto colon = profileArg.find(':');
      settings.selectProfile(profileArg.substr(0, colon),
                             colon == std::string::npos ? "" : profileArg.substr(colon + 1));
    }
    for (const auto& [key, value] : sets) {
      char* end = nullptr;
      const double number = std::strtod(value.c_str(), &end);
      settings.set(key, settings.def(key).kind == app::Kind::Number && end != value.c_str() && *end == 0
                            ? app::Value(number)
                            : app::Value(value));
    }
    if (!backendFlag.empty())
      settings.set("backend", backendFlag);

    auto snap = board::parseSnapshot(slurp(snapshotPath));
    std::vector<std::string> geomNotes; // every override is printed and goes into the run summary
    if (!geomOverrides.empty()) {
      geomNotes = board::applyGeometryOverrides(snap, geomOverrides);
      for (const std::string& n : geomNotes)
        std::fprintf(stderr, "%s\n", n.c_str());
    }
    if (!pickedNets
             .empty()) { // net-driven selection: walk from the picked nets, then keep only what it reaches
      auto ground = board::guessGroundNets(snap);
      ground.insert(extraGround.begin(), extraGround.end());
      const auto walk = board::walkNets(snap, pickedNets, ground, selection.parts);
      board::applyNetSelection(selection, walk, ground);
      for (const std::string& n : selection.netNotes)
        std::fprintf(stderr, "%s\n", n.c_str());
    }
    selection.refine.insert(selection.refine.end(), refine.begin(), refine.end()); // the typed boxes
    refine.clear();
    std::vector<std::string> refineNotes; // suggestions that are listed but not applied
    {
      const auto suggestions = board::suggestRefinement(snap, selection, refineGaps > 0 ? refineGaps : 6.0);
      for (const auto& g : suggestions) {
        if (refineGaps > 0 && !(uiMode || !shotFile.empty())) {
          selection.refine.push_back(g.box);
          geomNotes.push_back("refinement (--refine-gaps " + std::to_string(static_cast<int>(refineGaps)) +
                              "): " + g.reason);
        } else {
          refineNotes.push_back("refinement suggestion, not applied: " + g.reason +
                                " (apply with --refine-gaps 6, or switch it on in the window)");
        }
      }
    }
    if (uiMode || !shotFile.empty()) {
      std::shared_ptr<vkcore::Context> ctxHolder;
      auto chooseBackend = [&](const std::string& requested) {
        ui::Backend b;
        if (requested != "cpu" && !ctxHolder) {
          try {
            ctxHolder = std::make_shared<vkcore::Context>(vkcore::ContextOptions{.device = deviceSel});
          } catch (const std::exception&) {
            if (requested == "gpu" || !deviceSel.empty())
              throw;
          }
        }
        if (requested != "cpu" && ctxHolder) {
          b.gpu = true;
          b.name = std::string("GPU (") + ctxHolder->properties().deviceName + ")";
          b.details = ctxHolder->floatControls();
          b.deviceBytes = static_cast<double>(ctxHolder->deviceLocalBytes());
          auto ctx = ctxHolder;
          b.factory = [ctx](fdtd::Solver<float>& s) { return std::make_unique<fdtd_vk::GpuEngine>(*ctx, s); };
        } else {
          b.name = "CPU reference";
          b.factory = sim::makeCpuEngine;
        }
        return b;
      };
      ui::Env env{catalog, settings, snap, snapshotPath.string(), paths, selection, chooseBackend, 1.0f};
      if (!shotFile.empty()) { // render one screen to an image, no window
        ui::Ui screen(std::move(env));
        screen.setHeadless(shotW, shotH);
        if (!openRunDir.empty()) {
          std::vector<std::uint8_t> warm;
          screen.frame(0.0, shotW, shotH, warm); // a first frame, so the widgets exist
          if (!screen.openRun(openRunDir)) {
            std::fprintf(stderr, "emfdtd: %s\n", screen.statusMessage().c_str());
            return 1;
          }
        }
        if (!shotQuery.empty())
          screen.setSearch(shotQuery);
        if (!shotExpand.empty())
          screen.expandGroup(shotExpand);
        screen.setPanels(showSettings, !hideControls);
        screen.setAccessWanted(!a11yDump.empty());
        screen.setLayers(layersFlag);
        if (!probesText.empty())
          screen.setProbes(probesText);
        if (legendsFlag >= 0)
          screen.setLegends(legendsFlag == 1);
        if (!shotTheme.empty())
          screen.setTheme(shotTheme);
        screen.setAdaptive(benchFrames >
                           0); // still images render at full size; the benchmark measures the adaptive path
        std::vector<std::uint8_t> rgba;
        for (double t : {0.0, 1.0, 1.5, 2.0}) // a few frames: the model is prepared after a short settle time
          screen.frame(t, shotW, shotH, rgba);
        if (shotRun) { // run to the end (or 5 minutes), then capture
          screen.start();
          const auto begin = std::chrono::steady_clock::now();
          double t = 3.0;
          while (!screen.finished() && std::chrono::steady_clock::now() - begin < std::chrono::minutes(5)) {
            screen.frame(t += 0.1, shotW, shotH, rgba);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
          }
        }
        if (!sweepSpec.key.empty()) { // run the sweep to the end, show its overlay in the S11 tab
          screen.sweepNow(sweepSpec.key, sweepValuesText);
          screen.selectTab("S11 plot");
          screen.setLayers("sweep");
          for (int n = 0; n < 4; ++n)
            screen.frame(50.0 + 0.1 * n, shotW, shotH, rgba);
        }
        if (!farmResultDir.empty())
          screen.openFarmResult(farmResultDir);
        if (!shotTab.empty())
          screen.selectTab(shotTab);
        for (int n = 0; n < 16;
             ++n) { // frames for the camera to settle (auto-fit eases towards the activity)
          screen.frame(100.0 + 0.1 * n, shotW, shotH, rgba);
          if (shotTab == "Farm") // its data comes from the farm tool in the background: give it time
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        if (exportAfterRun) {
          screen.exportVolume();
          std::printf("%s\n", screen.statusMessage().c_str());
        }
        if (clickWanted) { // a left click, then a few frames for the result to settle
          screen.pressAt(clickX, clickY);
          screen.frame(120.0, shotW, shotH, rgba);
          screen.frame(120.1, shotW, shotH, rgba);
          screen.releaseMouse();
          for (int n = 0; n < 4; ++n) {
            screen.frame(120.2 + 0.1 * n, shotW, shotH, rgba);
            if (shotTab == "Farm") // a button there may start the farm tool: give it time to answer
              std::this_thread::sleep_for(std::chrono::milliseconds(400));
          }
        }
        int a11yProblems = 0;
        if (!a11yDump.empty()) { // what a screen reader is told, and which controls have no name
          std::ofstream(a11yDump) << screen.accessTree().dump();
          for (const std::string& p : screen.accessTree().problems()) {
            std::fprintf(stderr, "emfdtd: accessibility: %s\n", p.c_str());
            ++a11yProblems;
          }
          std::printf("wrote %s (%zu nodes)\n", a11yDump.c_str(), screen.accessTree().nodes().size());
        }
        if (benchFrames > 0) { // average frame cost by part, at the requested window size
          ui::Ui::FrameTimes sum;
          for (int n = 0; n < benchFrames; ++n) {
            screen.frame(200.0 + 0.04 * n, shotW, shotH, rgba);
            const auto& t = screen.frameTimes();
            sum = {sum.layout + t.layout, sum.slice + t.slice, sum.volume + t.volume, sum.ui + t.ui,
                   sum.total + t.total};
          }
          const double k = 1.0 / benchFrames;
          std::printf("frame time (ms) at %ux%u: layout %.1f, slice %.1f, volume %.1f, ui raster %.1f, total "
                      "%.1f => %.1f fps\n",
                      shotW, shotH, sum.layout * k, sum.slice * k, sum.volume * k, sum.ui * k, sum.total * k,
                      1000.0 / (sum.total * k));
        }
        std::vector<std::uint8_t> rgb;
        rgb.reserve(rgba.size() / 4 * 3);
        for (std::size_t i = 0; i < rgba.size(); i += 4)
          rgb.insert(rgb.end(), {rgba[i], rgba[i + 1], rgba[i + 2]});
        viz::writePng(shotFile, shotW, shotH, rgb);
        std::printf("wrote %s\n", shotFile.c_str());
        return a11yProblems > 0 ? 1 : 0;
      }
      viz::Window window("kicad-em-fdtd", 1280, 800);
      env.pixelScale = window.pixelScale();
      ui::Ui screen(std::move(env));
      if (!openRunDir.empty() && !screen.openRun(openRunDir))
        std::fprintf(stderr, "emfdtd: %s\n", screen.statusMessage().c_str());
      if (!shotTab.empty()) // the plugin's Farm button starts the window on that tab
        screen.selectTab(shotTab);
      ImGui_ImplGlfw_InitForOther(window.handle(), true);
      std::unique_ptr<viz::GpuUi>
          gpuUi; // the UI drawn by the GPU; the CPU rasteriser if that is not possible
      if (!softUi) {
        try {
          gpuUi = std::make_unique<viz::GpuUi>(window);
          screen.setGpuUi([&gpuUi] { gpuUi->fontsChanged(); });
        } catch (const std::exception& e) {
          gpuUi.reset();
          std::fprintf(stderr, "emfdtd: UI on the CPU (GPU drawing not used: %s)\n", e.what());
        }
      }
      double frameMs = 0, presentMs = 0;
      int framesDone = 0;
      const auto reader = viz::ScreenReader::create(window.handle()); // VoiceOver etc.; null if unsupported
      // One window per project: the plugin finds it through this file and, on a new export, just rewrites
      // snapshot.json, which this loop notices.
      fs::create_directories(paths.root);
      const fs::path pidFile = paths.root / "solver.pid";
      std::ofstream(pidFile) << getpid() << "\n";
      std::error_code ec;
      auto lastWrite = fs::last_write_time(snapshotPath, ec);
      double lastCheck = 0;
      bool pendingRun = autorun; // start once the first frame has built the model
      std::vector<std::uint8_t> rgba;
      while (!window.shouldClose() && !screen.wantsQuit()) {
        window.pollEvents();
        const VkExtent2D ex = window.extent();
        if (ex.width == 0 || ex.height == 0) {
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
          continue;
        }
        if (glfwGetTime() - lastCheck > 0.5 && !screen.busy()) {
          lastCheck = glfwGetTime();
          const auto written = fs::last_write_time(snapshotPath, ec);
          if (!ec && written != lastWrite) {
            lastWrite = written;
            try {
              if (screen.setSnapshot(board::parseSnapshot(slurp(snapshotPath)))) {
                window.focus();
                const fs::path request = paths.root / "request.txt";
                if (fs::exists(request)) { // "run" solves, "farm" opens the Farm tab
                  std::string word;
                  std::ifstream(request) >> word;
                  fs::remove(request, ec);
                  pendingRun = word == "run";
                  if (word == "farm")
                    screen.selectTab("Farm");
                }
              }
            } catch (const std::exception& e) {
              std::fprintf(stderr, "emfdtd: could not reload %s: %s\n", snapshotPath.string().c_str(),
                           e.what());
            }
          }
        }
        ImGui_ImplGlfw_NewFrame();
        if (gpuUi)
          gpuUi->newFrame();
        if (reader) { // the tree is only built once a screen reader attaches
          screen.setAccessWanted(reader->active());
          screen.perform(reader->takeActions());
        }
        const auto tFrame = std::chrono::steady_clock::now();
        screen.frame(glfwGetTime(), ex.width, ex.height, rgba);
        const auto tPresent = std::chrono::steady_clock::now();
        if (reader)
          reader->update(screen.accessTree(), glfwGetWindowAttrib(window.handle(), GLFW_FOCUSED) != 0);
        if (pendingRun && screen.screen() == ui::Screen::Preview) {
          screen.start();
          pendingRun = false; // one attempt per request; if the model cannot run, the preview says why
        }
        if (gpuUi)
          gpuUi->present(rgba, ex.width, ex.height, ImGui::GetDrawData());
        else
          window.presentRgba(rgba, ex.width, ex.height);
        const auto tDone = std::chrono::steady_clock::now();
        frameMs += std::chrono::duration<double, std::milli>(tPresent - tFrame).count();
        presentMs += std::chrono::duration<double, std::milli>(tDone - tPresent).count();
        if (windowFrames > 0 && ++framesDone >= windowFrames) { // a measurement run: report and leave
          std::printf(
              "window, %s UI: frame() %.1f ms, present %.1f ms (includes waiting for the display) over %d "
              "frames\n",
              gpuUi ? "GPU" : "CPU", frameMs / framesDone, presentMs / framesDone, framesDone);
          break;
        }
      }
      gpuUi.reset(); // before the ImGui context goes
      ImGui_ImplGlfw_Shutdown();
      fs::remove(pidFile, ec);
      return 0;
    }
    auto prepared = app::prepare(snap, settings, selection);
    prepared.model.notes.insert(prepared.model.notes.end(), geomNotes.begin(), geomNotes.end());
    const auto& g = prepared.model.grid;
    const auto& plan = prepared.plan;

    std::unique_ptr<vkcore::Context> ctx;
    bool gpu = plan.backend != "cpu";
    if (gpu) {
      try {
        ctx = std::make_unique<vkcore::Context>(vkcore::ContextOptions{.device = deviceSel});
      } catch (const std::exception& e) {
        if (plan.backend == "gpu" || !deviceSel.empty())
          throw;
        std::printf("no usable GPU (%s); using the CPU reference\n", e.what());
        gpu = false;
      }
    }
    const auto est = app::estimate(prepared, gpu, ctx ? static_cast<double>(ctx->deviceLocalBytes()) : 0.0);

    const auto& variant = settings.currentVariant();
    std::printf("profile: %s / %s  [%s]\n", catalog.profileName(settings.text("profile")).c_str(),
                variant.name.c_str(), variant.status.c_str());
    std::printf(
        "band of interest %.4g to %.4g GHz; S11 reported %.4g to %.4g GHz; source covers %.4g to %.4g GHz\n",
        plan.bandHz[0] * 1e-9, plan.bandHz[1] * 1e-9, plan.sweepHz[0] * 1e-9, plan.sweepHz[1] * 1e-9,
        plan.excitationHz[0] * 1e-9, plan.excitationHz[1] * 1e-9);
    std::printf("grid %zu x %zu x %zu = %.2f M cells, smallest cell %.3g mm, time step %.3g ps\n",
                g.cells(fdtd::X), g.cells(fdtd::Y), g.cells(fdtd::Z),
                static_cast<double>(g.cellCount()) * 1e-6, est.smallestCell * 1e3, est.timeStep * 1e12);
    std::printf("estimate: ~%zu steps, %.0f MB, about %.0f s on the %s%s\n", est.steps,
                est.memoryBytes * 1e-6, est.seconds, gpu ? "GPU" : "CPU",
                est.exceedsMemory ? "  ** exceeds the device memory budget **" : "");
    for (const auto& key : settings.modifiedKeys())
      std::printf("changed from default: %s\n", key.c_str());
    for (const auto& n : prepared.model.notes)
      std::printf("note: %s\n", n.c_str());
    for (const auto& n : refineNotes)
      std::printf("note: %s\n", n.c_str());
    const std::string deviceName = ctx ? ctx->properties().deviceName : "";
    auto facts = app::clijson::runFacts(settings, prepared, est, gpu, deviceName, refineNotes);
    try {
      facts.fieldBytes = app::estimateFieldBytes(snap, prepared);
    } catch (const std::exception&) { // an estimate only: the run itself reports what it could not do
    }
    if (keepFields)
      std::printf("field data: about %.0f MB on disk with --keep-fields\n", facts.fieldBytes * 1e-6);
    if (dryRun) {
      if (jsonMode)
        emitJson(app::clijson::dryRun(FDTD_VERSION, facts));
      return 0;
    }
    if (est.exceedsMemory)
      throw std::runtime_error(
          "the model would not fit in device memory; choose a coarser mesh preset or a smaller region");

    sim::EngineFactory factory = sim::makeCpuEngine;
    if (gpu) {
      factory = [&](fdtd::Solver<float>& s) { return std::make_unique<fdtd_vk::GpuEngine>(*ctx, s); };
      std::printf("backend: GPU (%s)\n", ctx->properties().deviceName);
      if (!ctx->floatControls().empty())
        std::printf("float controls: %s\n", ctx->floatControls().c_str());
    } else {
      std::printf("backend: CPU reference\n");
    }

    if (!sweepSpec.key
             .empty()) { // one run per value, then a table; S11 curves are in <results>/sweep-*/sweep.json
      app::SweepEnv senv{snap,    snapshotPath.string(),        catalog, paths, selection,
                         factory, gpu ? "GPU" : "CPU reference"};
      senv.notes = geomNotes;
      senv.gpu = gpu;
      senv.deviceBytes = ctx ? static_cast<double>(ctx->deviceLocalBytes()) : 0.0;
      senv.stamp = app::timestamp();
      senv.progress = [&](std::size_t i, std::size_t n, const app::SweepPoint* done) {
        if (!done)
          std::printf("sweep %zu/%zu: %s = %s ...\n", i + 1, n, sweepSpec.key.c_str(),
                      sweepSpec.values[i].c_str());
        else if (!done->error.empty())
          std::printf("  failed: %s\n", done->error.c_str());
        else
          std::printf("  best %.2f dB at %.4f GHz  -> %s\n", done->bestDb, done->bestHz * 1e-9,
                      done->dir.string().c_str());
      };
      app::runSweep(senv, settings, sweepSpec);
      return 0;
    }
    sim::RunOptions& opt = prepared.options;
    const auto& model = prepared.model;

    // Checkpoints (docs/checkpoint.md): the identity a checkpoint must match, the store, and the one to
    // resume.
    std::optional<app::checkpoint::Store> store;
    std::optional<app::checkpoint::Loaded> loaded;
    app::checkpoint::Identity ckptId;
    fs::path runDir;
    if (!checkpointDir.empty()) {
      store.emplace(checkpointDir);
      ckptId.runHash = app::checkpoint::hashRun(model, opt);
      ckptId.backend = gpu ? "gpu" : "cpu";
      if (ctx) {
        ckptId.deviceUuid = ctx->deviceInfo().uuid;
        ckptId.deviceName = ctx->deviceInfo().name;
        ckptId.driver = ctx->deviceInfo().driver;
      }
      ckptId.binaryVersion = FDTD_VERSION;
      ckptId.nodes = {g.nodes(fdtd::X), g.nodes(fdtd::Y), g.nodes(fdtd::Z)};
      ckptId.dt = opt.courant * fdtd::maxStableDt(g);
      if (resumeRun) {
        loaded = store->load(ckptId); // throws with the reason if nothing can be resumed
        runDir = loaded->identity.runDir;
        fs::create_directories(runDir);
        std::error_code ec;
        fs::remove(store->dir() / "stop", ec); // the request that paused the run is answered
        std::printf("resuming from the checkpoint at step %zu (%s)\n", loaded->state.steps,
                    store->path(loaded->generation).string().c_str());
      } else {
        store->clear(); // the files of an earlier run must not be mistaken for this one's
      }
      std::signal(SIGTERM, onPauseSignal); // SIGTERM: write a checkpoint, then end with status 75
    }
    if (runDir.empty())
      runDir = paths.newRunDir(app::timestamp());
    std::printf("results: %s\n", runDir.string().c_str());

    // Animation: accumulate Ez on a plane mid-way between the outer copper layers at 25 frequencies in the
    // band.
    std::mutex phasorMutex;
    std::unique_ptr<viz::PhasorAccumulator> phasors;
    std::unique_ptr<app::PhasorFeed> feed;
    viz::Plane plane{};
    std::vector<std::uint8_t> mask;
    if (frameCount > 0 || view) {
      std::size_t kTop = 0, kBottom = g.nodes(fdtd::Z);
      for (std::size_t i = 0; i < snap.stackup.size(); ++i)
        if (snap.stackup[i].copper) {
          kTop = std::max(kTop, model.copperPlaneNode[i]);
          kBottom = std::min(kBottom, model.copperPlaneNode[i]);
        }
      plane = viz::makePlane(g, fdtd::Z, (kTop + kBottom) / 2);
      std::vector<double> freqs(25);
      for (std::size_t i = 0; i < freqs.size(); ++i)
        freqs[i] = plan.sweepHz[0] +
                   (plan.sweepHz[1] - plan.sweepHz[0]) * static_cast<double>(i) / (freqs.size() - 1);
      phasors = std::make_unique<viz::PhasorAccumulator>(freqs, plane.nu * plane.nv);
      mask.assign(plane.nu * plane.nv, 0);
      for (const auto& e : model.pecEdges)
        if (e.axis != fdtd::Z && e.node / (g.nodes(fdtd::X) * g.nodes(fdtd::Y)) == kTop)
          mask[e.node % (g.nodes(fdtd::X) * g.nodes(fdtd::Y))] = 1;
      const auto filled = mask; // draw only the outline of the copper, so the field under it stays visible
      for (std::size_t j = 1; j + 1 < plane.nv; ++j)
        for (std::size_t i = 1; i + 1 < plane.nu; ++i) {
          const std::size_t n = i + plane.nu * j;
          if (filled[n] && filled[n - 1] && filled[n + 1] && filled[n - plane.nu] && filled[n + plane.nu])
            mask[n] = 0;
        }
      std::vector<fdtd::PhasorChannel> channels;
      for (std::size_t b = 0; b < plane.nv; ++b)
        for (std::size_t a = 0; a < plane.nu; ++a) {
          std::array<std::size_t, 3> idx{};
          idx[plane.normal] = plane.index;
          idx[plane.u] = a;
          idx[plane.v] = b;
          channels.push_back(
              {fdtd::Kind::E, fdtd::Z, static_cast<std::uint32_t>(g.nodeIndex(idx[0], idx[1], idx[2]))});
        }
      feed = std::make_unique<app::PhasorFeed>(*phasors, std::move(channels), freqs);
      opt.onSample = [&](sim::Engine& eng, double, double dtSample) {
        feed->sample(eng, dtSample, phasorMutex);
      };
      opt.onFinish = [&](sim::Engine& eng) { feed->sync(eng, phasorMutex); };
    }
    // --keep-fields (docs/ux.md, "Saved runs"): the same field data a window run keeps. The accumulators are
    // not part of a checkpoint (the solver refuses phasor lists with checkpoints), so a run that uses
    // --checkpoint-dir, fresh or resumed, keeps none and says why.
    app::FieldRecorder recorder;
    std::mutex fieldMutex;
    std::string fieldsNote; // why requested fields were not kept
    const bool recordFields = keepFields && !store;
    if (keepFields && store)
      fieldsNote = std::string("fields requested but not kept: ") +
                   (loaded ? "the run was resumed from a checkpoint, " : "the run writes checkpoints, ") +
                   "and the field accumulators are not part of a checkpoint (the solver refuses both); run "
                   "without --checkpoint-dir to keep fields";
    if (recordFields) {
      recorder.layout(snap, prepared);
      recorder.allocate();
      recorder.attach(opt, g, fieldMutex, nullptr);
    }
    std::atomic<bool> done{false}, stop{false};
    opt.shouldStop = [&] { return stop.load(); };
    auto lastCheckpoint = std::chrono::steady_clock::now();
    if (store) {
      opt.checkpointDue = [&](std::size_t) {
        return std::chrono::steady_clock::now() - lastCheckpoint >=
               std::chrono::duration<double>(checkpointEvery);
      };
      opt.pauseRequested = [&] {
        std::error_code ec;
        return gPauseSignal.load() || fs::exists(store->dir() / "stop", ec);
      };
      opt.onCheckpoint = [&](const sim::Checkpoint& cp) {
        const auto w0 = std::chrono::steady_clock::now();
        app::checkpoint::Identity id = ckptId;
        id.runDir = runDir.string();
        store->save(id, cp);
        lastCheckpoint = std::chrono::steady_clock::now();
        std::printf("checkpoint: step %zu written (%.0f MB, %.1f s)\n", cp.steps,
                    static_cast<double>(cp.host.size() + cp.engine.size()) * 1e-6,
                    std::chrono::duration<double>(lastCheckpoint - w0).count());
      };
      if (loaded)
        opt.resume = &loaded->state;
    }
    sim::RunResult r;
    std::exception_ptr failure;
    const auto t0 = std::chrono::steady_clock::now();
    // A progress line every few seconds, for people watching a log and for the job farm (which turns it into
    // a progress bar). The percentage is how far the signal has decayed toward the stop level: an estimate.
    auto lastProgress = t0;
    opt.onProgress = [&](const sim::RunResult& rr) {
      const auto now = std::chrono::steady_clock::now();
      if (now - lastProgress < std::chrono::seconds(5))
        return;
      lastProgress = now;
      std::printf("progress: %zu steps, about %d%%, %.0f s\n", rr.steps,
                  static_cast<int>(rr.progress * 100.0), std::chrono::duration<double>(now - t0).count());
    };
    auto work = [&] {
      try {
        r = sim::runOnePort(model, opt, factory);
      } catch (...) {
        failure = std::current_exception();
      }
      done = true;
    };
    if (view) {
      std::thread worker(work);
      std::exception_ptr viewFailure;
      try {
        runViewer(g, plane, *phasors, phasorMutex, mask, done, stop, r);
      } catch (...) {
        viewFailure = std::current_exception();
      }
      stop = true; // the solver thread must be joined even if the window failed
      worker.join();
      if (viewFailure)
        std::rethrow_exception(viewFailure);
    } else {
      work();
    }
    if (failure)
      std::rethrow_exception(failure);
    if (r.paused) {
      std::printf("paused: checkpoint at step %zu\n", r.steps);
      if (jsonMode)
        emitJson(app::clijson::paused(FDTD_VERSION, runDir.string(), r.steps, checkpointDir));
      return 75; // EX_TEMPFAIL: not an error, resume with --resume
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const std::size_t stepsHere = r.steps - r.resumedFromStep; // a resumed run times only its own part
    const double mcells = static_cast<double>(g.cellCount()) * static_cast<double>(stepsHere) / secs * 1e-6;
    std::printf("%zu steps of %.3g ps in %.1f s (%.0f Mcells/s)%s\n", r.steps, r.dt * 1e12, secs, mcells,
                r.resumedFromStep ? "; the first part is from the checkpoint" : "");

    const std::uint64_t gpuBytes = ctx ? ctx->peakBufferBytes() : 0;
    if (gpuBytes)
      std::printf("device memory: %.1f MB in buffers at the peak (estimate %.1f MB)\n", gpuBytes * 1e-6,
                  est.memoryBytes * 1e-6);
    std::ofstream out(runDir / "s11.s1p");
    fdtd::writeTouchstone(
        out, r.s, "kicad-em-fdtd S11, board " + snap.boardName + ", profile " + settings.text("profile"));
    std::ofstream(runDir / "settings.json") << settings.toJson();
    fs::copy_file(snapshotPath, runDir / "board.json");
    {
      std::ofstream sum(runDir / "summary.txt");
      sum << "kicad-em-fdtd run summary\nboard: " << snap.boardName
          << "\nprofile: " << settings.text("profile") << " / " << variant.name << " [" << variant.status
          << "]\nsource of the band: " << variant.source << "\ngrid: " << g.cells(fdtd::X) << " x "
          << g.cells(fdtd::Y) << " x " << g.cells(fdtd::Z) << " cells, smallest cell "
          << est.smallestCell * 1e3 << " mm\nsteps: " << r.steps << ", dt " << r.dt * 1e12 << " ps, backend "
          << (gpu ? "GPU" : "CPU") << "\n";
      if (r.resumedFromStep)
        sum << "resumed at step " << r.resumedFromStep
            << " from a checkpoint (the earlier part ran on the same "
            << "device)\n";
      for (const auto& key : settings.modifiedKeys())
        sum << "changed from default: " << key << "\n";
      for (const auto& n : prepared.model.notes)
        sum << "note: " << n << "\n";
    }
    std::size_t best = 0;
    for (std::size_t i = 1; i < r.s.s11.size(); ++i)
      if (std::abs(r.s.s11[i]) < std::abs(r.s.s11[best]))
        best = i;
    std::uint64_t fieldsBytes = 0;
    bool fieldsKept = false;
    if (recordFields) {
      try {
        recorder.farResults = recorder.computeFar(r, plan.portImpedanceOhm);
        recorder.save(runDir);
        fieldsKept = true;
        fieldsBytes = app::folderBytes(runDir / "fields");
      } catch (const std::exception& e) {
        fs::remove_all(runDir / "fields");
        fieldsNote = std::string("fields requested but not kept: could not save them: ") + e.what();
      }
    }
    if (keepFields) { // the manifest is what makes the folder a saved run that --open-run accepts
      app::RunManifest man;
      man.id = runDir.filename().string();
      man.board = snap.boardName;
      man.profile = settings.text("profile");
      man.backend = gpu ? "GPU" : "CPU reference";
      man.steps = r.steps;
      man.wallSeconds = secs;
      if (!r.s.s11.empty()) {
        man.bestDb = 20 * std::log10(std::max(std::abs(r.s.s11[best]), 1e-6));
        man.bestHz = r.s.frequenciesHz[best];
      }
      man.fields = fieldsKept;
      app::writeManifest(runDir, man);
      std::ofstream sum(runDir / "summary.txt", std::ios::app);
      if (fieldsKept)
        sum << "fields: kept, " << fieldsBytes << " bytes\n";
      else
        sum << "note: " << fieldsNote << "\n";
      std::printf("%s\n", fieldsKept
                              ? ("fields: kept, " + std::to_string(fieldsBytes / 1000000) + " MB").c_str()
                              : fieldsNote.c_str());
    }
    std::printf("best match %.4g dB at %.4f GHz (band %.4g to %.4g GHz)\n",
                20 * std::log10(std::abs(r.s.s11[best])), r.s.frequenciesHz[best] * 1e-9,
                plan.bandHz[0] * 1e-9, plan.bandHz[1] * 1e-9);
    if (phasors && frameCount > 0) {
      std::size_t k = 0;
      for (std::size_t i = 1; i < phasors->frequencies(); ++i)
        if (std::abs(phasors->frequencyHz(i) - r.s.frequenciesHz[best]) <
            std::abs(phasors->frequencyHz(k) - r.s.frequenciesHz[best]))
          k = i;
      double peak = 0;
      for (std::size_t v = 0; v < phasors->voxels(); ++v)
        peak = std::max(peak, static_cast<double>(std::abs(phasors->phasor(k, v))));
      viz::FrameSpec spec;
      spec.colourScale = peak;
      fs::create_directories(runDir / "frames");
      for (int f = 0; f < frameCount; ++f) {
        std::uint32_t w = 0, h = 0;
        const auto rgb =
            viz::renderFrame(g, plane, *phasors, k, 2 * 3.14159265358979 * f / frameCount, spec, mask, w, h);
        char name[64];
        std::snprintf(name, sizeof name, "frame_%03d.png", f);
        viz::writePng((runDir / "frames" / name).string(), w, h, rgb);
      }
    }
    if (jsonMode) {
      app::clijson::RunOutcome outcome;
      outcome.runDir = runDir.string();
      outcome.steps = r.steps;
      outcome.timeStepS = r.dt;
      outcome.wallSeconds = secs;
      outcome.mcellsPerSecond = mcells;
      outcome.resumedFromStep = r.resumedFromStep;
      outcome.gpuBytesAllocated = gpuBytes;
      outcome.fieldsRequested = keepFields;
      outcome.fieldsKept = fieldsKept;
      outcome.fieldsBytes = fieldsBytes;
      outcome.fieldsNote = fieldsNote;
      if (!r.s.s11.empty()) {
        outcome.bestHz = r.s.frequenciesHz[best];
        outcome.bestDb = 20 * std::log10(std::abs(r.s.s11[best]));
      }
      emitJson(app::clijson::run(FDTD_VERSION, facts, outcome));
    }
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "emfdtd: %s\n", e.what());
    return fail(1, e.what());
  }
}
