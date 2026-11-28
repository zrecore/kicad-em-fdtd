// The solver window's screens: Preview (settings, estimate, board and domain), Running (live S11, progress,
// field slice) and Done (results and where they are). One class, drawn with Dear ImGui through the CPU
// rasteriser, so it can also render to an image without a window (used to check the layout).
#pragma once
#include <atomic>
#include <chrono>
#include <complex>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "app/farm_client.hpp"
#include "app/field_recorder.hpp"
#include "app/paths.hpp"
#include "app/prepare.hpp"
#include "app/probes.hpp"
#include "app/run_store.hpp"
#include "app/selection_store.hpp"
#include "app/settings.hpp"
#include "app/sweep.hpp"
#include "board/antenna.hpp"
#include "board/feed_solve.hpp"
#include "board/refine_suggest.hpp"
#include "board/snapshot.hpp"
#include "fdtd/ntff.hpp"
#include "fdtd/surface_current.hpp"
#include "imgui.h"
#include "viz/access.hpp"
#include "viz/imgui_soft.hpp"
#include "viz/isosurface.hpp"
#include "viz/slice.hpp"
#include "viz/view_axes.hpp"
#include "viz/volume.hpp"
#include "viz/volume_renderer.hpp"
#include "viz_vk/gpu_slice.hpp"
#include "vkcore/context.hpp"

struct ImGuiContext;

struct ImDrawList;
struct ImVec2;

namespace ui {

struct Backend {
  std::string name;    // "GPU (Apple M1 Pro)" or "CPU reference"
  std::string details; // for the run summary: the device's float controls (fp32 denormals, NaN handling)
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
  // The window draws the ImGui draw data itself (on the GPU) after frame(): frame() then leaves the UI out of
  // the returned pixels. `fontsChanged` is called when the font atlas is rebuilt.
  void setGpuUi(std::function<void()> fontsChanged) {
    gpuUi_ = true;
    onFontsChanged_ = std::move(fontsChanged);
  }
  void setAdaptive(bool on) { adaptive_ = on; } // adaptive render resolution (off for still images)
  void setSearch(const std::string& text) {
    searchText_ = text;
    searchApplied_ = text;
  }
  void expandGroup(const std::string& g) { expanded_.insert(g); }
  void start(); // begin the solve (the Run button)
  // Opens a saved run (its folder under <project>/em-fdtd/results) without solving: the board, settings, S11
  // and field data it kept. False, with statusMessage() saying why, when it cannot be opened.
  bool openRun(const std::filesystem::path& dir);
  // Shows the S11 of a job fetched from the farm (a folder with s11.s1p; the fields stay on the worker) in
  // the S11 tab. False, with a message, if a run is going or the file cannot be read.
  bool openFarmResult(const std::filesystem::path& dir);
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
  // for tests and screenshots: a left click at a window position (press now, release with releaseMouse())
  void pressAt(float x, float y) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    io.AddMouseButtonEvent(0, true);
  }
  void releaseMouse() { ImGui::GetIO().AddMouseButtonEvent(0, false); }
  void setProbes(const std::string& text) { probeText_ = text; }
  void exportVolume(); // the 3D volume (and the isosurface, if shown) into the results folder
  const std::string& statusMessage() const { return message_; }
  // for screenshots and tests: start a sweep and wait for it to finish
  void sweepNow(const std::string& key, const std::string& values) {
    sweepKey_ = key;
    sweepValues_ = values;
    startSweep();
    if (sweepWorker_.joinable())
      sweepWorker_.join();
  }
  // for screenshots: layers is a comma list of field, nofield, current, nocurrent (both tabs)
  void setLayers(const std::string& layers) {
    auto has = [&](const char* w) {
      return ("," + layers + ",").find(std::string(",") + w + ",") != std::string::npos;
    };
    if (has("nofield"))
      showFieldSlice_ = showField3d_ = false;
    openModelOnce_ = has("model");
    openRefineOnce_ = has("refine");
    openRunPopup_ = has("openrun");
    showPowerSlice_ = has("power");
    showIso_ = has("iso");
    showVoxelModel_ = has("voxels");
    pinAfterRun_ = has("pin");
    s11View_ = has("smith") ? 1 : has("impedance") ? 2 : has("sweep") ? 3 : 0;
    modelScrollFrames_ = openModelOnce_ ? 4 : 0;
    refineScrollFrames_ = openRefineOnce_ ? 4 : 0;
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
  void setupViewer();
  void drawAntennaTab(float w, float h);
  std::vector<board::AntennaRule> antennaRules_;
  bool antennaRulesLoaded_ = false;
  std::string antennaRef_, antennaKey_, antennaRulesError_;
  board::AntennaReport antennaReport_;
  board::FeedReport feedReport_;
  // The field solve of the feed line (board/feed_solve.hpp) runs off the draw thread, on the CPU, once per
  // distinct line; the draw reads the result under the mutex.
  void startFeedSolve(const board::FeedReport& report, const std::string& key);
  std::thread feedWorker_;
  std::mutex feedMutex_;
  std::string feedSolveKey_;
  bool feedSolveRunning_ = false;
  board::FeedSolve feedSolve_;
  const board::AntennaRule* antennaRule_ = nullptr;
  void drawOpenRun();
  bool openRunPopup_ = false;
  std::vector<app::RunManifest> runList_;
  int runPick_ = -1;
  std::string runListMessage_;
  void drawSettings();
  void drawSetting(const std::string& key);
  void drawPreviewTab(float w, float h);
  void drawS11Tab(float w, float h);
  void drawProbesTab(float w, float h);
  void drawFarmTab(float w, float h);
  // Runs that go to the farm (docs/ux.md "Farm screen"): the Run button sends the window's snapshot and its
  // deviations to the farm instead of solving here; the result is fetched later from the Farm tab.
  void startFarmRun();
  void pollFarm();
  void ensureFarm();
  void drawProbePanel();
  void refreshIsosurface();
  void drawSweepOverlay(float w, float h);
  void drawSweepPanel();
  void startSweep();
  void drawFarFieldTab(float w, float h);
  void drawS11Plot(float w, float h);
  void drawSmithChart(float w, float h);
  void drawImpedancePlot(float w, float h);
  fdtd::SParam1 pinnedS11_; // freeze-and-compare: a run's S11 kept as a reference for the next runs
  std::string pinnedLabel_;
  bool pinAfterRun_ = false; // screenshots: pin the finished run's S11 at once (a smoke test of the overlay)
  bool showVoxelModel_ = false; // the preview shows the model as the solver sees it
  int s11View_ = 0;             // 0 dB plot, 1 Smith chart, 2 impedance and VSWR, 3 sweep overlay
  // Export, isosurface and probes (docs/PLAN.md, Next steps 11h)
  bool showIso_ = false;
  float isoLevel_ = 0.5f; // fraction of the 92nd-percentile magnitude
  std::vector<viz::Triangle> isoMesh_;
  std::uint64_t isoVersion_ = ~0ull;
  std::size_t isoFreq_ = ~std::size_t(0);
  float isoLevelBuilt_ = -1;
  std::string probeText_, probeStatus_;
  std::vector<app::ProbeSpec> probes_;
  std::unique_ptr<viz::PhasorAccumulator> probeAcc_;
  std::vector<double> probeFreqs_;
  // Sweeps (docs/PLAN.md, Next steps 11g): one run per value of a setting, the S11 curves overlaid
  std::thread sweepWorker_;
  std::atomic<bool> sweepRunning_{false};
  std::vector<app::SweepPoint> sweepPoints_; // under mu_
  std::string sweepKey_ = "mesh_preset", sweepValues_ = "coarse,medium", sweepStatus_, sweepLabelKey_;
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
  bool gpuUi_ = false; // the window draws the ImGui data on the GPU: do not rasterise it here
  std::function<void()> onFontsChanged_; // tells the GPU backend the atlas was rebuilt

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
  // the field data a run records (src/app/field_recorder.hpp): layout, accumulators, far-field results
  app::FieldRecorder rec_;
  std::atomic<bool> stop_{false}, finished_{false};
  std::atomic<std::size_t> steps_{0};
  fdtd::SParam1 liveS11_;
  std::vector<std::uint8_t> mask_;
  sim::RunResult result_;
  std::string runError_;
  std::filesystem::path runDir_;
  std::chrono::steady_clock::time_point runStart_;
  double runSeconds_ = 0;

  // 3D volume view (display grid, phasors at display resolution, camera)
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
  std::vector<std::complex<double>> currentJ_; // J_s phasor per cell at freq_
  double currentScale_ = 0;                    // |J_s| at the 92nd percentile, A/m
  std::uint64_t currentVersion_ = ~0ull;
  std::size_t currentFreq_ = ~std::size_t(0);
  void refreshCurrent();
  // Power flow in the slice plane: E and H phasors (6 channels per plane node) -> time-averaged S
  std::vector<std::array<float, 3>> poyntingS_; // per plane node, at freq_
  double poyntingScale_ = 0;                    // |S lateral| at the 95th percentile, W/m^2 per unit source
  std::uint64_t poyntingVersion_ = ~0ull;
  std::size_t poyntingFreq_ = ~std::size_t(0);
  bool showPowerSlice_ = false;
  double animCyclesPerSecond_ = 0.5; // phase animation speed
  void refreshPoynting();
  // Far field (docs/PLAN.md, Next steps 11f): a Huygens box around the board, accumulated at a few
  // frequencies of the sweep, transformed after the run.
  using FarResult = app::FarResult;
  int farFreq_ = 0;
  bool farRealised_ = true; // plot the realised gain (else the directivity)
  void animationControl();
  std::vector<viz::Rgb> volumeLut() const;
  // Draws the surface current of the cells on copper plane `onlyK` (or all if npos) as coloured quads;
  // `project` maps grid coordinates (m) to the screen.
  void drawCurrent(ImDrawList* dl, const std::function<bool(double, double, double, ImVec2&)>& project,
                   bool light, std::size_t onlyK);
  // Model selection (docs/PLAN.md, Next steps 12): picked nets, the guessed ground nets, and the text the
  // user typed for each two-terminal part's R, L and C.
  std::set<std::string> pickedNets_, groundNets_, extraGroundNets_;
  bool groundGuessed_ = false, openModelOnce_ = false;
  int modelScrollFrames_ = 0;
  std::map<std::string, std::array<std::string, 3>> partText_;
  void drawModelSelection();
  // Suggested fine-cell boxes for narrow gaps near a port (board/refine_suggest.hpp), the user's choice for
  // each (saved in the sidecar file), and the boxes the user typed on the command line.
  std::vector<board::RefineSuggestion> refineSuggestions_;
  std::map<std::string, app::RefineChoice> refineChoices_;
  std::map<std::string, std::string> refineText_;
  std::vector<board::RefineBox> typedRefine_;
  std::string refineKey_;
  bool typedRefineKept_ = false, openRefineOnce_ = false;
  int refineScrollFrames_ = 0;
  void drawRefinement();
  void ensureRefinement();
  void applyRefinement();
  void applyNets();
  void ensureGround();
  void loadSelectionOnce();
  void saveSelection();
  bool selectionLoaded_ = false;
  bool showGeometry_ = true; // copper, vias, port and board thickness drawn over the 3D view
  // Legends are collapsible: preview, S11, slice, volume field, volume board (the preview's starts closed
  // because it sits over the drawing).
  bool legendOpen_[5] = {false, true, true, true, true};
  bool dark_ = true, themeApplied_ = false;
  // Accessibility (system preference or the user's override): see viz/a11y.hpp
  double textScale_ = 0; // 0: not applied yet
  std::string fontMode_; // "system" or "builtin": the font the atlas was built with
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
  // The GPU volume renderer: its own Vulkan context (the solver thread submits to the solver's queue, and a
  // queue must not be used from two threads at once), made on first use; null means the CPU marcher.
  std::unique_ptr<vkcore::Context> renderCtx_;
  std::unique_ptr<viz::VolumeRenderer> volRenderer_;
  bool renderTried_ = false;
  std::unique_ptr<viz::GpuSliceRenderer> sliceRenderer_; // the same context; null means the CPU slice
  bool sliceTried_ = false;
  vkcore::Context* renderContext(); // made on first use; null on the CPU backend, in stills, or on failure
  std::uint64_t renderVersion_ = ~0ull;
  std::size_t renderFreq_ = ~std::size_t{0};
  std::vector<std::uint8_t> volImg_;
  std::uint32_t volImgW_ = 0, volImgH_ = 0;
  std::array<double, 20> lastKey_{};
  bool adaptive_ = true;
  std::optional<viz::Pose> viewGoal_; // glide target set by the axis triad or keys 0-6
  bool autoFit_ = true;               // camera follows the activity until the user moves it
  struct VolRect {
    float x = 0, y = 0, w = 0, h = 0;
    bool dragging = false;
  } volRect_;

  // slice view
  std::size_t freq_ = 0;
  double phase_ = 0, scaleFactor_ = 1, lastFrame_ = 0;
  bool paused_ = false, jumped_ = false;
  std::string pendingTab_;
  // Farm tab (docs/ux.md "Farm screen"): created when the tab is first shown, so a window that never shows it
  // runs nothing.
  std::unique_ptr<farm::Client> farm_;
  std::string farmPendingEnable_, farmPendingRevoke_;
  bool farmLocalConfirm_ = false; // the two-step confirmation for offering this computer's GPU
  // The Agent section: which of Stop or Restart waits for "yes, cancel the running job", and the two
  // expanders.
  std::string farmAgentConfirm_;
  bool farmAgentChecksOpen_ = false, farmAgentLogOpen_ = false;
  char farmName_[64] = "";
  char farmListen_[64] = "";
  bool farmInviteForClient_ = false;
  std::string runOn_ =
      "gpu"; // "gpu" or "farm", remembered in the farm folder (window.json), never the sidecar
  bool runOnLoaded_ = false, clientOn_ = false;
  double clientCheckedAt_ = -100;
  bool farmSubmitPending_ = false, farmResult_ = false;
  std::string farmFetching_;                       // id of the job being fetched
  std::map<std::string, std::string> farmFetched_; // job id -> the folder its results are in
  std::string farmNote_;                           // the last farm run or fetch, in words
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
