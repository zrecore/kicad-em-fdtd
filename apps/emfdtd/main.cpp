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
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "app/paths.hpp"
#include "app/prepare.hpp"
#include "app/settings.hpp"
#include "board/snapshot.hpp"
#include "board/voxelize.hpp"
#include "core/resources.hpp"
#include "fdtd_vk/gpu_engine.hpp"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "sim/simulation.hpp"
#include "ui.hpp"
#include "version.hpp"
#include "viz/png.hpp"
#include "viz/screen_reader.hpp"
#include "viz/slice.hpp"
#include "viz/window.hpp"

#include <GLFW/glfw3.h>

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

void usage() {
  std::fprintf(
      stderr,
      "usage: emfdtd SNAPSHOT.json [options]\n"
      "  --project-dir DIR        the KiCAD project folder; results go to DIR/em-fdtd/results/run-<time>/\n"
      "                           (default: the folder holding SNAPSHOT.json)\n"
      "  --profile ID[:VARIANT]   radio profile: wifi, bluetooth, zigbee, lwmesh, uwb, custom (default "
      "wifi)\n"
      "  --set KEY=VALUE          change any setting (repeatable); --list-settings shows them\n"
      "  --version                print the version (semver) and exit\n"
      "  --list-settings          print every setting with its default, tier and help, then exit\n"
      "  --include REF[,REF...]   model only these footprints (default: the whole board)\n"
      "  --refine X0 Y0 X1 Y1 MM  extra fine-cell box in board coordinates (mm); repeatable\n"
      "  --cpu | --gpu            force a backend (default: setting 'backend', auto = GPU if present)\n"
      "  --view                   open a window and animate the field while the solve runs\n"
      "  --frames N               write N animation frames into the run folder\n"
      "  --dry-run                prepare and print the plan and estimate, do not solve\n"
      "  --ui                     open the solver window on its first screen (preview and settings)\n"
      "  --autorun                with --ui: start the run as soon as the model is ready\n"
      "  --ui-a11y-dump FILE      with --ui-screenshot: write what a screen reader is told; exit 1 if a "
      "control is unnamed\n"
      "  --ui-screenshot FILE.png [--ui-size WxH] [--ui-query TEXT] [--ui-expand GROUP]  render that screen "
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

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0); // progress lines should appear even if the run is killed
  try {
    if (argc < 2) {
      usage();
      return 2;
    }
    if (std::string(argv[1]) == "--version") {
      std::printf("emfdtd %s\n", FDTD_VERSION);
      return 0;
    }
    const app::Catalog catalog =
        app::Catalog::load(core::resourceDir("data", FDTD_DATA_DIR) + "/radio_profiles.json");
    app::Settings settings(catalog);
    if (std::string(argv[1]) == "--list-settings") {
      listSettings(settings);
      return 0;
    }
    const fs::path snapshotPath = argv[1];
    fs::path projectDir = fs::absolute(snapshotPath).parent_path();
    std::string profileArg;
    std::vector<std::pair<std::string, std::string>> sets;
    std::vector<board::RefineBox> refine;
    board::Selection selection;
    int frameCount = 0;
    bool view = false, dryRun = false, uiMode = false, autorun = false;
    std::string shotFile, shotQuery, shotExpand, shotTab;
    bool shotRun = false, showSettings = false, hideControls = false;
    int benchFrames = 0;
    std::string shotTheme, a11yDump;
    int legendsFlag = -1; // -1 default, 0 collapsed, 1 expanded
    std::uint32_t shotW = 1280, shotH = 800;
    std::string backendFlag;
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
      } else if (a == "--refine") {
        double v[5];
        for (double& x : v)
          x = std::atof(next()) * 1e-3;
        refine.push_back({v[0], v[1], v[2], v[3], v[4]});
      } else if (a == "--cpu" || a == "--gpu")
        backendFlag = a.substr(2);
      else if (a == "--view")
        view = true;
      else if (a == "--frames")
        frameCount = std::atoi(next());
      else if (a == "--dry-run")
        dryRun = true;
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
      else if (a == "--ui-a11y-dump")
        a11yDump = next();
      else if (a == "--ui-theme")
        shotTheme = next();
      else if (a == "--ui-legends")
        legendsFlag = std::string(next()) == "open" ? 1 : 0;
      else if (a == "--ui-bench")
        benchFrames = std::atoi(next());
      else if (a == "--ui-show-settings")
        showSettings = true;
      else if (a == "--ui-hide-controls")
        hideControls = true;
      else {
        usage();
        return 2;
      }
    }

    const app::ProjectPaths paths(projectDir);
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

    const auto snap = board::parseSnapshot(slurp(snapshotPath));
    if (uiMode || !shotFile.empty()) {
      std::shared_ptr<vkcore::Context> ctxHolder;
      auto chooseBackend = [&](const std::string& requested) {
        ui::Backend b;
        if (requested != "cpu" && !ctxHolder) {
          try {
            ctxHolder = std::make_shared<vkcore::Context>();
          } catch (const std::exception&) {
            if (requested == "gpu")
              throw;
          }
        }
        if (requested != "cpu" && ctxHolder) {
          b.gpu = true;
          b.name = std::string("GPU (") + ctxHolder->properties().deviceName + ")";
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
        if (!shotQuery.empty())
          screen.setSearch(shotQuery);
        if (!shotExpand.empty())
          screen.expandGroup(shotExpand);
        screen.setPanels(showSettings, !hideControls);
        screen.setAccessWanted(!a11yDump.empty());
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
        if (!shotTab.empty())
          screen.selectTab(shotTab);
        for (int n = 0; n < 16; ++n) // frames for the camera to settle (auto-fit eases towards the activity)
          screen.frame(100.0 + 0.1 * n, shotW, shotH, rgba);
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
      ImGui_ImplGlfw_InitForOther(window.handle(), true);
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
                if (fs::exists(request)) { // "run" starts the solve, anything else just shows the preview
                  std::string word;
                  std::ifstream(request) >> word;
                  fs::remove(request, ec);
                  pendingRun = word == "run";
                }
              }
            } catch (const std::exception& e) {
              std::fprintf(stderr, "emfdtd: could not reload %s: %s\n", snapshotPath.c_str(), e.what());
            }
          }
        }
        ImGui_ImplGlfw_NewFrame();
        if (reader) { // the tree is only built once a screen reader attaches
          screen.setAccessWanted(reader->active());
          screen.perform(reader->takeActions());
        }
        screen.frame(glfwGetTime(), ex.width, ex.height, rgba);
        if (reader)
          reader->update(screen.accessTree(), glfwGetWindowAttrib(window.handle(), GLFW_FOCUSED) != 0);
        if (pendingRun && screen.screen() == ui::Screen::Preview) {
          screen.start();
          pendingRun = false; // one attempt per request; if the model cannot run, the preview says why
        }
        window.presentRgba(rgba, ex.width, ex.height);
      }
      ImGui_ImplGlfw_Shutdown();
      fs::remove(pidFile, ec);
      return 0;
    }
    auto prepared = app::prepare(snap, settings, selection, refine);
    const auto& g = prepared.model.grid;
    const auto& plan = prepared.plan;

    std::unique_ptr<vkcore::Context> ctx;
    bool gpu = plan.backend != "cpu";
    if (gpu) {
      try {
        ctx = std::make_unique<vkcore::Context>();
      } catch (const std::exception& e) {
        if (plan.backend == "gpu")
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
    if (dryRun)
      return 0;
    if (est.exceedsMemory)
      throw std::runtime_error(
          "the model would not fit in device memory; choose a coarser mesh preset or a smaller region");

    sim::EngineFactory factory = sim::makeCpuEngine;
    if (gpu) {
      factory = [&](fdtd::Solver<float>& s) { return std::make_unique<fdtd_vk::GpuEngine>(*ctx, s); };
      std::printf("backend: GPU (%s)\n", ctx->properties().deviceName);
    } else {
      std::printf("backend: CPU reference\n");
    }

    const fs::path runDir = paths.newRunDir(app::timestamp());
    std::printf("results: %s\n", runDir.string().c_str());
    sim::RunOptions& opt = prepared.options;
    const auto& model = prepared.model;

    // Animation: accumulate Ez on a plane mid-way between the outer copper layers at 25 frequencies in the
    // band.
    std::mutex phasorMutex;
    std::unique_ptr<viz::PhasorAccumulator> phasors;
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
      opt.onSample = [&](const sim::Engine& eng, double t, double dtSample) {
        auto values = viz::extractPlane(g, plane, eng.field(fdtd::Kind::E, fdtd::Z));
        std::lock_guard<std::mutex> lock(phasorMutex);
        phasors->add(t, dtSample, values);
      };
    }
    std::atomic<bool> done{false}, stop{false};
    opt.shouldStop = [&] { return stop.load(); };
    sim::RunResult r;
    std::exception_ptr failure;
    const auto t0 = std::chrono::steady_clock::now();
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
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%zu steps of %.3g ps in %.1f s (%.0f Mcells/s)\n", r.steps, r.dt * 1e12, secs,
                static_cast<double>(g.cellCount()) * static_cast<double>(r.steps) / secs * 1e-6);

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
      for (const auto& key : settings.modifiedKeys())
        sum << "changed from default: " << key << "\n";
      for (const auto& n : prepared.model.notes)
        sum << "note: " << n << "\n";
    }
    std::size_t best = 0;
    for (std::size_t i = 1; i < r.s.s11.size(); ++i)
      if (std::abs(r.s.s11[i]) < std::abs(r.s.s11[best]))
        best = i;
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
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "emfdtd: %s\n", e.what());
    return 1;
  }
}
