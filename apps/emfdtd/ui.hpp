// The solver window's screens: Preview (settings, estimate, board and domain), Running (live S11, progress,
// field slice) and Done (results and where they are). One class, drawn with Dear ImGui through the CPU
// rasteriser, so it can also render to an image without a window (used to check the layout).
#pragma once
#include <atomic>
#include <chrono>
#include <complex>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "app/paths.hpp"
#include "app/prepare.hpp"
#include "app/settings.hpp"
#include "board/snapshot.hpp"
#include "fdtd/surface_current.hpp"
#include "viz/access.hpp"
#include "viz/imgui_soft.hpp"
#include "viz/slice.hpp"
#include "viz/view_axes.hpp"
#include "viz/volume.hpp"

struct ImGuiContext;

struct ImDrawList;
struct ImVec2;

namespace ui {

struct Backend {
  std::string name; // "GPU (Apple M1 Pro)" or "CPU reference"
  bool gpu = false;
  double deviceBytes = 0;
  sim::EngineFactory factory;
};

struct Env {
  const app::Catalog& catalog;
  app::Settings& settings;
  const board::Snapshot& snapshot;
  std::string snapshotPath;
  app::ProjectPaths paths;
  board::Selection selection;
  std::function<Backend(const std::string& requested)> chooseBackend; // "auto", "gpu" or "cpu"
  float pixelScale = 1;                                               // framebuffer pixels per UI point
};

enum class Screen { Preview, Running, Done };

class Ui {
public:
  explicit Ui(Env env);
  ~Ui();
  Ui(const Ui&) = delete;
  Ui& operator=(const Ui&) = delete;

  // Builds the UI for one frame and draws it into rgba (w*h*4, top row first). `now` is seconds on any
  // monotonic clock (drives the search debounce and animation).
  void frame(double now, std::uint32_t w, std::uint32_t h, std::vector<std::uint8_t>& rgba);
  bool wantsQuit() const { return quit_; }
  Screen screen() const { return screen_; }

  // For layout checks without a window.
  void setHeadless(std::uint32_t w, std::uint32_t h);
  void setAdaptive(bool on) { adaptive_ = on; } // adaptive render resolution (off for still images)
  void setSearch(const std::string& text) {
    searchText_ = text;
    searchApplied_ = text;
  }
  void expandGroup(const std::string& g) { expanded_.insert(g); }
  void start(); // begin the solve (the Run button)
  // A new export from KiCAD: replace the board and go back to the preview. Refused (false) while a run is
  // going.
  bool setSnapshot(board::Snapshot s);
  bool busy() const { return screen_ == Screen::Running; }
  void selectTab(const std::string& name) { pendingTab_ = name; }

  // Screen readers. The tree is only built while `on` (an assistive technology has attached, or a check asks
  // for it); it describes the last frame. Requests from the assistive technology are applied on the next
  // frame.
  void setAccessWanted(bool on) { accessWanted_ = on; }
  bool accessWanted() const { return accessWanted_; }
  const viz::access::Tree& accessTree() const { return access_; }
  void perform(const std::vector<viz::access::Action>& actions);
  void setTheme(const std::string& mode) { env_.settings.set("ui_theme", mode); } // for layout checks
  // for screenshots: layers is a comma list of field, nofield, current, nocurrent (both tabs)
  void setLayers(const std::string& layers) {
    auto has = [&](const char* w) {
      return ("," + layers + ",").find(std::string(",") + w + ",") != std::string::npos;
    };
    if (has("nofield"))
      showFieldSlice_ = showField3d_ = false;
    if (has("current"))
      showCurrentSlice_ = showCurrent3d_ = true;
  }
  void setLegends(bool open) { // for layout checks
    for (bool& l : legendOpen_)
      l = open;
  }
  void setPanels(bool settingsOpen, bool controlsOpen) { // for layout checks
    settingsOpen_ = settingsOpen;
    sliceControlsOpen_ = volControlsOpen_ = controlsOpen;
  }
  bool finished() const { return screen_ == Screen::Done; }
  // Milliseconds spent in the last frame, by part (for the fps readout and the benchmark flag).
  struct FrameTimes {
    double layout = 0, slice = 0, volume = 0, ui = 0, total = 0;
  };
  const FrameTimes& frameTimes() const { return times_; }

private:
  void drawSettings();
  void drawSetting(const std::string& key);
  void drawPreviewTab(float w, float h);
  void drawS11Tab(float w, float h);
  void drawSliceTab(float w, float h);
  void drawVolumeTab(float w, float h);
  void drawStatusBar();
  // The field colours the user configured for the dark or the light theme.
  viz::Rgb fieldColour(bool light, bool positive) const;
  void refreshPrepared(double now);
  void runWorker();
  void applyToProject();
  std::string blockedReason() const;

  Env env_;
  board::Snapshot snapshot_; // the board shown; replaced when KiCAD exports again
  ImGuiContext* ctx_ = nullptr;
  std::unique_ptr<viz::SoftImGui> soft_;
  Screen screen_ = Screen::Preview;
  bool quit_ = false, headless_ = false;

  // settings panel
  std::set<std::string> expanded_;
  std::string searchText_, searchApplied_;
  app::Debouncer debounce_{0.2};
  std::string newProfileName_ = "my-profile", message_;
  bool dirty_ = true;
  double lastEdit_ = 0, now_ = 0;

  // model and estimate (rebuilt a moment after the last settings change)
  std::unique_ptr<app::Prepared> prepared_;
  app::Estimate estimate_;
  std::string prepareError_;
  Backend backend_;

  // the run (worker thread) and what the screens read from it, under mu_
  std::thread worker_;
  std::mutex mu_;
  std::atomic<bool> stop_{false}, finished_{false};
  std::atomic<std::size_t> steps_{0};
  fdtd::SParam1 liveS11_;
  std::unique_ptr<viz::PhasorAccumulator> phasors_;
  viz::Plane plane_{};
  std::vector<std::uint8_t> mask_;
  sim::RunResult result_;
  std::string runError_;
  std::filesystem::path runDir_;
  std::chrono::steady_clock::time_point runStart_;
  double runSeconds_ = 0;

  // 3D volume view (display grid, phasors at display resolution, camera)
  viz::DisplayVolume dv_;
  std::unique_ptr<viz::PhasorAccumulator> volPhasors_;
  std::vector<float> volScratch_;
  viz::Camera cam_;
  double density_ = 5, volBrightness_ = -0.3;
  bool settingsOpen_ = false, sliceControlsOpen_ = true, volControlsOpen_ = true; // collapsible panels
  // Field slice view: zoom (1 = board plus margin) and pan of the view centre, metres in grid coordinates.
  double sliceZoom_ = 1, slicePanU_ = 0, slicePanV_ = 0, sliceMPerPx_ = 0;
  bool lightSlice_ = false; // dark panel (sky blue/yellow on near-black) by default; the light panel is the
                            // published-figure look
  bool showOutline_ = true; // board outline drawn over the field slice
  std::array<double, 4> sliceCrop_{0, 0, 0, 0}; // visible window of the last slice frame: u0, u1, v0, v1 (m)
  // Layers of the animated views, each switched on its own so they can be seen alone or overlaid.
  bool showFieldSlice_ = true, showCurrentSlice_ = false, currentBottom_ = false; // slice tab
  bool showField3d_ = true, showCurrent3d_ = false;                               // 3D tab
  fdtd::SurfaceCurrentList
      currentList_; // copper cells and their two H channels (see fdtd/surface_current.hpp)
  std::unique_ptr<viz::PhasorAccumulator> currentH_; // 2 channels per cell
  std::vector<std::complex<double>> currentJ_;       // J_s phasor per cell at freq_
  double currentScale_ = 0;                          // |J_s| at the 92nd percentile, A/m
  std::uint64_t currentVersion_ = ~0ull;
  std::size_t currentFreq_ = ~std::size_t(0);
  void refreshCurrent();
  // Draws the surface current of the cells on copper plane `onlyK` (or all if npos) as coloured quads;
  // `project` maps grid coordinates (m) to the screen.
  void drawCurrent(ImDrawList* dl, const std::function<bool(double, double, double, ImVec2&)>& project,
                   bool light, std::size_t onlyK);
  bool showGeometry_ = true; // copper, vias, port and board thickness drawn over the 3D view
  // Legends are collapsible: preview, S11, slice, volume field, volume board (the preview's starts closed
  // because it sits over the drawing).
  bool legendOpen_[5] = {false, true, true, true, true};
  bool dark_ = true, themeApplied_ = false;
  // Accessibility (system preference or the user's override): see viz/a11y.hpp
  double textScale_ = 0; // 0: not applied yet
  bool reduceMotion_ = false, highContrast_ = false, opaquePanels_ = false;
  bool lightVolume_ = false; // 3D view panel, from the volume_panel setting
  double themeCheck_ = -1e9;
  FrameTimes times_;
  double sliceScale_ = 0; // field value of full colour in the slice (for the legend)
  // 3D view performance: statistics and camera fit recomputed only when the data changes, the last image
  // reused when nothing changed, and the render size adapted so a frame stays inside its time budget.
  std::atomic<std::uint64_t> phasorVersion_{0};
  std::uint64_t statsVersion_ = ~0ull;
  std::size_t statsFreq_ = ~std::size_t(0);
  double statsTime_ = -1e9, volMagScale_ = 1e-30, volRes_ = 1.0;
  viz::Camera fitCam_;
  std::vector<float> volField_, volMag_;
  std::vector<std::uint8_t> volImg_;
  std::uint32_t volImgW_ = 0, volImgH_ = 0;
  std::array<double, 20> lastKey_{};
  bool adaptive_ = true;
  std::optional<viz::Pose> viewGoal_; // glide target set by the axis triad or keys 0-6
  bool autoFit_ = true;               // camera follows the activity until the user moves it
  fdtd::Axis fieldAxis_ = fdtd::Z;
  struct VolRect {
    float x = 0, y = 0, w = 0, h = 0;
    bool dragging = false;
  } volRect_;

  // slice view
  std::size_t freq_ = 0;
  double phase_ = 0, scaleFactor_ = 1, lastFrame_ = 0;
  bool paused_ = false, jumped_ = false;
  std::string pendingTab_;
  viz::access::Tree access_;
  bool accessWanted_ = false;
  struct PendingClick {
    float x = 0, y = 0;
    int phase = 0; // 0 none, 1 press, 2 hold, 3 release
  } click_;
  struct SliceRect {
    float x = 0, y = 0, w = 0, h = 0;
  } sliceRect_;
};

} // namespace ui
