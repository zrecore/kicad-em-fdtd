#include "ui.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <numbers>
#include <sstream>

#include "app/phasor_feed.hpp"
#include "app/results.hpp"
#include "app/selection_store.hpp"
#include "board/netwalk.hpp"
#include "fdtd/impedance.hpp"
#include "fdtd/sparam.hpp"
#include "imgui.h"
#include "viz/a11y.hpp"
#include "viz/colormap.hpp"
#include "viz/poynting.hpp"
#include "viz/theme.hpp"
#include "viz/triangulate.hpp"
#include "viz/view_axes.hpp"
#include "viz/vtk_export.hpp"
#include "viz_vk/gpu_volume.hpp"

namespace fs = std::filesystem;

namespace ui {
namespace {

// Layout sizes are written for the default text size; the font is 16 px at 100 %, so this scales them with
// it.
float px(float v) {
  return v * ImGui::GetFontSize() / 16.0f;
}

// ---- accessibility tree
// ------------------------------------------------------------------------------------ Each widget records
// what it is for a screen reader straight after it is drawn. The tree is only built while an assistive
// technology is attached (gTree is null otherwise), so this costs nothing for other users.
viz::access::Tree* gTree = nullptr;

std::string visibleName(const char* label) { // "Run##id" is announced as "Run"
  std::string s = label;
  const auto hash = s.find("##");
  return hash == std::string::npos ? s : s.substr(0, hash);
}

// Records the last item drawn. `id` is ImGui's, so it stays the same while the control exists.
void note(viz::access::Role role, std::string name, std::string value = {}, std::string help = {},
          std::string key = {}) {
  if (!gTree)
    return;
  viz::access::Node n;
  n.id = (1ull << 32) | ImGui::GetItemID();
  n.role = role;
  n.name = std::move(name);
  n.value = std::move(value);
  n.help = std::move(help);
  n.key = std::move(key);
  const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  n.x = a.x;
  n.y = a.y;
  n.w = b.x - a.x;
  n.h = b.y - a.y;
  n.focused = ImGui::IsItemFocused() || ImGui::IsItemActive();
  gTree->add(std::move(n));
}

// Text that is not a control (a status line, a result): read in order, no id of its own.
void noteText(std::string text, viz::access::Role role = viz::access::Role::Label) {
  if (!gTree || text.empty())
    return;
  viz::access::Node n;
  n.role = role;
  n.name = std::move(text);
  n.live = role == viz::access::Role::Status;
  const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  n.x = a.x;
  n.y = a.y;
  n.w = b.x - a.x;
  n.h = b.y - a.y;
  gTree->add(std::move(n));
}

std::string strf(const char* format, ...) __attribute__((format(printf, 1, 2)));
std::string strf(const char* format, ...) {
  char buf[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buf, sizeof buf, format, args);
  va_end(args);
  return buf;
}

// Wrapped text, also read out.
void wrapped(const std::string& text) {
  ImGui::TextWrapped("%s", text.c_str());
  noteText(text);
}

void coloured(ImVec4 colour, const std::string& text) {
  ImGui::TextColored(colour, "%s", text.c_str());
  noteText(text);
}

bool button(const char* label, ImVec2 size = {0, 0}) {
  const bool hit = ImGui::Button(label, size);
  note(viz::access::Role::Button, visibleName(label));
  return hit;
}

bool smallButton(const char* label, const char* name = nullptr) {
  const bool hit = ImGui::SmallButton(label);
  note(viz::access::Role::Button, name ? name : visibleName(label));
  return hit;
}

bool checkbox(const char* label, bool* value) {
  const bool hit = ImGui::Checkbox(label, value);
  note(viz::access::Role::CheckBox, visibleName(label));
  if (gTree) {
    gTree->mutableBack().checkable = true;
    gTree->mutableBack().checked = *value;
  }
  return hit;
}

bool sliderInt(const char* name, const char* id, int* v, int lo, int hi, const char* fmt) {
  const bool hit = ImGui::SliderInt(id, v, lo, hi, fmt);
  note(viz::access::Role::Slider, name, std::to_string(*v));
  return hit;
}

bool sliderFloat(const char* name, const char* id, float* v, float lo, float hi, const char* fmt) {
  const bool hit = ImGui::SliderFloat(id, v, lo, hi, fmt);
  char text[32];
  std::snprintf(text, sizeof text, "%.1f", static_cast<double>(*v));
  note(viz::access::Role::Slider, name, text);
  return hit;
}

bool selectable(const std::string& text, bool selected) {
  const bool hit = ImGui::Selectable(text.c_str(), selected);
  note(viz::access::Role::Button, text, selected ? "current choice" : "");
  return hit;
}

bool tabItem(const char* label, bool* open, ImGuiTabItemFlags flags) {
  const bool selected = ImGui::BeginTabItem(label, open, flags);
  note(viz::access::Role::Tab, label);
  if (gTree)
    gTree->mutableBack().selected = selected;
  return selected;
}

// Colours the window draws itself (ImGui's own widgets follow StyleColorsDark/Light). One palette per theme.
struct Palette {
  ImU32 text, dim, accent, warn, bad, good;
  ImU32 plotBg, gridLine, legendBg, legendBorder, legendText, legendDim, curve;
  ImU32 geoTop, geoOther, geoOutline, geoVia, geoPort, geoPortText, geoBox, pml; // board drawing colours
  std::uint8_t clear[3]; // frame background behind everything
};

Palette darkPalette() {
  return {IM_COL32(230, 230, 235, 255), IM_COL32(150, 150, 160, 255),
          IM_COL32(90, 160, 255, 255),  IM_COL32(255, 190, 70, 255),
          IM_COL32(255, 110, 100, 255), IM_COL32(110, 220, 150, 255),
          IM_COL32(22, 22, 26, 255),    IM_COL32(60, 60, 68, 255),
          IM_COL32(12, 12, 18, 215),    IM_COL32(80, 80, 95, 255),
          IM_COL32(230, 230, 238, 255), IM_COL32(200, 200, 212, 255),
          IM_COL32(66, 135, 245, 255),  IM_COL32(255, 175, 70, 255),
          IM_COL32(110, 160, 230, 255), IM_COL32(200, 200, 210, 255),
          IM_COL32(230, 230, 110, 255), IM_COL32(255, 90, 90, 255),
          IM_COL32(255, 150, 150, 255), IM_COL32(120, 120, 130, 255),
          IM_COL32(110, 75, 75, 255),   {28, 28, 32}};
}

Palette lightPalette() {
  return {IM_COL32(30, 30, 36, 255),    IM_COL32(95, 95, 108, 255),
          IM_COL32(20, 90, 200, 255),   IM_COL32(170, 95, 0, 255),
          IM_COL32(190, 40, 40, 255),   IM_COL32(20, 130, 70, 255),
          IM_COL32(250, 250, 252, 255), IM_COL32(215, 215, 222, 255),
          IM_COL32(255, 255, 255, 230), IM_COL32(170, 170, 185, 255),
          IM_COL32(30, 30, 36, 255),    IM_COL32(70, 70, 82, 255),
          IM_COL32(20, 90, 200, 255),   IM_COL32(200, 115, 10, 255),
          IM_COL32(50, 95, 185, 255),   IM_COL32(70, 70, 85, 255),
          IM_COL32(150, 130, 0, 255),   IM_COL32(210, 40, 40, 255),
          IM_COL32(170, 30, 30, 255),   IM_COL32(140, 140, 152, 255),
          IM_COL32(205, 150, 150, 255), {240, 240, 244}};
}

ImU32 alpha(ImU32 c, int a) {
  return (c & 0x00FFFFFFu) | (static_cast<ImU32>(a) << 24);
}

// Increase Contrast: pure text, strong accents (each at least 7:1 on its background), opaque panels with a
// full-strength border.
Palette highContrast(Palette p, bool dark) {
  p.text = p.legendText = dark ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 255);
  p.dim = p.legendDim = dark ? IM_COL32(225, 225, 230, 255) : IM_COL32(35, 35, 40, 255);
  p.accent = dark ? IM_COL32(130, 195, 255, 255) : IM_COL32(0, 50, 150, 255);
  p.warn = dark ? IM_COL32(255, 210, 80, 255) : IM_COL32(120, 60, 0, 255);
  p.bad = dark ? IM_COL32(255, 140, 130, 255) : IM_COL32(160, 0, 0, 255);
  p.good = dark ? IM_COL32(130, 240, 160, 255) : IM_COL32(0, 90, 40, 255);
  p.legendBorder = p.text;
  p.gridLine = dark ? IM_COL32(130, 130, 140, 255) : IM_COL32(110, 110, 120, 255);
  p.curve = p.accent;
  return p;
}

// Panels drawn over images become opaque (Reduce Transparency, Increase Contrast).
Palette opaque(Palette p) {
  p.legendBg |= 0xFF000000u;
  return p;
}

Palette pal = darkPalette(); // the active palette, set once per frame from the theme

// Board-drawing colours of the panel being drawn: when the user picks a panel (light or dark) that differs
// from the application theme, the overlays on it use that panel's colours. Restores the active palette on
// scope exit.
struct PanelColours {
  Palette saved;
  PanelColours(bool lightPanel, bool darkApp) : saved(pal) {
    if (lightPanel == darkApp) {
      const Palette alt = lightPanel ? lightPalette() : darkPalette();
      pal.geoTop = alt.geoTop;
      pal.geoOther = alt.geoOther;
      pal.geoOutline = alt.geoOutline;
      pal.geoVia = alt.geoVia;
      pal.geoPort = alt.geoPort;
      pal.geoPortText = alt.geoPortText;
      pal.geoBox = alt.geoBox;
    }
  }
  ~PanelColours() { pal = saved; }
};

std::string fmtTime(double s) {
  char b[32];
  if (s < 90)
    std::snprintf(b, sizeof b, "%.0f s", s);
  else if (s < 5400)
    std::snprintf(b, sizeof b, "%.0f min", s / 60);
  else
    std::snprintf(b, sizeof b, "%.1f h", s / 3600);
  return b;
}

std::string fmtBytes(double b) {
  char buf[32];
  std::snprintf(buf, sizeof buf, b < 1e9 ? "%.0f MB" : "%.1f GB", b < 1e9 ? b * 1e-6 : b * 1e-9);
  return buf;
}

// "2.41 GHz" style tick labels.
std::string ghz(double hz) {
  char b[32];
  std::snprintf(b, sizeof b, "%.3g", hz * 1e-9);
  return b;
}

struct LegendItem {
  enum Kind { Swatch, Line, Dashed, Dot, Ring } kind;
  ImU32 colour;
  std::string label;
};

// A small translucent box with colour swatches and what they mean (so a reader never has to guess a colour).
// Returns the box height. Text is drawn in the UI font at the current scale.
// Click the header to collapse it to a one-line chip (and again to expand). `open` keeps the state.
void legendHeaderButton(const char* id, ImVec2 topLeft, ImVec2 size, bool& open) {
  const ImVec2 save = ImGui::GetCursorScreenPos(); // do not disturb the layout of what follows
  ImGui::SetCursorScreenPos(topLeft);
  ImGui::PushID(id);
  if (ImGui::InvisibleButton("##legend", size))
    open = !open;
  ImGui::PopID();
  ImGui::SetCursorScreenPos(save);
}

// Size of the box drawLegend draws, so a caller can place it inside its panel before drawing.
ImVec2 legendBox(const char* title, const std::vector<LegendItem>& items, bool open) {
  const float row = ImGui::GetTextLineHeightWithSpacing(), pad = px(8);
  const std::string head = std::string(title) + (open ? "  [-]" : "  [+]");
  float width = ImGui::CalcTextSize(head.c_str()).x;
  if (!open)
    return {width + pad * 2, row + pad};
  for (const auto& it : items)
    width = std::max(width, 26 + ImGui::CalcTextSize(it.label.c_str()).x);
  return {width + pad * 2, pad * 2 + row * static_cast<float>(items.size() + 1)};
}

float drawLegend(ImDrawList* dl, ImVec2 topLeft, const char* title, const std::vector<LegendItem>& items,
                 const char* id, bool& open) {
  const float row = ImGui::GetTextLineHeightWithSpacing(), pad = px(8);
  const std::string head = std::string(title) + (open ? "  [-]" : "  [+]");
  if (!open) { // a chip: just the title
    const float w = ImGui::CalcTextSize(head.c_str()).x + pad * 2, h = row + pad;
    dl->AddRectFilled(topLeft, {topLeft.x + w, topLeft.y + h}, pal.legendBg, 5.0f);
    dl->AddRect(topLeft, {topLeft.x + w, topLeft.y + h}, pal.legendBorder, 5.0f);
    dl->AddText({topLeft.x + pad, topLeft.y + pad * 0.5f}, pal.legendText, head.c_str());
    legendHeaderButton(id, topLeft, {w, h}, open);
    return h;
  }
  float width = ImGui::CalcTextSize(head.c_str()).x;
  for (const auto& it : items)
    width = std::max(width, 26 + ImGui::CalcTextSize(it.label.c_str()).x);
  const float height = pad * 2 + row * static_cast<float>(items.size() + 1);
  dl->AddRectFilled(topLeft, {topLeft.x + width + pad * 2, topLeft.y + height}, pal.legendBg, 5.0f);
  dl->AddRect(topLeft, {topLeft.x + width + pad * 2, topLeft.y + height}, pal.legendBorder, 5.0f);
  float y = topLeft.y + pad;
  dl->AddText({topLeft.x + pad, y}, pal.legendText, head.c_str());
  legendHeaderButton(id, topLeft, {width + pad * 2, row + pad}, open);
  y += row;
  for (const auto& it : items) {
    const float cy = y + row * 0.45f, x0 = topLeft.x + pad;
    switch (it.kind) {
    case LegendItem::Swatch:
      dl->AddRectFilled({x0, cy - 5}, {x0 + 16, cy + 5}, it.colour, 2.0f);
      break;
    case LegendItem::Line:
      dl->AddLine({x0, cy}, {x0 + 16, cy}, it.colour, 2.5f);
      break;
    case LegendItem::Dashed:
      dl->AddLine({x0, cy}, {x0 + 6, cy}, it.colour, 2.0f);
      dl->AddLine({x0 + 10, cy}, {x0 + 16, cy}, it.colour, 2.0f);
      break;
    case LegendItem::Dot:
      dl->AddCircleFilled({x0 + 8, cy}, 4.5f, it.colour, 12);
      break;
    case LegendItem::Ring:
      dl->AddCircle({x0 + 8, cy}, 4.5f, it.colour, 12, 1.5f);
      break;
    }
    dl->AddText({x0 + 24, y}, pal.legendDim, it.label.c_str());
    y += row;
  }
  return height;
}

// Horizontal colour bar from `map(0)` to `map(1)` with captions under the left end, middle and right end.
void drawColourBar(ImDrawList* dl, ImVec2 pos, float width, const char* titleText,
                   const std::function<viz::Rgb(float)>& map, const char* left, const char* middle,
                   const char* right, const char* id, bool& open) {
  const float barH = px(14), pad = px(8), row = ImGui::GetTextLineHeightWithSpacing();
  const std::string head = std::string(titleText) + (open ? "  [-]" : "  [+]");
  const char* title = head.c_str();
  if (!open) { // a chip: just the title
    const float w = ImGui::CalcTextSize(title).x + pad * 2, h = row + pad;
    dl->AddRectFilled(pos, {pos.x + w, pos.y + h}, pal.legendBg, 5.0f);
    dl->AddRect(pos, {pos.x + w, pos.y + h}, pal.legendBorder, 5.0f);
    dl->AddText({pos.x + pad, pos.y + pad * 0.5f}, pal.legendText, title);
    legendHeaderButton(id, pos, {w, h}, open);
    return;
  }
  width = std::max(width, ImGui::CalcTextSize(title).x); // never let the title spill out of the box
  const float boxW = width + pad * 2, boxH = pad * 2 + row * 2 + barH + 2;
  dl->AddRectFilled(pos, {pos.x + boxW, pos.y + boxH}, pal.legendBg, 5.0f);
  dl->AddRect(pos, {pos.x + boxW, pos.y + boxH}, pal.legendBorder, 5.0f);
  dl->AddText({pos.x + pad, pos.y + pad}, pal.legendText, title);
  legendHeaderButton(id, pos, {boxW, row + pad}, open);
  const float y = pos.y + pad + row;
  constexpr int kSteps = 96;
  for (int i = 0; i < kSteps; ++i) {
    const viz::Rgb c = map((static_cast<float>(i) + 0.5f) / kSteps);
    dl->AddRectFilled({pos.x + pad + width * static_cast<float>(i) / kSteps, y},
                      {pos.x + pad + width * static_cast<float>(i + 1) / kSteps + 0.5f, y + barH},
                      IM_COL32(c.r, c.g, c.b, 255));
  }
  dl->AddRect({pos.x + pad, y}, {pos.x + pad + width, y + barH}, IM_COL32(120, 120, 135, 255));
  dl->AddLine({pos.x + pad + width / 2, y}, {pos.x + pad + width / 2, y + barH + 3}, pal.legendText, 1.0f);
  const float ty = y + barH + 3;
  dl->AddText({pos.x + pad, ty}, pal.legendDim, left);
  dl->AddText({pos.x + pad + width / 2 - ImGui::CalcTextSize(middle).x / 2, ty}, pal.legendDim, middle);
  dl->AddText({pos.x + pad + width - ImGui::CalcTextSize(right).x, ty}, pal.legendDim, right);
}

} // namespace

Ui::Ui(Env env) : env_(std::move(env)), snapshot_(env_.snapshot) {
  ctx_ = ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  ImFontConfig cfg;
  cfg.SizePixels = 16;
  cfg.RasterizerDensity = env_.pixelScale; // crisp text on a Retina display
  io.Fonts->AddFontDefault(&cfg);
  ImGui::StyleColorsDark();
  ImGuiStyle& st = ImGui::GetStyle();
  st.WindowRounding = 0;
  st.FrameRounding = 3;
  st.ItemSpacing = {8, 6};
  soft_ = std::make_unique<viz::SoftImGui>();
}

Ui::~Ui() {
  stop_ = true;
  if (worker_.joinable())
    worker_.join();
  if (sweepWorker_.joinable())
    sweepWorker_.join();
  ImGui::DestroyContext(ctx_);
}

bool Ui::setSnapshot(board::Snapshot s) {
  if (screen_ == Screen::Running)
    return false;
  if (worker_.joinable())
    worker_.join();
  snapshot_ = std::move(s);
  groundGuessed_ = false; // the nets may have changed: guess again, keep the user's picks
  phasors_.reset();
  volPhasors_.reset();
  volRenderer_.reset(); // sized for the old display volume
  sliceRenderer_.reset();
  sliceTried_ = false;
  renderTried_ = false;
  renderVersion_ = ~0ull;
  currentH_.reset();
  poyntingH_.reset();
  ntffAcc_.reset();
  farResults_.clear();
  probeAcc_.reset();
  currentList_ = {};
  volImg_.clear();
  statsVersion_ = ~0ull;
  screen_ = Screen::Preview;
  dirty_ = true;
  lastEdit_ = -1;
  message_ = "Board updated from KiCAD.";
  return true;
}

void Ui::setHeadless(std::uint32_t w, std::uint32_t h) {
  headless_ = true;
  ImGui::GetIO().DisplaySize = {static_cast<float>(w), static_cast<float>(h)};
}

std::string Ui::blockedReason() const {
  if (!prepared_)
    return prepareError_.empty() ? "preparing the model..." : prepareError_;
  if (prepared_->model.ports.empty())
    return "No port in the design: select the pad that feeds your antenna in KiCAD and start again.";
  if (estimate_.exceedsMemory)
    return "The model would not fit in memory: choose a coarser mesh quality or reduce the free space.";
  return "";
}

void Ui::refreshPrepared(double now) {
  if (!dirty_ || now - lastEdit_ < 0.3 || screen_ != Screen::Preview)
    return;
  dirty_ = false;
  try {
    loadSelectionOnce();
    prepared_ = std::make_unique<app::Prepared>(app::prepare(snapshot_, env_.settings, env_.selection));
    prepareError_.clear();
    backend_ = env_.chooseBackend(prepared_->plan.backend);
    estimate_ = app::estimate(*prepared_, backend_.gpu, backend_.deviceBytes);
  } catch (const std::exception& e) {
    prepared_.reset();
    prepareError_ = e.what();
  }
}

void Ui::start() {
  if (screen_ != Screen::Preview)
    return;
  lastEdit_ = -1;
  dirty_ = true;
  refreshPrepared(now_ + 1);
  if (!blockedReason().empty())
    return;
  // Animation: Ez on a plane mid-way between the outer copper layers, at 25 frequencies across the sweep.
  const fdtd::Grid& g = prepared_->model.grid;
  std::size_t kTop = 0, kBottom = g.nodes(fdtd::Z);
  for (std::size_t i = 0; i < snapshot_.stackup.size(); ++i)
    if (snapshot_.stackup[i].copper) {
      kTop = std::max(kTop, prepared_->model.copperPlaneNode[i]);
      kBottom = std::min(kBottom, prepared_->model.copperPlaneNode[i]);
    }
  plane_ = viz::makePlane(g, fdtd::Z, (kTop + kBottom) / 2);
  const auto& sweep = prepared_->plan.sweepHz;
  std::vector<double> freqs(std::max<std::size_t>(2, prepared_->plan.animationFrequencies));
  for (std::size_t i = 0; i < freqs.size(); ++i)
    freqs[i] = sweep[0] + (sweep[1] - sweep[0]) * static_cast<double>(i) / (freqs.size() - 1);
  phasors_ = std::make_unique<viz::PhasorAccumulator>(freqs, plane_.nu * plane_.nv);
  fieldAxis_ = prepared_->plan.fieldComponent == "ex"   ? fdtd::X
               : prepared_->plan.fieldComponent == "ey" ? fdtd::Y
                                                        : fdtd::Z;
  {
    // the volume covers the board and a margin around it: 30 % sideways, and as far up and down
    const auto& m = prepared_->model;
    double bx0 = 1e9, bx1 = -1e9, by0 = 1e9, by1 = -1e9;
    for (const auto& pt : snapshot_.outline) {
      bx0 = std::min(bx0, pt.x - m.originX);
      bx1 = std::max(bx1, pt.x - m.originX);
      by0 = std::min(by0, pt.y - m.originY);
      by1 = std::max(by1, pt.y - m.originY);
    }
    const double side = std::max(bx1 - bx0, by1 - by0), zc = g.edge(fdtd::Z, (kTop + kBottom) / 2);
    viz::VolumeBox box;
    box.lo = {std::max(0.0, bx0 - 0.3 * side), std::max(0.0, by0 - 0.3 * side),
              std::max(0.0, zc - 0.4 * side)};
    box.hi = {std::min(g.length(fdtd::X), bx1 + 0.3 * side), std::min(g.length(fdtd::Y), by1 + 0.3 * side),
              std::min(g.length(fdtd::Z), zc + 0.4 * side)};
    dv_ = viz::makeDisplayVolume(g, prepared_->plan.volumeResolution, box);
    volPhasors_ = std::make_unique<viz::PhasorAccumulator>(freqs, dv_.voxels());
  }
  currentList_ = fdtd::surfaceCurrentList(g, prepared_->model.pecEdges);
  currentH_ = currentList_.channels.empty()
                  ? nullptr
                  : std::make_unique<viz::PhasorAccumulator>(freqs, currentList_.channels.size());
  currentJ_.clear();
  currentVersion_ = ~0ull;
  poyntingH_ = std::make_unique<viz::PhasorAccumulator>(freqs, 6 * plane_.nu * plane_.nv);
  // Far field: a box in free space inside the absorbing layer, at up to five frequencies of the sweep
  ntffAcc_.reset();
  farResults_.clear();
  ntffNote_.clear();
  {
    const auto& m = prepared_->model;
    const std::size_t pml = prepared_->plan.pmlCells + 2;
    std::array<std::size_t, 3> lo{}, hi{};
    bool ok = true;
    for (int a = 0; a < 3; ++a) {
      lo[static_cast<std::size_t>(a)] = pml;
      hi[static_cast<std::size_t>(a)] = g.nodes(static_cast<fdtd::Axis>(a)) - 1 - pml;
      ok = ok && hi[static_cast<std::size_t>(a)] > lo[static_cast<std::size_t>(a)] + 4;
    }
    // the box must clear the board and the dielectric: laterally the outline, vertically the copper planes
    double bx0 = 1e9, bx1 = -1e9, by0 = 1e9, by1 = -1e9;
    for (const auto& pt : snapshot_.outline) {
      bx0 = std::min(bx0, pt.x - m.originX);
      bx1 = std::max(bx1, pt.x - m.originX);
      by0 = std::min(by0, pt.y - m.originY);
      by1 = std::max(by1, pt.y - m.originY);
    }
    if (ok)
      ok = g.edge(fdtd::X, lo[0]) < bx0 && g.edge(fdtd::X, hi[0]) > bx1 && g.edge(fdtd::Y, lo[1]) < by0 &&
           g.edge(fdtd::Y, hi[1]) > by1 && g.edge(fdtd::Z, lo[2]) < g.edge(fdtd::Z, kBottom) &&
           g.edge(fdtd::Z, hi[2]) > g.edge(fdtd::Z, kTop);
    if (!ok) {
      ntffNote_ =
          "No far field: the board fills the space inside the absorbing layer. Increase the air margin "
          "(Settings) to get one.";
    } else {
      ntffPlan_ = fdtd::makeNtffPlan(g, lo, hi);
      const std::size_t count = std::min<std::size_t>(5, freqs.size());
      ntffFreqs_.clear();
      for (std::size_t i = 0; i < count; ++i)
        ntffFreqs_.push_back(freqs[count == 1 ? 0 : i * (freqs.size() - 1) / (count - 1)]);
      ntffAcc_ = std::make_unique<viz::PhasorAccumulator>(ntffFreqs_, ntffPlan_.channels.size());
    }
  }
  poyntingS_.clear();
  poyntingVersion_ = ~0ull;
  // Probes: E at the chosen points, as a spectrum over the sweep's frequencies
  probeAcc_.reset();
  probeFreqs_.clear();
  try {
    probes_ = app::parseProbes(probeText_);
    probeStatus_.clear();
  } catch (const std::exception& e) {
    probes_.clear();
    probeStatus_ = e.what();
  }
  if (!probes_.empty()) {
    const std::size_t points =
        std::min<std::size_t>(101, std::max<std::size_t>(11, prepared_->options.points));
    for (std::size_t i = 0; i < points; ++i)
      probeFreqs_.push_back(sweep[0] +
                            (sweep[1] - sweep[0]) * static_cast<double>(i) / static_cast<double>(points - 1));
    probeAcc_ = std::make_unique<viz::PhasorAccumulator>(probeFreqs_, probes_.size());
  }
  freq_ = freqs.size() / 2;
  mask_.assign(plane_.nu * plane_.nv, 0);
  const std::size_t plate = g.nodes(fdtd::X) * g.nodes(fdtd::Y);
  for (const auto& e : prepared_->model.pecEdges)
    if (e.axis != fdtd::Z && e.node / plate == kTop)
      mask_[e.node % plate] = 1;
  const auto filled = mask_; // outline only, so the field under the copper stays visible
  for (std::size_t j = 1; j + 1 < plane_.nv; ++j)
    for (std::size_t i = 1; i + 1 < plane_.nu; ++i) {
      const std::size_t n = i + plane_.nu * j;
      if (filled[n] && filled[n - 1] && filled[n + 1] && filled[n - plane_.nu] && filled[n + plane_.nu])
        mask_[n] = 0;
    }
  runDir_ = env_.paths.newRunDir(app::timestamp());
  stop_ = false;
  finished_ = false;
  steps_ = 0;
  liveS11_ = {};
  runError_.clear();
  jumped_ = false;
  runStart_ = std::chrono::steady_clock::now();
  screen_ = Screen::Running;
  pendingTab_ = "S11 plot";
  worker_ = std::thread([this] { runWorker(); });
}

void Ui::runWorker() {
  try {
    sim::RunOptions opt = prepared_->options;
    const fdtd::Grid& g = prepared_->model.grid;
    const double impedance = prepared_->plan.portImpedanceOhm;
    // Slice and volume accumulate on the backend (one phasor list each); the viewer's copies refresh about
    // once a second and at the end.
    auto frequenciesOf = [](const viz::PhasorAccumulator& a) {
      std::vector<double> f(a.frequencies());
      for (std::size_t i = 0; i < f.size(); ++i)
        f[i] = a.frequencyHz(i);
      return f;
    };
    std::vector<fdtd::PhasorChannel> sliceChannels, volumeChannels;
    for (std::size_t b = 0; b < plane_.nv; ++b)
      for (std::size_t a = 0; a < plane_.nu; ++a) {
        std::array<std::size_t, 3> idx{};
        idx[plane_.normal] = plane_.index;
        idx[plane_.u] = a;
        idx[plane_.v] = b;
        sliceChannels.push_back(
            {fdtd::Kind::E, fieldAxis_, static_cast<std::uint32_t>(g.nodeIndex(idx[0], idx[1], idx[2]))});
      }
    for (std::size_t k = 0; k < dv_.dims[2]; ++k)
      for (std::size_t j = 0; j < dv_.dims[1]; ++j)
        for (std::size_t i = 0; i < dv_.dims[0]; ++i)
          volumeChannels.push_back({fdtd::Kind::E, fieldAxis_,
                                    static_cast<std::uint32_t>(
                                        g.nodeIndex(dv_.nodeOf[0][i], dv_.nodeOf[1][j], dv_.nodeOf[2][k]))});
    app::PhasorFeed sliceFeed(*phasors_, std::move(sliceChannels), frequenciesOf(*phasors_));
    app::PhasorFeed volumeFeed(*volPhasors_, std::move(volumeChannels), frequenciesOf(*volPhasors_));
    std::vector<fdtd::PhasorChannel> powerChannels; // E and H, all three components, at every slice node
    for (std::size_t b = 0; b < plane_.nv; ++b)
      for (std::size_t a = 0; a < plane_.nu; ++a) {
        std::array<std::size_t, 3> idx{};
        idx[plane_.normal] = plane_.index;
        idx[plane_.u] = a;
        idx[plane_.v] = b;
        const auto node = static_cast<std::uint32_t>(g.nodeIndex(idx[0], idx[1], idx[2]));
        for (fdtd::Kind kind : {fdtd::Kind::E, fdtd::Kind::H})
          for (fdtd::Axis ax : {fdtd::X, fdtd::Y, fdtd::Z})
            powerChannels.push_back({kind, ax, node});
      }
    app::PhasorFeed powerFeed(*poyntingH_, std::move(powerChannels), frequenciesOf(*poyntingH_));
    std::unique_ptr<app::PhasorFeed> probeFeed;
    if (probeAcc_) {
      std::vector<fdtd::PhasorChannel> pch;
      const auto& m = prepared_->model;
      std::size_t kBottomNode = g.nodes(fdtd::Z);
      for (std::size_t i = 0; i < snapshot_.stackup.size(); ++i)
        if (snapshot_.stackup[i].copper)
          kBottomNode = std::min(kBottomNode, m.copperPlaneNode[i]);
      for (const auto& p : probes_)
        pch.push_back({fdtd::Kind::E, fieldAxis_,
                       app::probeNode(g, p, m.originX, m.originY, g.edge(fdtd::Z, kBottomNode))});
      probeFeed = std::make_unique<app::PhasorFeed>(*probeAcc_, std::move(pch), probeFreqs_);
    }
    std::unique_ptr<app::PhasorFeed> farFeed;
    if (ntffAcc_)
      farFeed = std::make_unique<app::PhasorFeed>(*ntffAcc_, ntffPlan_.channels, ntffFreqs_);
    std::unique_ptr<app::PhasorFeed> currentFeed;
    if (currentH_)
      currentFeed =
          std::make_unique<app::PhasorFeed>(*currentH_, currentList_.channels, frequenciesOf(*currentH_));
    opt.onSample = [&](sim::Engine& eng, double, double dtSample) {
      const bool a = sliceFeed.sample(eng, dtSample, mu_);
      const bool b = volumeFeed.sample(eng, dtSample, mu_);
      const bool c = currentFeed && currentFeed->sample(eng, dtSample, mu_);
      const bool d = powerFeed.sample(eng, dtSample, mu_);
      const bool e2 = farFeed && farFeed->sample(eng, dtSample, mu_, 4.0);
      const bool e3 = probeFeed && probeFeed->sample(eng, dtSample, mu_);
      if (a || b || c || d || e2 || e3)
        ++phasorVersion_;
    };
    opt.onFinish = [&](sim::Engine& eng) {
      sliceFeed.sync(eng, mu_);
      volumeFeed.sync(eng, mu_);
      if (currentFeed)
        currentFeed->sync(eng, mu_);
      powerFeed.sync(eng, mu_);
      if (farFeed)
        farFeed->sync(eng, mu_);
      if (probeFeed)
        probeFeed->sync(eng, mu_);
      ++phasorVersion_;
    };
    double lastLive = -10;
    opt.onProgress = [&](const sim::RunResult& r) {
      steps_ = r.steps;
      const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - runStart_).count();
      if (sec - lastLive < 1.0 || r.portVoltage.size() < 100)
        return;
      lastLive = sec;
      std::vector<double> f(opt.points);
      for (std::size_t i = 0; i < f.size(); ++i)
        f[i] = opt.fMinHz +
               (opt.fMaxHz - opt.fMinHz) * static_cast<double>(i) / static_cast<double>(f.size() - 1);
      auto s = fdtd::s11FromPort(r.portVoltage, r.portSource, r.dt, f, impedance);
      std::lock_guard<std::mutex> lock(mu_);
      liveS11_ = std::move(s);
    };
    opt.shouldStop = [&] { return stop_.load(); };
    sim::RunResult r = sim::runOnePort(prepared_->model, opt, backend_.factory);
    if (ntffAcc_ && !stop_) { // far field from the box phasors, S11 and the source at the same frequencies
      std::vector<FarResult> out;
      const auto s11 = fdtd::s11FromPort(r.portVoltage, r.portSource, r.dt, ntffFreqs_, impedance);
      const auto vs = fdtd::sourceSpectrum(r.portSource, r.dt, ntffFreqs_);
      const auto accepted =
          fdtd::acceptedPower(r.stripVoltage, r.portSource, r.dt, r.stripResistance, ntffFreqs_);
      for (std::size_t k = 0; k < ntffFreqs_.size(); ++k) {
        FarResult fr;
        fr.frequencyHz = ntffFreqs_[k];
        fr.field = fdtd::farField(
            ntffPlan_, [&](std::uint32_t ch) { return std::complex<double>(ntffAcc_->phasor(k, ch)); },
            ntffFreqs_[k], 36, 72);
        fr.figures = fdtd::antennaFigures(fr.field, vs[k], s11.s11[k], impedance, std::max(accepted[k], 0.0));
        fr.s11Db = 20 * std::log10(std::max(std::abs(s11.s11[k]), 1e-6));
        out.push_back(std::move(fr));
      }
      std::lock_guard<std::mutex> lock(mu_);
      farResults_ = std::move(out);
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      liveS11_ = r.s;
      result_ = std::move(r);
    }
    if (pinAfterRun_) {
      std::lock_guard<std::mutex> lock(mu_);
      pinnedS11_ = liveS11_;
    }
    app::writeResults(runDir_, env_.snapshotPath, snapshot_, env_.settings, *prepared_, estimate_, result_,
                      backend_.details.empty() ? backend_.name : backend_.name + "; " + backend_.details);
    if (probeAcc_) { // the probes' spectra next to the S11
      std::vector<std::vector<std::complex<double>>> ph(probes_.size());
      for (std::size_t p = 0; p < probes_.size(); ++p)
        for (std::size_t k = 0; k < probeFreqs_.size(); ++k)
          ph[p].push_back(probeAcc_->phasor(k, p));
      std::ofstream(runDir_ / "probes.csv") << app::probesCsv(probeFreqs_, probes_, ph);
    }
  } catch (const std::exception& e) {
    runError_ = e.what();
  }
  runSeconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - runStart_).count();
  finished_ = true;
}

void Ui::applyToProject() {
  try {
    fs::create_directories(env_.paths.root);
    std::ofstream(env_.paths.settingsFile) << env_.settings.toJson();
    message_ = "Saved for this board: " + env_.paths.settingsFile.string();
  } catch (const std::exception& e) {
    message_ = std::string("Could not save: ") + e.what();
  }
}

// ---- settings panel --------------------------------------------------------------------------------------

void Ui::drawSetting(const std::string& key) {
  static std::map<std::string, std::string> buffers, errors;
  const app::SettingDef& d = env_.settings.def(key);
  ImGui::PushID(key.c_str());
  const bool modified = env_.settings.source(key) == app::Source::User;
  ImGui::TextUnformatted(d.label.c_str());
  const std::string spoken = [&] {
    if (d.kind == app::Kind::Number)
      return strf("%g", env_.settings.number(key)) + (d.unit.empty() ? "" : " " + d.unit);
    return env_.settings.text(key);
  }();
  if (modified) {
    ImGui::SameLine();
    ImGui::TextColored(ImColor(pal.warn), "changed");
    ImGui::SameLine();
    if (smallButton("reset", ("Reset " + d.label).c_str())) {
      env_.settings.reset(key);
      buffers.erase(key);
      errors.erase(key);
      dirty_ = true;
      lastEdit_ = now_;
    }
  }
  try {
    ImGui::SetNextItemWidth(px(170));
    if (d.kind == app::Kind::Number) {
      std::string& buf = buffers[key];
      char text[48];
      if (!ImGui::IsAnyItemActive()) { // show the stored value unless the user is typing
        std::snprintf(text, sizeof text, "%g", env_.settings.number(key));
        buf = text;
      }
      std::snprintf(text, sizeof text, "%s", buf.c_str());
      const bool entered =
          ImGui::InputText("##v", text, sizeof text,
                           ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue |
                               ImGuiInputTextFlags_AutoSelectAll);
      note(viz::access::Role::TextInput, d.label, spoken, d.help, key);
      if (entered) {
        env_.settings.set(key, std::strtod(text, nullptr));
        errors.erase(key);
        dirty_ = true;
        lastEdit_ = now_;
      } else if (ImGui::IsItemActive()) {
        buf = text;
      } else if (ImGui::IsItemDeactivatedAfterEdit()) {
        env_.settings.set(key, std::strtod(text, nullptr));
        errors.erase(key);
        dirty_ = true;
        lastEdit_ = now_;
      }
      if (!d.unit.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImColor(pal.dim), "%s", d.unit.c_str());
      }
    } else if (d.kind == app::Kind::Colour) {
      const auto rgb = app::parseColour(env_.settings.text(key));
      float col[3] = {rgb[0] / 255.0f, rgb[1] / 255.0f, rgb[2] / 255.0f};
      const bool picked =
          ImGui::ColorEdit3("##c", col, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
      note(viz::access::Role::TextInput, d.label, spoken, d.help, key);
      if (picked) {
        char hex[8];
        std::snprintf(hex, sizeof hex, "#%02X%02X%02X", static_cast<int>(col[0] * 255 + 0.5f),
                      static_cast<int>(col[1] * 255 + 0.5f), static_cast<int>(col[2] * 255 + 0.5f));
        env_.settings.set(key, std::string(hex)); // colours only change the drawing, not the model
        errors.erase(key);
      }
      ImGui::SameLine();
      ImGui::TextColored(ImColor(pal.dim), "%s", env_.settings.text(key).c_str());
    } else {
      const auto choices = env_.settings.choices(key);
      const std::string current = env_.settings.text(key);
      auto label = [&](const std::string& id) {
        if (key == "profile")
          return env_.catalog.profileName(id);
        if (key == "variant")
          return env_.catalog.variant(env_.settings.text("profile"), id).name;
        if (key == "pcb_profile")
          return id == "board" ? std::string("The board's own stackup") : env_.catalog.fabProfile(id).name;
        if (key == "mesh_preset")
          for (const auto& p : env_.catalog.meshing().presets)
            if (p.first == id)
              return p.second.label;
        return id;
      };
      ImGui::SetNextItemWidth(px(330));
      const bool open = ImGui::BeginCombo("##c", label(current).c_str());
      note(viz::access::Role::ComboBox, d.label, label(current), d.help, key);
      if (open) {
        for (const std::string& c : choices)
          if (selectable(label(c), c == current)) {
            env_.settings.set(key, c);
            errors.erase(key);
            dirty_ = true;
            lastEdit_ = now_;
          }
        ImGui::EndCombo();
      }
    }
  } catch (const std::exception& e) {
    errors[key] = e.what();
  }
  if (errors.count(key))
    ImGui::TextColored(ImColor(pal.bad), "%s", errors[key].c_str());
  ImGui::PushStyleColor(ImGuiCol_Text, pal.dim);
  ImGui::TextWrapped("%s", d.help.c_str());
  if (key == "band_min_ghz") {
    const auto& v = env_.settings.currentVariant();
    ImGui::TextWrapped("%s: %s", v.status.c_str(),
                       env_.settings.source(key) == app::Source::User ? "set by you" : "from the profile");
  }
  ImGui::PopStyleColor();
  ImGui::Spacing();
  ImGui::PopID();
}

void Ui::perform(const std::vector<viz::access::Action>& actions) {
  using viz::access::Action;
  for (const Action& a : actions) {
    const viz::access::Node* n = access_.find(a.id);
    if (!n)
      continue; // the control is gone (the screen changed since the tree was sent)
    if (a.kind == Action::Kind::Click) {
      click_ = {n->x + n->w / 2, n->y + n->h / 2, 1};
    } else if (a.kind == Action::Kind::SetValue && !n->key.empty()) {
      try {
        if (env_.settings.def(n->key).kind == app::Kind::Number)
          env_.settings.set(n->key, std::strtod(a.text.c_str(), nullptr));
        else
          env_.settings.set(n->key, a.text);
        dirty_ = true;
        lastEdit_ = now_;
      } catch (const std::exception& e) {
        message_ = std::string("Could not set ") + n->name + ": " + e.what();
      }
    } // Focus: the screen reader's cursor is its own; ImGui's focus moves with the keyboard or a click
  }
}

void Ui::drawSettings() {
  ImGui::TextColored(ImColor(pal.accent), "Settings");
  noteText("Settings");
  ImGui::SameLine();
  ImGui::TextColored(ImColor(pal.dim), "(%zu changed)", env_.settings.modifiedKeys().size());
  ImGui::SetNextItemWidth(-1);
  char q[96];
  std::snprintf(q, sizeof q, "%s", searchText_.c_str());
  const bool searched =
      ImGui::InputTextWithHint("##search", "Search settings (name, help or keyword)", q, sizeof q);
  note(viz::access::Role::TextInput, "Search settings", searchText_,
       "Type a name, a word from the help or a keyword");
  if (searched) {
    searchText_ = q;
    debounce_.update(searchText_, now_);
  }
  if (auto applied = debounce_.poll(now_))
    searchApplied_ = *applied;
  ImGui::Separator();

  ImGui::BeginChild("##list", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 5.2f));
  const auto groups = env_.settings.groups();
  if (!searchApplied_.empty()) { // search results, any tier, grouped
    const auto shown = env_.settings.visible({}, searchApplied_);
    if (shown.empty())
      ImGui::TextColored(ImColor(pal.warn), "No setting matches \"%s\".", searchApplied_.c_str());
    for (const std::string& g : groups) {
      bool header = false;
      for (const std::string& key : shown)
        if (env_.settings.groupOf(key) == g) {
          if (!header) {
            ImGui::SeparatorText(g.c_str());
            noteText(g);
            header = true;
          }
          drawSetting(key);
        }
    }
  } else {
    for (const std::string& g : groups) {
      std::vector<std::string> essential, more;
      for (const auto& d : env_.settings.defs())
        if (d.group == g)
          (d.tier == app::Tier::Essential ? essential : more).push_back(d.key);
      if (essential.empty() && more.empty())
        continue;
      ImGui::SeparatorText(g.c_str());
      noteText(g);
      for (const auto& key : essential)
        drawSetting(key);
      if (!more.empty()) {
        char label[96];
        const bool open = expanded_.count(g) > 0;
        std::snprintf(label, sizeof label, "%s %zu more %s setting%s##%s", open ? "Hide" : "Show",
                      more.size(), g.c_str(), more.size() == 1 ? "" : "s", g.c_str());
        if (smallButton(label)) {
          if (open)
            expanded_.erase(g);
          else
            expanded_.insert(g);
        }
        if (open)
          for (const auto& key : more)
            drawSetting(key);
        ImGui::Spacing();
      }
    }
  }
  ImGui::EndChild();

  ImGui::Separator();
  if (button("Save for this board"))
    applyToProject();
  ImGui::SameLine();
  if (button("Reset all to defaults")) {
    env_.settings.resetAll();
    dirty_ = true;
    lastEdit_ = now_;
    message_ = "All settings are back to the profile defaults.";
  }
  static char name[64] = "my-profile";
  ImGui::SetNextItemWidth(px(170));
  ImGui::InputText("##pname", name, sizeof name);
  note(viz::access::Role::TextInput, "Profile name", name);
  ImGui::SameLine();
  if (button("Save as my profile")) {
    try {
      const fs::path dir = app::userConfigDir() / "profiles";
      fs::create_directories(dir);
      std::ofstream(dir / (std::string(name) + ".json")) << env_.settings.toJson();
      message_ = "Saved profile '" + std::string(name) + "' in " + dir.string();
    } catch (const std::exception& e) {
      message_ = std::string("Could not save the profile: ") + e.what();
    }
  }
  const fs::path dir = app::userConfigDir() / "profiles";
  if (fs::is_directory(dir)) {
    ImGui::SetNextItemWidth(px(250));
    const bool loadOpen = ImGui::BeginCombo("##load", "Load one of my profiles...");
    note(viz::access::Role::ComboBox, "Load one of my profiles");
    if (loadOpen) {
      for (const auto& f : fs::directory_iterator(dir))
        if (f.path().extension() == ".json" && ImGui::Selectable(f.path().stem().string().c_str())) {
          std::ifstream in(f.path());
          std::stringstream ss;
          ss << in.rdbuf();
          std::vector<std::string> problems;
          env_.settings.fromJson(ss.str(), problems);
          message_ = problems.empty() ? "Loaded profile '" + f.path().stem().string() + "'" : problems[0];
          dirty_ = true;
          lastEdit_ = now_;
        }
      ImGui::EndCombo();
    }
  }
  if (!message_.empty()) {
    wrapped(message_);
    if (gTree)
      gTree->mutableBack().live = true; // saved, loaded, failed: announced when it changes
  }
}

// ---- right-hand tabs -------------------------------------------------------------------------------------

void Ui::ensureGround() {
  if (groundGuessed_)
    return;
  groundNets_ = board::guessGroundNets(snapshot_);
  groundNets_.insert(extraGroundNets_.begin(), extraGroundNets_.end());
  groundGuessed_ = true;
}

void Ui::applyNets() {
  ensureGround();
  if (pickedNets_.empty()) {
    board::clearNetSelection(env_.selection);
  } else {
    const auto walk = board::walkNets(snapshot_, pickedNets_, groundNets_, env_.selection.parts);
    board::applyNetSelection(env_.selection, walk, groundNets_);
  }
  dirty_ = true;
  lastEdit_ = now_;
  saveSelection();
}

// The sidecar file next to the board holds the picks and the per-part treatments (never the KiCAD files). A
// command-line --nets wins over the file for this session.
void Ui::loadSelectionOnce() {
  if (selectionLoaded_)
    return;
  selectionLoaded_ = true;
  try {
    const app::StoredSelection st = app::loadSelection(env_.paths.selectionFile);
    if (env_.selection.parts.empty())
      env_.selection.parts = st.parts;
    if (env_.selection.bodies.empty())
      env_.selection.bodies = st.bodies;
    extraGroundNets_ = st.extraGroundNets;
    pickedNets_ = env_.selection.netFilter ? env_.selection.pickedNets : st.pickedNets;
    if (!pickedNets_.empty() && !env_.selection.netFilter)
      applyNets();
  } catch (const std::exception& e) {
    message_ = std::string("The saved model selection could not be read, so it is not used: ") + e.what();
  }
}

void Ui::saveSelection() {
  app::StoredSelection st;
  st.pickedNets = pickedNets_;
  st.extraGroundNets = extraGroundNets_;
  st.parts = env_.selection.parts;
  st.bodies = env_.selection.bodies;
  st.defaultBody = env_.selection.defaultBody;
  try {
    app::saveSelection(env_.paths.selectionFile, st);
  } catch (const std::exception& e) {
    message_ = std::string("Could not save the model selection: ") + e.what();
  }
}

// The model selection: which nets to model (the parts that join them follow), and what each two-terminal
// part is. Collapsed by default: the default is to model everything (docs/ux.md, "Model and view selection").
void Ui::drawModelSelection() {
  if (openModelOnce_ || modelScrollFrames_ > 0) // a screenshot request: open it and bring it into view
    ImGui::SetNextItemOpen(true);
  if (!ImGui::CollapsingHeader("Parts and nets in the model (optional)"))
    return;
  if (modelScrollFrames_ > 0) {
    ImGui::SetScrollHereY(0.0f);
    --modelScrollFrames_;
  }
  openModelOnce_ = false;
  ensureGround();
  wrapped("By default everything is modelled. Tick nets to model only those, plus the parts that join them "
          "(resistors, inductors, capacitors and 0 ohm links carry the selection across to the net on their "
          "other side; ICs and connectors are included but not crossed). Ground and plane nets are always "
          "kept.");
  std::set<std::string> nets;
  for (const auto& c : snapshot_.copper)
    if (!c.net.empty())
      nets.insert(c.net);
  bool changed = false;
  for (const std::string& n : nets) {
    bool on = pickedNets_.count(n) > 0;
    const std::string label = n + (groundNets_.count(n) ? " (looks like ground or a plane)" : "");
    if (checkbox(label.c_str(), &on)) {
      if (on)
        pickedNets_.insert(n);
      else
        pickedNets_.erase(n);
      changed = true;
    }
    ImGui::SameLine();
  }
  ImGui::NewLine();
  if (!pickedNets_.empty() && smallButton("Model everything again")) {
    pickedNets_.clear();
    changed = true;
  }
  if (changed)
    applyNets();
  for (const std::string& n : env_.selection.netNotes)
    ImGui::TextColored(ImColor(pal.dim), "%s", n.c_str());

  // two-terminal parts in the model: what each one is, and the user's override
  bool any = false;
  for (const auto& comp : snapshot_.components) {
    if (!board::isTwoTerminal(comp) || !env_.selection.includes(comp.ref))
      continue;
    if (!any) {
      ImGui::SeparatorText("Two-terminal parts");
      wrapped("What sits between each part's two pads. Auto reads the value; Open, Short or Values override "
              "it (Values: a parallel R, L, C, for example a ferrite bead). A part marked do-not-populate is "
              "open unless you change it.");
      any = true;
    }
    ImGui::PushID(comp.ref.c_str());
    board::PartTreatment t;
    if (const auto it = env_.selection.parts.find(comp.ref); it != env_.selection.parts.end())
      t = it->second;
    const board::PartModel pm = board::modelPart(comp, t);
    std::string what = pm.kind == board::PartModel::Kind::Lumped
                           ? (pm.shortIt ? "short"
                                         : strf("%s%s%s", pm.r ? strf("R %.4g ohm ", *pm.r).c_str() : "",
                                                pm.l ? strf("L %.4g nH ", *pm.l * 1e9).c_str() : "",
                                                pm.c ? strf("C %.4g pF", *pm.c * 1e12).c_str() : ""))
                       : pm.kind == board::PartModel::Kind::Open ? "open"
                                                                 : "not modelled";
    ImGui::Text("%s  %s%s", comp.ref.c_str(), comp.value.c_str(), comp.dnp ? "  [DNP]" : "");
    ImGui::SameLine(px(190));
    static const char* names[] = {"Auto", "Open", "Short", "Values"};
    int cur = static_cast<int>(t.state);
    ImGui::SetNextItemWidth(px(110));
    const bool open = ImGui::BeginCombo("##treat", names[cur]);
    note(viz::access::Role::ComboBox, strf("Treatment of %s", comp.ref.c_str()), names[cur],
         "Auto reads the value field; Open leaves the gap empty; Short is a 0 ohm link; Values sets R, L and "
         "C");
    if (open) {
      for (int i = 0; i < 4; ++i)
        if (selectable(names[i], i == cur)) {
          t.state = static_cast<board::PartTreatment::State>(i);
          env_.selection.parts[comp.ref] = t;
          if (!pickedNets_.empty())
            applyNets();
          dirty_ = true;
          lastEdit_ = now_;
          saveSelection();
        }
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextColored(ImColor(pal.dim), "%s", what.c_str());
    if (t.state == board::PartTreatment::State::Values) {
      auto& txt = partText_[comp.ref];
      if (txt[0].empty() && txt[1].empty() && txt[2].empty()) { // fill from the saved values
        if (t.r)
          txt[0] = strf("%g", *t.r);
        if (t.l)
          txt[1] = strf("%gn", *t.l * 1e9);
        if (t.c)
          txt[2] = strf("%gp", *t.c * 1e12);
      }
      const char* hint[3] = {"R (ohm, e.g. 600)", "L (e.g. 100n)", "C (e.g. 0.2p)"};
      for (int k = 0; k < 3; ++k) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%s", txt[static_cast<std::size_t>(k)].c_str());
        ImGui::SetNextItemWidth(px(150));
        const bool edited = ImGui::InputTextWithHint(strf("##v%d", k).c_str(), hint[k], buf, sizeof buf);
        note(viz::access::Role::TextInput,
             strf("%s of %s",
                  k == 0   ? "Resistance"
                  : k == 1 ? "Inductance"
                           : "Capacitance",
                  comp.ref.c_str()),
             txt[static_cast<std::size_t>(k)], hint[k]);
        ImGui::SameLine();
        if (edited) {
          txt[static_cast<std::size_t>(k)] = buf;
          const std::string v = buf;
          board::PartTreatment nt = env_.selection.parts[comp.ref];
          const auto x = v.empty() ? std::optional<double>()
                         : k == 0  ? board::parseResistance(v)
                         : k == 1  ? board::parseInductance(v + (v.back() == 'H' ? "" : "H"))
                                   : board::parseCapacitance(v);
          (k == 0 ? nt.r : k == 1 ? nt.l : nt.c) = x;
          env_.selection.parts[comp.ref] = nt;
          if (!pickedNets_.empty())
            applyNets();
          dirty_ = true;
          lastEdit_ = now_;
          saveSelection();
        }
      }
      ImGui::NewLine();
    }
    ImGui::PopID();
  }
  if (!any)
    ImGui::TextColored(ImColor(pal.dim), "No two-terminal parts in the model (or this board file has no pad "
                                         "data: export it again from KiCAD).");

  // component bodies: the footprint outline extruded by the body height (settings), as a dielectric or metal
  // block
  bool anyBody = false;
  for (const auto& comp : snapshot_.components) {
    if (comp.body.empty() || !env_.selection.includes(comp.ref))
      continue;
    if (!anyBody) {
      ImGui::SeparatorText("Component bodies");
      wrapped(
          "Each part's footprint outline, extruded by the body height (Settings, PCB). Default follows the "
          "\"Component bodies\" setting; the others override it for this part. A dielectric body uses the "
          "part's "
          "permittivity below, or the setting's.");
      anyBody = true;
    }
    ImGui::PushID((comp.ref + "#body").c_str());
    const auto it = env_.selection.bodies.find(comp.ref);
    int cur = it == env_.selection.bodies.end() ? 0 : 1 + static_cast<int>(it->second.kind);
    static const char* bodyNames[] = {"Default", "Ignore", "Dielectric", "Metal"};
    ImGui::Text("%s  %s", comp.ref.c_str(), comp.value.c_str());
    ImGui::SameLine(px(190));
    ImGui::SetNextItemWidth(px(110));
    const bool open = ImGui::BeginCombo("##body", bodyNames[cur]);
    note(viz::access::Role::ComboBox, strf("Body of %s", comp.ref.c_str()), bodyNames[cur],
         "Default follows the Component bodies setting; Ignore leaves the body out; Dielectric and Metal "
         "fill it");
    if (open) {
      for (int i = 0; i < 4; ++i)
        if (selectable(bodyNames[i], i == cur)) {
          if (i == 0) {
            env_.selection.bodies.erase(comp.ref);
          } else {
            board::BodyTreatment bt;
            bt.kind = static_cast<board::BodyKind>(i - 1);
            bt.epsilonR = it == env_.selection.bodies.end() ? env_.settings.number("component_body_eps")
                                                            : it->second.epsilonR;
            env_.selection.bodies[comp.ref] = bt;
          }
          dirty_ = true;
          lastEdit_ = now_;
          saveSelection();
        }
      ImGui::EndCombo();
    }
    const auto now = env_.selection.bodies.find(comp.ref); // re-found: the combo may just have changed it
    if (now != env_.selection.bodies.end() && now->second.kind == board::BodyKind::Dielectric) {
      float eps = static_cast<float>(now->second.epsilonR);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(px(140));
      if (sliderFloat(strf("Permittivity of %s", comp.ref.c_str()).c_str(), "##beps", &eps, 1.0f, 30.0f,
                      "epsilon_r %.1f")) {
        env_.selection.bodies[comp.ref].epsilonR = static_cast<double>(eps);
        dirty_ = true;
        lastEdit_ = now_;
        saveSelection();
      }
    }
    ImGui::PopID();
  }
}

void Ui::drawPreviewTab(float w, float h) {
  const board::Snapshot& s = snapshot_;
  if (prepared_) {
    checkbox("Show the model as the solver sees it (grid and copper edges)", &showVoxelModel_);
    h -= ImGui::GetFrameHeightWithSpacing();
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float drawH = std::max(120.0f, h - 190);
  dl->AddRectFilled(origin, {origin.x + w, origin.y + drawH}, pal.plotBg);
  if (prepared_) {
    const auto& m = prepared_->model;
    const double x0 = m.originX, y0 = m.originY, dx = m.grid.length(fdtd::X), dy = m.grid.length(fdtd::Y);
    const double scale = std::min((w - 20) / dx, (drawH - 20) / dy);
    auto P = [&](double x, double y) {
      return ImVec2(origin.x + 10 + static_cast<float>((x - x0) * scale),
                    origin.y + drawH - 10 - static_cast<float>((y - y0) * scale));
    };
    // domain, with the absorbing layer shaded
    dl->AddRect(P(x0, y0), P(x0 + dx, y0 + dy), pal.geoBox, 0, 0, 1.0f);
    const double pml = static_cast<double>(prepared_->plan.pmlCells) * prepared_->plan.coarseCell;
    dl->AddRectFilled(P(x0, y0 + dy), P(x0 + dx, y0 + dy - pml), alpha(pal.pml, 120));
    dl->AddRectFilled(P(x0, y0 + pml), P(x0 + dx, y0), alpha(pal.pml, 120));
    dl->AddRectFilled(P(x0, y0 + dy), P(x0 + pml, y0), alpha(pal.pml, 120));
    dl->AddRectFilled(P(x0 + dx - pml, y0 + dy), P(x0 + dx, y0), alpha(pal.pml, 120));
    // board outline and copper
    std::vector<ImVec2> pts;
    for (const auto& p : s.outline)
      pts.push_back(P(p.x, p.y));
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), pal.geoOutline, ImDrawFlags_Closed, 1.5f);
    for (const auto& c : s.copper) {
      if (!env_.selection.includes(c.owner) || !env_.selection.netIncluded(c.net))
        continue;
      pts.clear();
      for (const auto& p : c.outer)
        pts.push_back(P(p.x, p.y));
      const bool top = !s.stackup.empty() && c.layer == s.stackup.front().name;
      dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), top ? pal.geoTop : alpha(pal.geoOther, 160),
                      ImDrawFlags_Closed, 1.0f);
    }
    // component bodies that are in the model (a dielectric block in blue, a metal one in grey), by their
    // outline
    for (const auto& comp : s.components) {
      if (comp.body.empty() || !env_.selection.includes(comp.ref))
        continue;
      const auto it = env_.selection.bodies.find(comp.ref);
      const std::string mode = it != env_.selection.bodies.end()
                                   ? (it->second.kind == board::BodyKind::Dielectric ? "dielectric"
                                      : it->second.kind == board::BodyKind::Metal    ? "metal"
                                                                                     : "ignore")
                                   : env_.settings.text("component_bodies");
      if (mode == "ignore")
        continue;
      std::vector<ImVec2> bp;
      for (const auto& p : comp.body)
        bp.push_back(P(p.x, p.y));
      const ImU32 fill = mode == "metal" ? IM_COL32(170, 170, 180, 70) : IM_COL32(86, 180, 233, 60);
      std::vector<std::array<double, 2>> flat;
      for (const ImVec2& q : bp)
        flat.push_back({q.x, q.y});
      for (const auto& t : viz::triangulate(flat))
        dl->AddTriangleFilled(bp[t[0]], bp[t[1]], bp[t[2]], fill);
      dl->AddPolyline(bp.data(), static_cast<int>(bp.size()), alpha(fill, 200), ImDrawFlags_Closed, 1.5f);
      dl->AddText({bp[0].x + 2, bp[0].y - 14}, pal.dim, comp.ref.c_str());
    }
    if (showVoxelModel_) {
      // What the solver actually has: the grid lines, the copper as forced edges (thick on the top layer,
      // faint on the others) and the ports' edges. A staircased edge or a widened port shows here before a
      // result is trusted.
      const auto& gr = m.grid;
      auto G = [&](double gx, double gy) { return P(gx + m.originX, gy + m.originY); };
      const std::size_t lines = std::max(gr.nodes(fdtd::X), gr.nodes(fdtd::Y));
      const std::size_t every = std::max<std::size_t>(1, lines / 160); // keep the grid readable on big models
      for (std::size_t i = 0; i < gr.nodes(fdtd::X); i += every)
        dl->AddLine(G(gr.edge(fdtd::X, i), 0), G(gr.edge(fdtd::X, i), gr.length(fdtd::Y)),
                    alpha(pal.gridLine, 90));
      for (std::size_t j = 0; j < gr.nodes(fdtd::Y); j += every)
        dl->AddLine(G(0, gr.edge(fdtd::Y, j)), G(gr.length(fdtd::X), gr.edge(fdtd::Y, j)),
                    alpha(pal.gridLine, 90));
      std::size_t kTopNode = 0;
      for (std::size_t i = 0; i < s.stackup.size(); ++i)
        if (s.stackup[i].copper)
          kTopNode = std::max(kTopNode, m.copperPlaneNode[i]);
      const std::size_t plate = gr.nodes(fdtd::X) * gr.nodes(fdtd::Y);
      const std::size_t stride = std::max<std::size_t>(1, m.pecEdges.size() / 60000);
      for (std::size_t n = 0; n < m.pecEdges.size(); n += stride) {
        const auto& e = m.pecEdges[n];
        if (e.axis == fdtd::Z)
          continue;
        const std::size_t k = e.node / plate, i = e.node % gr.nodes(fdtd::X),
                          j = (e.node % plate) / gr.nodes(fdtd::X);
        const bool top = k == kTopNode;
        const ImVec2 a = G(gr.edge(fdtd::X, i), gr.edge(fdtd::Y, j));
        const ImVec2 b = e.axis == fdtd::X ? G(gr.edge(fdtd::X, i + 1), gr.edge(fdtd::Y, j))
                                           : G(gr.edge(fdtd::X, i), gr.edge(fdtd::Y, j + 1));
        dl->AddLine(a, b, top ? pal.geoTop : alpha(pal.geoOther, 90), top ? 2.5f : 1.0f);
      }
      for (const auto& port : m.ports)
        for (const auto& pe : port.edges) {
          const auto& ed = pe.edge;
          const ImVec2 a = G(gr.edge(fdtd::X, ed.i), gr.edge(fdtd::Y, ed.j));
          if (ed.component == fdtd::Z) {
            dl->AddCircleFilled(a, 3.5f, pal.geoPort);
          } else {
            const ImVec2 b = ed.component == fdtd::X ? G(gr.edge(fdtd::X, ed.i + 1), gr.edge(fdtd::Y, ed.j))
                                                     : G(gr.edge(fdtd::X, ed.i), gr.edge(fdtd::Y, ed.j + 1));
            dl->AddLine(a, b, pal.geoPort, 3.5f);
          }
        }
    }
    for (const auto& v : s.vias) {
      const ImVec2 c = P(v.x, v.y);
      dl->AddCircle(c, std::max(1.5f, static_cast<float>(0.5 * v.diameter * scale)), alpha(pal.geoVia, 200),
                    12);
    }
    for (const auto& pl : prepared_->model.ports) {
      for (const auto& pd : s.ports)
        if (pd.name == pl.name) {
          const ImVec2 a = P(pd.a.x, pd.a.y), b = P(pd.b.x, pd.b.y);
          dl->AddCircleFilled(a, 5, pal.geoPort);
          dl->AddLine(a, b, pal.geoPort, 2);
          dl->AddText({a.x + 8, a.y - 18}, pal.geoPortText, (std::string("port ") + pd.name).c_str());
        }
    }
    drawLegend(dl, {std::max(origin.x + 10, origin.x + w - (legendOpen_[0] ? 400.0f : 120.0f)), origin.y + 8},
               "Legend",
               {{LegendItem::Line, pal.geoTop, "top copper layer"},
                {LegendItem::Line, pal.geoOther, "other copper layers"},
                {LegendItem::Line, pal.geoOutline, "board outline"},
                {LegendItem::Ring, pal.geoVia, "via"},
                {LegendItem::Dot, pal.geoPort, "port (where the signal is applied)"},
                {LegendItem::Swatch, pal.pml, "absorbing layer (edge of the domain)"},
                {LegendItem::Line, pal.geoBox, "domain (free space around the board)"},
                {LegendItem::Swatch, IM_COL32(86, 180, 233, 120),
                 "component body in the model (blue: dielectric, grey: metal)"}},
               "legendPreview", legendOpen_[0]);
  } else {
    dl->AddText({origin.x + 14, origin.y + 14}, pal.bad,
                prepareError_.empty() ? "Preparing the model..." : prepareError_.c_str());
  }
  ImGui::Dummy({w, drawH});

  if (ImGui::CollapsingHeader("Before you run: estimate and assumptions", ImGuiTreeNodeFlags_DefaultOpen) &&
      prepared_) {
    const auto& g = prepared_->model.grid;
    wrapped(strf("Band of interest %.4g - %.4g GHz   |   S11 reported %.4g - %.4g GHz   |   source covers "
                 "%.4g - %.4g GHz",
                 prepared_->plan.bandHz[0] * 1e-9, prepared_->plan.bandHz[1] * 1e-9,
                 prepared_->plan.sweepHz[0] * 1e-9, prepared_->plan.sweepHz[1] * 1e-9,
                 prepared_->plan.excitationHz[0] * 1e-9, prepared_->plan.excitationHz[1] * 1e-9));
    wrapped(strf("Grid %zu x %zu x %zu = %.2f million cells, smallest cell %.3g mm", g.cells(fdtd::X),
                 g.cells(fdtd::Y), g.cells(fdtd::Z), static_cast<double>(g.cellCount()) * 1e-6,
                 estimate_.smallestCell * 1e3));
    coloured(ImColor(estimate_.exceedsMemory ? pal.bad : pal.text),
             strf("Estimate: about %s of memory, ~%zu steps, roughly %s on the %s",
                  fmtBytes(estimate_.memoryBytes).c_str(), estimate_.steps,
                  fmtTime(estimate_.seconds).c_str(), backend_.name.c_str()));
    coloured(ImColor(pal.dim), "(a rough guide: the run stops by itself when the signal has decayed)");
    if (env_.settings.text("mesh_preset") == "coarse")
      coloured(ImColor(pal.warn), "Coarse mesh: resonances can shift by several percent. Use it for a "
                                  "quick look, then re-run at Medium or Fine.");
    const auto& v = env_.settings.currentVariant();
    wrapped(strf("Assumptions: profile \"%s\" [%s]. %s", v.name.c_str(), v.status.c_str(), v.source.c_str()));
    for (const auto& n : prepared_->model.notes)
      wrapped("- " + n);
  }
  drawModelSelection();
  drawProbePanel();
  drawSweepPanel();
}

viz::Rgb Ui::fieldColour(bool light, bool positive) const {
  const std::string key =
      std::string("colour_") + (light ? "light_" : "dark_") + (positive ? "positive" : "negative");
  try {
    const auto c = app::parseColour(env_.settings.text(key));
    return {c[0], c[1], c[2]};
  } catch (const std::exception&) { // a bad saved value: fall back to the theme default
    return light ? (positive ? viz::kLightPositive : viz::kLightNegative)
                 : (positive ? viz::kDarkPositive : viz::kDarkNegative);
  }
}

// The S11 tab: the plot in dB (default), the Smith chart, or impedance and VSWR against frequency. All three
// show the same sweep; the chart and the numbers are derived from the complex S11.
void Ui::drawS11Tab(float w, float h) {
  bool haveRun, haveSweep;
  {
    std::lock_guard<std::mutex> lock(mu_);
    haveRun = !liveS11_.s11.empty();
    haveSweep = !sweepPoints_.empty();
  }
  if (!prepared_ || (!haveRun && !haveSweep)) {
    drawS11Plot(w, h); // the waiting message
    return;
  }
  static const char* names[4] = {"S11 in dB", "Smith chart", "Impedance and VSWR", "Sweep overlay"};
  auto available = [&](int i) { return i == 3 ? haveSweep : haveRun; };
  if (!available(s11View_))
    s11View_ = haveRun ? 0 : 3;
  bool first = true;
  for (int i = 0; i < 4; ++i) {
    if (!available(i))
      continue;
    if (!first)
      ImGui::SameLine();
    first = false;
    const bool on = s11View_ == i;
    if (on)
      ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (button(names[i]))
      s11View_ = i;
    if (on)
      ImGui::PopStyleColor();
    if (gTree) {
      gTree->mutableBack().checkable = true;
      gTree->mutableBack().checked = on;
    }
  }
  if (haveRun) { // freeze-and-compare: keep this run's S11 as a grey dashed reference under the next runs
    ImGui::SameLine();
    if (button(pinnedS11_.s11.empty() ? "Pin this S11 as a reference" : "Replace the pinned S11")) {
      std::lock_guard<std::mutex> lock(mu_);
      pinnedS11_ = liveS11_;
      pinnedLabel_ = "pinned " + app::timestamp();
    }
    if (!pinnedS11_.s11.empty()) {
      ImGui::SameLine();
      if (button("Clear the pin")) {
        std::lock_guard<std::mutex> lock(mu_);
        pinnedS11_ = {};
      }
    }
  }
  const float rowH = ImGui::GetFrameHeightWithSpacing();
  if (s11View_ == 3)
    drawSweepOverlay(w, h - rowH);
  else if (s11View_ == 1)
    drawSmithChart(w, h - rowH);
  else if (s11View_ == 2)
    drawImpedancePlot(w, h - rowH);
  else
    drawS11Plot(w, h - rowH);
}

void Ui::drawS11Plot(float w, float h) {
  fdtd::SParam1 s;
  {
    std::lock_guard<std::mutex> lock(mu_);
    s = liveS11_;
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetCursorScreenPos();
  const float L = 60, B = 40, T = 10, R = 20;
  const ImVec2 p0{o.x + L, o.y + T}, p1{o.x + w - R, o.y + h - B};
  dl->AddRectFilled(o, {o.x + w, o.y + h}, pal.plotBg);
  const auto& plan = prepared_ ? prepared_->plan : app::RunPlan{};
  if (!prepared_ || s.s11.empty()) {
    dl->AddText({o.x + 14, o.y + 14}, pal.dim,
                "S11 (how much power reflects back) appears here as the run proceeds.");
    ImGui::Dummy({w, h});
    return;
  }
  const double f0 = plan.sweepHz[0], f1 = plan.sweepHz[1];
  double lo = -30;
  for (const auto& c : s.s11)
    lo = std::min(lo, 20 * std::log10(std::max(std::abs(c), 1e-6)) - 2);
  lo = std::floor(lo / 10) * 10;
  auto X = [&](double f) { return p0.x + static_cast<float>((f - f0) / (f1 - f0)) * (p1.x - p0.x); };
  auto Y = [&](double db) { return p0.y + static_cast<float>((0 - db) / (0 - lo)) * (p1.y - p0.y); };
  // band of interest and channel markers
  dl->AddRectFilled({X(plan.bandHz[0]), p0.y}, {X(plan.bandHz[1]), p1.y}, alpha(pal.curve, 40));
  int channelRow = 0;
  for (const auto& ch :
       env_.settings.currentVariant().channels) { // labels staggered so neighbours stay readable
    dl->AddLine({X(ch.centerHz), p0.y}, {X(ch.centerHz), p1.y}, alpha(pal.geoBox, 160), 1);
    dl->AddText({X(ch.centerHz) + 3, p0.y + 2 + 14.0f * static_cast<float>(channelRow++ % 3)}, pal.dim,
                ch.label.c_str());
  }
  for (double db = 0; db >= lo; db -= 10) { // dB grid
    dl->AddLine({p0.x, Y(db)}, {p1.x, Y(db)}, pal.gridLine);
    char t[16];
    std::snprintf(t, sizeof t, "%.0f", db);
    dl->AddText({o.x + 24, Y(db) - 7}, pal.dim, t);
  }
  // -10 dB reference, dashed so it does not rely on colour
  for (float x = p0.x; x < p1.x; x += 12)
    dl->AddLine({x, Y(-10)}, {std::min(x + 6, p1.x), Y(-10)}, pal.good, 1.5f);
  dl->AddText({p1.x - 190, Y(-10) - 16}, pal.good, "-10 dB: a good match");
  for (int i = 0; i <= 4; ++i) { // frequency ticks
    const double f = f0 + (f1 - f0) * i / 4.0;
    dl->AddText({X(f) - 14, p1.y + 6}, pal.dim, ghz(f).c_str());
  }
  dl->AddText({o.x + w / 2 - 60, p1.y + 22}, pal.dim, "frequency (GHz)    |S11| (dB)");
  std::vector<ImVec2> line;
  for (std::size_t i = 0; i < s.s11.size(); ++i)
    line.push_back(
        {X(s.frequenciesHz[i]), Y(std::max(20 * std::log10(std::max(std::abs(s.s11[i]), 1e-6)), lo))});
  if (!pinnedS11_.s11.empty()) { // dashed grey: the reference pinned from an earlier run
    std::vector<ImVec2> ref;
    for (std::size_t i = 0; i < pinnedS11_.s11.size(); ++i)
      ref.push_back({X(pinnedS11_.frequenciesHz[i]),
                     Y(std::max(20 * std::log10(std::max(std::abs(pinnedS11_.s11[i]), 1e-6)), lo))});
    for (std::size_t i = 0; i + 1 < ref.size(); i += 2)
      dl->AddLine(ref[i], ref[i + 1], pal.dim, 2.0f);
  }
  dl->AddPolyline(line.data(), static_cast<int>(line.size()), pal.curve, 0, 2.5f);
  std::array<float, 4> bestLabel{}; // x0, y0, x1, y1 of the label and dot, so the legend keeps clear of it
  std::size_t best = 0;
  for (std::size_t i = 1; i < s.s11.size(); ++i)
    if (std::abs(s.s11[i]) < std::abs(s.s11[best]))
      best = i;
  const double bdb = 20 * std::log10(std::max(std::abs(s.s11[best]), 1e-6));
  dl->AddCircleFilled({X(s.frequenciesHz[best]), Y(bdb)}, 4, pal.text);
  char t[96];
  std::snprintf(t, sizeof t, "best %.1f dB at %.3f GHz", bdb, s.frequenciesHz[best] * 1e-9);
  {
    // beside the dot, on whichever side has room, and never below the plot
    const ImVec2 size = ImGui::CalcTextSize(t);
    const float dotX = X(s.frequenciesHz[best]), dotY = Y(bdb);
    const float x = dotX + 8 + size.x <= p1.x ? dotX + 8 : std::max(p0.x, dotX - 8 - size.x);
    const float y = std::min(dotY + 4, p1.y - size.y - 2);
    dl->AddText({x, y}, pal.text, t);
    bestLabel = {x - 4, std::min(y, dotY - 6), x + size.x + 4, std::max(y + size.y, dotY + 6)};
  }
  if (gTree) { // the plot in words: the best match, and where the match is better than -10 dB
    std::string below;
    double first = 0, last = 0;
    for (std::size_t i = 0; i < s.s11.size(); ++i)
      if (20 * std::log10(std::max(std::abs(s.s11[i]), 1e-6)) <= -10) {
        if (first == 0)
          first = s.frequenciesHz[i];
        last = s.frequenciesHz[i];
      }
    below = first > 0 ? strf("Better than -10 dB from %.3f to %.3f GHz.", first * 1e-9, last * 1e-9)
                      : "Never better than -10 dB: the antenna is not matched in this sweep.";
    noteText(strf("Plot of S11 in dB against frequency, %.3f to %.3f GHz. Best match %.1f dB at %.3f GHz. %s",
                  s.frequenciesHz.front() * 1e-9, s.frequenciesHz.back() * 1e-9, bdb,
                  s.frequenciesHz[best] * 1e-9, below.c_str()),
             viz::access::Role::Image);
  }
  {
    std::vector<LegendItem> items = {
        {LegendItem::Line, pal.curve, "|S11| in dB (lower = more power accepted)"},
        {LegendItem::Swatch, alpha(pal.curve, 90), "band of interest"},
        {LegendItem::Dashed, pal.good, "-10 dB: a good match"},
        {LegendItem::Line, pal.geoBox, "channel centres (profile)"},
        {LegendItem::Dot, pal.text, "best match in the sweep"}};
    if (!pinnedS11_.s11.empty())
      items.push_back({LegendItem::Dashed, pal.dim, "pinned reference (an earlier run)"});
    // Keep the whole legend inside the plot and clear of the best-match label: the expanded box at the lower
    // right if it fits and does not cover the label, else the one-line chip there, else the chip at the lower
    // left. (The user can still collapse or expand it by clicking its header.)
    const float margin = px(10);
    auto hits = [&](ImVec2 at, ImVec2 size) {
      return at.x < bestLabel[2] && at.x + size.x > bestLabel[0] && at.y < bestLabel[3] &&
             at.y + size.y > bestLabel[1];
    };
    auto fits = [&](ImVec2 size) {
      return size.x <= p1.x - p0.x - 2 * margin && size.y <= p1.y - p0.y - 2 * margin;
    };
    bool open = legendOpen_[1], forcedShut = false;
    ImVec2 box = legendBox("Legend", items, open);
    ImVec2 at{std::max(p0.x + margin, p1.x - box.x - margin), std::max(p0.y + margin, p1.y - box.y - margin)};
    if (open && (!fits(box) || hits(at, box))) {
      open = false;
      forcedShut = true;
      box = legendBox("Legend", items, false);
      at = {std::max(p0.x + margin, p1.x - box.x - margin), std::max(p0.y + margin, p1.y - box.y - margin)};
    }
    if (hits(at, box))
      at.x = p0.x + margin; // the label is at the right: use the lower left
    drawLegend(dl, at, "Legend", items, "legendS11", open);
    if (!forcedShut) // a click on a chip that was only shut for lack of room changes nothing
      legendOpen_[1] = open;
  }
  ImGui::Dummy({w, h});
}

void Ui::drawSmithChart(float w, float h) {
  fdtd::SParam1 s;
  {
    std::lock_guard<std::mutex> lock(mu_);
    s = liveS11_;
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetCursorScreenPos();
  dl->AddRectFilled(o, {o.x + w, o.y + h}, pal.plotBg);
  const double z0 = prepared_ ? prepared_->plan.portImpedanceOhm : 50.0;
  const float radius = std::max(40.0f, 0.5f * std::min(w - 30, h - 30));
  const ImVec2 c{o.x + 0.5f * w - 0.25f * std::max(0.0f, w - 2 * radius - 260), o.y + 0.5f * h};
  auto P = [&](std::complex<double> g) {
    return ImVec2(c.x + static_cast<float>(g.real()) * radius, c.y - static_cast<float>(g.imag()) * radius);
  };
  dl->AddCircle(c, radius, pal.dim, 96, 1.5f);
  dl->AddLine({c.x - radius, c.y}, {c.x + radius, c.y}, pal.gridLine);
  // constant-resistance circles and constant-reactance arcs, normalised to the port impedance
  for (const double r : {0.2, 0.5, 1.0, 2.0, 5.0}) {
    dl->AddCircle(P({r / (1 + r), 0}), static_cast<float>(1 / (1 + r)) * radius, pal.gridLine, 64);
    dl->AddText({P({(r - 1) / (r + 1), 0}).x - 4, c.y + 2}, pal.dim, strf("%g", r).c_str());
  }
  for (const double x : {0.2, 0.5, 1.0, 2.0, 5.0})
    for (const double sign : {1.0, -1.0}) {
      std::vector<ImVec2> arc;
      for (int i = 0; i <= 120; ++i) {
        const double t = i / 120.0, r = 400.0 * t * t * t; // dense near r = 0, reaching out to the open point
        const std::complex<double> z(r, sign * x);
        arc.push_back(P((z - 1.0) / (z + 1.0)));
      }
      dl->AddPolyline(arc.data(), static_cast<int>(arc.size()), pal.gridLine, 0, 1.0f);
    }
  std::vector<ImVec2> trace;
  for (const auto& g : s.s11)
    trace.push_back(P(std::abs(g) > 1 ? g / std::abs(g) : g));
  dl->AddPolyline(trace.data(), static_cast<int>(trace.size()), pal.curve, 0, 2.5f);
  // markers: the band of interest's ends, and the best match
  const auto& plan = prepared_->plan;
  auto nearest = [&](double f) {
    std::size_t k = 0;
    for (std::size_t i = 1; i < s.frequenciesHz.size(); ++i)
      if (std::abs(s.frequenciesHz[i] - f) < std::abs(s.frequenciesHz[k] - f))
        k = i;
    return k;
  };
  for (const double f : {plan.bandHz[0], plan.bandHz[1]}) {
    const auto k = nearest(f);
    dl->AddCircleFilled(trace[k], 4, pal.warn);
    dl->AddText({trace[k].x + 6, trace[k].y - 14}, pal.warn, strf("%.3f GHz", f * 1e-9).c_str());
  }
  std::size_t best = 0;
  for (std::size_t i = 1; i < s.s11.size(); ++i)
    if (std::abs(s.s11[i]) < std::abs(s.s11[best]))
      best = i;
  dl->AddCircleFilled(trace[best], 5, pal.text);
  const auto zb = fdtd::impedanceFromGamma(s.s11[best], z0);
  const double vb = fdtd::vswr(s.s11[best]);
  const std::string summary =
      strf("Best match at %.3f GHz: Z = %.1f %s j%.1f ohm, VSWR %.2f, return loss %.1f dB",
           s.frequenciesHz[best] * 1e-9, zb.real(), zb.imag() < 0 ? "-" : "+", std::abs(zb.imag()), vb,
           fdtd::returnLossDb(s.s11[best]));
  const float tx = c.x + radius + 24;
  dl->AddText({tx, o.y + 12}, pal.text, "Best match in the sweep");
  dl->AddText({tx, o.y + 12 + 18}, pal.text, strf("%.3f GHz", s.frequenciesHz[best] * 1e-9).c_str());
  dl->AddText(
      {tx, o.y + 12 + 36}, pal.text,
      strf("Z = %.1f %s j%.1f ohm", zb.real(), zb.imag() < 0 ? "-" : "+", std::abs(zb.imag())).c_str());
  dl->AddText({tx, o.y + 12 + 54}, pal.text,
              strf("VSWR %.2f   RL %.1f dB", vb, fdtd::returnLossDb(s.s11[best])).c_str());
  dl->AddText({tx, o.y + 12 + 86}, pal.dim, strf("Chart normalised to %.0f ohm.", z0).c_str());
  dl->AddText({tx, o.y + 12 + 104}, pal.dim, "Centre = matched; right edge = open;");
  dl->AddText({tx, o.y + 12 + 122}, pal.dim, "left edge = short; upper half = inductive.");
  dl->AddText({tx, o.y + 12 + 140}, pal.warn, "orange dots: ends of the band");
  noteText("Smith chart of S11 across the sweep. " + summary + ". The chart is normalised to " +
               strf("%.0f", z0) + " ohm; the centre is a perfect match.",
           viz::access::Role::Image);
  ImGui::Dummy({w, h});
}

void Ui::drawImpedancePlot(float w, float h) {
  fdtd::SParam1 s;
  {
    std::lock_guard<std::mutex> lock(mu_);
    s = liveS11_;
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetCursorScreenPos();
  dl->AddRectFilled(o, {o.x + w, o.y + h}, pal.plotBg);
  const double z0 = prepared_ ? prepared_->plan.portImpedanceOhm : 50.0;
  const auto& plan = prepared_->plan;
  const double f0 = plan.sweepHz[0], f1 = plan.sweepHz[1];
  const float L = 70, R = 20, gap = 30;
  const float plotH = (h - 2 * gap - 50) / 2;
  struct Panel {
    ImVec2 p0, p1;
  };
  const Panel top{{o.x + L, o.y + 24}, {o.x + w - R, o.y + 24 + plotH}};
  const Panel bottom{{o.x + L, o.y + 24 + plotH + gap + 10}, {o.x + w - R, o.y + 24 + 2 * plotH + gap + 10}};
  auto X = [&](const Panel& p, double f) {
    return p.p0.x + static_cast<float>((f - f0) / (f1 - f0)) * (p.p1.x - p.p0.x);
  };
  // top: resistance and reactance (ohm), clipped to +/- 5 z0 so one resonance does not flatten the rest
  const double lim = 5 * z0;
  auto Yz = [&](double ohm) {
    return top.p1.y -
           static_cast<float>((std::clamp(ohm, -lim, lim) + lim) / (2 * lim)) * (top.p1.y - top.p0.y);
  };
  dl->AddRectFilled({X(top, plan.bandHz[0]), top.p0.y}, {X(top, plan.bandHz[1]), top.p1.y},
                    alpha(pal.curve, 40));
  for (const double v : {-lim, -z0, 0.0, z0, lim}) {
    dl->AddLine({top.p0.x, Yz(v)}, {top.p1.x, Yz(v)}, v == 0 ? pal.dim : pal.gridLine);
    dl->AddText({o.x + 10, Yz(v) - 7}, pal.dim, strf("%.0f", v).c_str());
  }
  std::vector<ImVec2> rl, xl;
  for (std::size_t i = 0; i < s.s11.size(); ++i) {
    const auto z = fdtd::impedanceFromGamma(s.s11[i], z0);
    rl.push_back({X(top, s.frequenciesHz[i]), Yz(z.real())});
    xl.push_back({X(top, s.frequenciesHz[i]), Yz(z.imag())});
  }
  dl->AddPolyline(rl.data(), static_cast<int>(rl.size()), pal.curve, 0, 2.5f);
  dl->AddPolyline(xl.data(), static_cast<int>(xl.size()), pal.warn, 0, 2.5f);
  dl->AddText({top.p0.x, o.y + 4}, pal.curve, "R (resistance, ohm)");
  dl->AddText({top.p0.x + 190, o.y + 4}, pal.warn, "X (reactance, ohm; above 0 is inductive)");
  // bottom: VSWR up to 10, with the 2:1 line
  const double vmax = 10;
  auto Yv = [&](double v) {
    return bottom.p1.y -
           static_cast<float>((std::clamp(v, 1.0, vmax) - 1) / (vmax - 1)) * (bottom.p1.y - bottom.p0.y);
  };
  dl->AddRectFilled({X(bottom, plan.bandHz[0]), bottom.p0.y}, {X(bottom, plan.bandHz[1]), bottom.p1.y},
                    alpha(pal.curve, 40));
  for (const double v : {1.0, 2.0, 3.0, 5.0, 10.0}) {
    dl->AddLine({bottom.p0.x, Yv(v)}, {bottom.p1.x, Yv(v)}, pal.gridLine);
    dl->AddText({o.x + 24, Yv(v) - 7}, pal.dim, strf("%g", v).c_str());
  }
  for (float x = bottom.p0.x; x < bottom.p1.x; x += 12)
    dl->AddLine({x, Yv(2)}, {std::min(x + 6, bottom.p1.x), Yv(2)}, pal.good, 1.5f);
  std::vector<ImVec2> vl;
  for (std::size_t i = 0; i < s.s11.size(); ++i)
    vl.push_back({X(bottom, s.frequenciesHz[i]), Yv(fdtd::vswr(s.s11[i]))});
  dl->AddPolyline(vl.data(), static_cast<int>(vl.size()), pal.curve, 0, 2.5f);
  dl->AddText({bottom.p0.x, bottom.p0.y - 18}, pal.curve, "VSWR (1 = perfect; dashed: 2, a common limit)");
  for (int i = 0; i <= 4; ++i) {
    const double f = f0 + (f1 - f0) * i / 4.0;
    dl->AddText({X(bottom, f) - 14, bottom.p1.y + 6}, pal.dim, ghz(f).c_str());
  }
  dl->AddText({o.x + w / 2 - 40, bottom.p1.y + 22}, pal.dim, "frequency (GHz)");
  std::size_t best = 0;
  for (std::size_t i = 1; i < s.s11.size(); ++i)
    if (std::abs(s.s11[i]) < std::abs(s.s11[best]))
      best = i;
  const auto zb = fdtd::impedanceFromGamma(s.s11[best], z0);
  noteText(
      strf("Plots of the input resistance and reactance, and of the VSWR, against frequency, %.3f to %.3f "
           "GHz. At the best match, %.3f GHz: %.1f %s j%.1f ohm, VSWR %.2f.",
           s.frequenciesHz.front() * 1e-9, s.frequenciesHz.back() * 1e-9, s.frequenciesHz[best] * 1e-9,
           zb.real(), zb.imag() < 0 ? "minus" : "plus", std::abs(zb.imag()), fdtd::vswr(s.s11[best])),
      viz::access::Role::Image);
  ImGui::Dummy({w, h});
}

// Gain pattern cuts and the numbers an antenna datasheet gives. The pattern comes from the Huygens box around
// the board; "realised gain" includes the radiation efficiency and the mismatch at the port, "directivity"
// is the shape alone.
void Ui::drawFarFieldTab(float w, float h) {
  std::vector<FarResult> results;
  {
    std::lock_guard<std::mutex> lock(mu_);
    results = farResults_;
  }
  if (results.empty()) {
    ImGui::TextColored(ImColor(pal.dim), "%s",
                       !ntffNote_.empty() ? ntffNote_.c_str()
                       : screen_ == Screen::Running
                           ? "The far-field pattern is computed when the run has finished."
                           : "The far-field pattern appears here after a run.");
    return;
  }
  farFreq_ = std::clamp(farFreq_, 0, static_cast<int>(results.size()) - 1);
  int k = farFreq_;
  ImGui::SetNextItemWidth(px(220));
  if (sliderInt("Frequency", "##farf", &k, 0, static_cast<int>(results.size()) - 1, "frequency %d"))
    farFreq_ = k;
  const FarResult& r = results[static_cast<std::size_t>(farFreq_)];
  ImGui::SameLine();
  coloured(ImGui::GetStyleColorVec4(ImGuiCol_Text), strf("%.4g GHz", r.frequencyHz * 1e-9));
  ImGui::SameLine();
  checkbox("realised gain (off: directivity)", &farRealised_);
  const auto& f = r.field;
  const auto& a = r.figures;
  const double scaleLin = farRealised_ ? a.realisedGain : a.directivity; // pattern: scale * U / U_peak
  auto dbi = [&](std::size_t it, std::size_t ip) {
    return 10 * std::log10(std::max(scaleLin * f.u[it * f.nPhi + ip] / std::max(f.peak, 1e-300), 1e-6));
  };
  const double top = std::ceil(10 * std::log10(std::max(scaleLin, 1e-6)) / 5) * 5, range = 30;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetCursorScreenPos();
  dl->AddRectFilled(o, {o.x + w, o.y + h - ImGui::GetFrameHeightWithSpacing()}, pal.plotBg);
  const float colW = std::min((w - 40) / 3, (h - 190) * 1.0f);
  const float radius = std::max(40.0f, 0.5f * colW - 20);
  struct Cut {
    const char* title;
    std::function<double(double)> at; // angle in radians (0 = up/right) -> dBi
  };
  auto thetaIndex = [&](double th) {
    return std::min(f.nTheta - 1, static_cast<std::size_t>(std::max(0.0, th) / std::numbers::pi *
                                                           static_cast<double>(f.nTheta)));
  };
  auto phiIndex = [&](double ph) {
    ph = std::fmod(std::fmod(ph, 2 * std::numbers::pi) + 2 * std::numbers::pi, 2 * std::numbers::pi);
    return std::min(f.nPhi - 1,
                    static_cast<std::size_t>(ph / (2 * std::numbers::pi) * static_cast<double>(f.nPhi)));
  };
  // vertical cuts: angle from +z, positive to the right (phi = 0 or 90), negative to the left (phi + 180)
  auto vertical = [&](double phi) {
    return [&, phi](double ang) {
      const bool right = ang >= 0;
      const double th = std::abs(ang);
      return dbi(thetaIndex(th), phiIndex(right ? phi : phi + std::numbers::pi));
    };
  };
  const Cut cuts[3] = {{"x-z plane (phi = 0)", vertical(0)},
                       {"y-z plane (phi = 90 deg)", vertical(std::numbers::pi / 2)},
                       {"x-y plane (theta = 90 deg), from +x",
                        [&](double ang) { return dbi(thetaIndex(std::numbers::pi / 2), phiIndex(ang)); }}};
  for (int c = 0; c < 3; ++c) {
    const ImVec2 centre{o.x + 20 + colW * (static_cast<float>(c) + 0.5f), o.y + 40 + radius};
    dl->AddText({centre.x - radius, o.y + 4}, pal.text, cuts[c].title);
    for (double ring = 0; ring <= range; ring += 10) {
      const float rr = static_cast<float>((range - ring) / range) * radius;
      dl->AddCircle(centre, std::max(rr, 1.0f), pal.gridLine, 64);
      dl->AddText({centre.x + 3, centre.y - rr - 12}, pal.dim, strf("%.0f", top - ring).c_str());
    }
    for (int d = 0; d < 4; ++d) {
      const double ang = d * std::numbers::pi / 4;
      dl->AddLine({centre.x - radius * static_cast<float>(std::sin(ang)),
                   centre.y - radius * static_cast<float>(std::cos(ang))},
                  {centre.x + radius * static_cast<float>(std::sin(ang)),
                   centre.y + radius * static_cast<float>(std::cos(ang))},
                  pal.gridLine);
    }
    std::vector<ImVec2> pts;
    for (int i = 0; i <= 180; ++i) {
      const double ang = -std::numbers::pi + 2 * std::numbers::pi * i / 180.0; // -pi .. pi
      const double v =
          std::clamp((cuts[c].at(c == 2 ? ang + std::numbers::pi : ang) - (top - range)) / range, 0.0, 1.0);
      const float rr = static_cast<float>(v) * radius;
      // vertical cuts: 0 is up (+z); the x-y cut: 0 is to the right (+x), counter-clockwise
      const float x = c == 2 ? centre.x + rr * static_cast<float>(std::cos(ang + std::numbers::pi))
                             : centre.x + rr * static_cast<float>(std::sin(ang));
      const float y = c == 2 ? centre.y - rr * static_cast<float>(std::sin(ang + std::numbers::pi))
                             : centre.y - rr * static_cast<float>(std::cos(ang));
      pts.push_back({x, y});
    }
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), pal.curve, ImDrawFlags_Closed, 2.5f);
    dl->AddText({centre.x - 8, centre.y - radius - 14}, pal.dim, c == 2 ? "+y" : "+z");
  }
  const float ty = o.y + 40 + 2 * radius + 24;
  dl->AddText(
      {o.x + 20, ty}, pal.text,
      strf("Peak %s %.2f dBi at theta %.0f deg, phi %.0f deg   |   directivity %.2f dBi   |   radiation "
           "efficiency %.0f %%   |   S11 %.1f dB",
           farRealised_ ? "realised gain" : "directivity", 10 * std::log10(std::max(scaleLin, 1e-6)),
           a.peakThetaDeg, a.peakPhiDeg, 10 * std::log10(std::max(a.directivity, 1e-6)), 100 * a.efficiency,
           r.s11Db)
          .c_str());
  dl->AddText({o.x + 20, ty + 20}, pal.dim,
              strf("Gain %.2f dBi (referenced to the accepted power); realised gain %.2f dBi includes the "
                   "mismatch. Pattern rings every 10 dB below the top (%.0f dBi). Only losses inside the "
                   "model count towards efficiency (not copper roughness, connectors, or a missing "
                   "enclosure).",
                   10 * std::log10(std::max(a.gain, 1e-6)), 10 * std::log10(std::max(a.realisedGain, 1e-6)),
                   top)
                  .c_str());
  noteText(
      strf("Far-field gain pattern at %.4g GHz in three cuts. Peak %s %.2f dBi at theta %.0f degrees, phi "
           "%.0f degrees. Directivity %.2f dBi. Radiation efficiency %.0f percent. S11 %.1f dB.",
           r.frequencyHz * 1e-9, farRealised_ ? "realised gain" : "directivity",
           10 * std::log10(std::max(scaleLin, 1e-6)), a.peakThetaDeg, a.peakPhiDeg,
           10 * std::log10(std::max(a.directivity, 1e-6)), 100 * a.efficiency, r.s11Db),
      viz::access::Role::Image);
  ImGui::Dummy({w, h - ImGui::GetFrameHeightWithSpacing()});
}

// Sweeps: one run per value of one setting (or "geom.NET.x" for a geometry stretch), each in its own results
// folder; the S11 curves are drawn over each other as the points finish.
void Ui::startSweep() {
  if (sweepRunning_ || screen_ == Screen::Running)
    return;
  app::SweepSpec spec;
  try {
    spec.key = sweepKey_;
    spec.values = app::parseSweepValues(sweepValues_);
    if (spec.key.rfind("geom.", 0) != 0)
      env_.settings.def(spec.key); // throws for an unknown key
  } catch (const std::exception& e) {
    sweepStatus_ = e.what();
    return;
  }
  if (sweepWorker_.joinable())
    sweepWorker_.join();
  {
    std::lock_guard<std::mutex> lock(mu_);
    sweepPoints_.clear();
  }
  sweepLabelKey_ = spec.key;
  sweepStatus_ = "Running...";
  sweepRunning_ = true;
  backend_ = env_.chooseBackend(env_.settings.text("backend"));
  const auto stamp = app::timestamp();
  sweepWorker_ = std::thread([this, spec, stamp] {
    try {
      const board::Snapshot snap = snapshot_;
      const board::Selection selection = env_.selection;
      const app::Settings base = env_.settings;
      app::SweepEnv senv{snap,      env_.snapshotPath, env_.catalog, env_.paths,
                         selection, backend_.factory,  backend_.name};
      senv.gpu = backend_.gpu;
      senv.deviceBytes = backend_.deviceBytes;
      senv.stamp = stamp;
      senv.shouldStop = [&] { return stop_.load(); };
      senv.progress = [&](std::size_t i, std::size_t n, const app::SweepPoint* done) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!done)
          sweepStatus_ =
              strf("Point %zu of %zu: %s = %s ...", i + 1, n, spec.key.c_str(), spec.values[i].c_str());
        else
          sweepPoints_.push_back(*done);
      };
      app::runSweep(senv, base, spec);
      std::lock_guard<std::mutex> lock(mu_);
      sweepStatus_ = strf("Sweep done: %zu points in %s", spec.values.size(),
                          (env_.paths.resultsRoot / ("sweep-" + stamp)).string().c_str());
    } catch (const std::exception& e) {
      std::lock_guard<std::mutex> lock(mu_);
      sweepStatus_ = std::string("Sweep failed: ") + e.what();
    }
    sweepRunning_ = false;
  });
}

void Ui::drawSweepPanel() {
  if (!ImGui::CollapsingHeader("Sweep a setting (optional)"))
    return;
  wrapped("Run the board once for each value of one setting and compare the S11 curves (the S11 tab gets a "
          "Sweep overlay view). Use a setting name from the Settings list (for example mesh_preset, "
          "fine_cell_mm, cells_per_wavelength, diel1_epsilon_r), or geom.NET.x or geom.NET.y to stretch a "
          "net's copper by that many millimetres (the board file is never changed). Values: a list "
          "(coarse,medium) or a range (start:stop:count). Each point keeps its own results folder.");
  char key[64], values[96];
  std::snprintf(key, sizeof key, "%s", sweepKey_.c_str());
  std::snprintf(values, sizeof values, "%s", sweepValues_.c_str());
  ImGui::SetNextItemWidth(px(220));
  if (ImGui::InputTextWithHint("##swkey", "setting (mesh_preset)", key, sizeof key))
    sweepKey_ = key;
  note(viz::access::Role::TextInput, "Setting to sweep", sweepKey_, "A setting name, or geom.NET.x");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(px(260));
  if (ImGui::InputTextWithHint("##swvals", "values (a,b,c or start:stop:count)", values, sizeof values))
    sweepValues_ = values;
  note(viz::access::Role::TextInput, "Values to sweep", sweepValues_,
       "A list a,b,c or a range start:stop:count");
  ImGui::SameLine();
  const bool busy = sweepRunning_ || screen_ == Screen::Running;
  if (busy)
    ImGui::BeginDisabled();
  if (button("Run sweep"))
    startSweep();
  if (busy)
    ImGui::EndDisabled();
  std::string status;
  {
    std::lock_guard<std::mutex> lock(mu_);
    status = sweepStatus_;
  }
  if (!status.empty())
    ImGui::TextColored(ImColor(pal.dim), "%s", status.c_str());
}

void Ui::drawSweepOverlay(float w, float h) {
  std::vector<app::SweepPoint> pts;
  {
    std::lock_guard<std::mutex> lock(mu_);
    pts = sweepPoints_;
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetCursorScreenPos();
  const float L = 60, B = 40, T = 10, R = 260;
  const ImVec2 p0{o.x + L, o.y + T}, p1{o.x + w - R, o.y + h - B};
  dl->AddRectFilled(o, {o.x + w, o.y + h}, pal.plotBg);
  const auto& plan = prepared_ ? prepared_->plan : app::RunPlan{};
  double f0 = 1e30, f1 = 0;
  for (const auto& p : pts)
    for (double f : p.s.frequenciesHz) {
      f0 = std::min(f0, f);
      f1 = std::max(f1, f);
    }
  if (!(f1 > f0)) {
    dl->AddText({o.x + 14, o.y + 14}, pal.dim, "No sweep point has finished yet.");
    ImGui::Dummy({w, h});
    return;
  }
  double lo = -30;
  for (const auto& p : pts)
    for (const auto& c : p.s.s11)
      lo = std::min(lo, 20 * std::log10(std::max(std::abs(c), 1e-6)) - 2);
  lo = std::floor(lo / 10) * 10;
  auto X = [&](double f) { return p0.x + static_cast<float>((f - f0) / (f1 - f0)) * (p1.x - p0.x); };
  auto Y = [&](double db) { return p0.y + static_cast<float>((0 - db) / (0 - lo)) * (p1.y - p0.y); };
  if (plan.bandHz[1] > plan.bandHz[0])
    dl->AddRectFilled({X(plan.bandHz[0]), p0.y}, {X(plan.bandHz[1]), p1.y}, alpha(pal.curve, 30));
  for (double db = 0; db >= lo; db -= 10) {
    dl->AddLine({p0.x, Y(db)}, {p1.x, Y(db)}, pal.gridLine);
    dl->AddText({o.x + 24, Y(db) - 7}, pal.dim, strf("%.0f", db).c_str());
  }
  for (int i = 0; i <= 4; ++i) {
    const double f = f0 + (f1 - f0) * i / 4.0;
    dl->AddText({X(f) - 14, p1.y + 6}, pal.dim, ghz(f).c_str());
  }
  dl->AddText({o.x + (w - R) / 2 - 60, p1.y + 22}, pal.dim, "frequency (GHz)    |S11| (dB)");
  // Okabe-Ito colours (colour-blind safe) and a dash pattern per curve, so the legend does not rely on colour
  static const ImU32 colours[8] = {IM_COL32(0, 114, 178, 255),  IM_COL32(213, 94, 0, 255),
                                   IM_COL32(0, 158, 115, 255),  IM_COL32(204, 121, 167, 255),
                                   IM_COL32(230, 159, 0, 255),  IM_COL32(86, 180, 233, 255),
                                   IM_COL32(240, 228, 66, 255), IM_COL32(200, 200, 200, 255)};
  float legendY = o.y + T + 4;
  dl->AddText({p1.x + 14, legendY}, pal.text, sweepLabelKey_.c_str());
  legendY += 20;
  std::string spoken;
  for (std::size_t n = 0; n < pts.size(); ++n) {
    const auto& p = pts[n];
    const ImU32 col = colours[n % 8];
    if (!p.error.empty()) {
      dl->AddText({p1.x + 14, legendY}, pal.bad, strf("%s: failed", p.value.c_str()).c_str());
      legendY += 18;
      spoken += strf("%s failed. ", p.value.c_str());
      continue;
    }
    std::vector<ImVec2> line;
    for (std::size_t i = 0; i < p.s.s11.size(); ++i)
      line.push_back(
          {X(p.s.frequenciesHz[i]), Y(std::max(20 * std::log10(std::max(std::abs(p.s.s11[i]), 1e-6)), lo))});
    // dashed for every other curve
    if (n % 2 == 0) {
      dl->AddPolyline(line.data(), static_cast<int>(line.size()), col, 0, 2.5f);
    } else {
      for (std::size_t i = 0; i + 1 < line.size(); i += 2)
        dl->AddLine(line[i], line[i + 1], col, 2.5f);
    }
    dl->AddLine({p1.x + 14, legendY + 7}, {p1.x + 40, legendY + 7}, col, 2.5f);
    dl->AddText({p1.x + 46, legendY}, pal.text,
                strf("%s: %.1f dB at %.3f GHz", p.value.c_str(), p.bestDb, p.bestHz * 1e-9).c_str());
    spoken += strf("%s = %s: best match %.1f dB at %.3f GHz. ", sweepLabelKey_.c_str(), p.value.c_str(),
                   p.bestDb, p.bestHz * 1e-9);
    legendY += 18;
  }
  noteText("Overlay of |S11| in dB for each swept value of " + sweepLabelKey_ + ". " + spoken,
           viz::access::Role::Image);
  ImGui::Dummy({w, h});
}

// Probes: points where E is recorded over the run as a spectrum (|E| against frequency).
void Ui::drawProbePanel() {
  if (!ImGui::CollapsingHeader("Field probes (optional)"))
    return;
  wrapped(
      "Points where the electric field is recorded during the run, as a spectrum: x,y,z in millimetres in "
      "the board's coordinates, z above the bottom copper plane; separate probes with a semicolon (at "
      "most 16). The spectra appear in a Probes tab and are saved as probes.csv in the results folder.");
  char buf[200];
  std::snprintf(buf, sizeof buf, "%s", probeText_.c_str());
  ImGui::SetNextItemWidth(px(420));
  if (ImGui::InputTextWithHint("##probes", "x,y,z; x,y,z  (mm)", buf, sizeof buf)) {
    probeText_ = buf;
    dirty_ = true;
    lastEdit_ = now_;
  }
  note(viz::access::Role::TextInput, "Probe points", probeText_,
       "x,y,z in millimetres, separated by semicolons");
  if (!probeStatus_.empty())
    ImGui::TextColored(ImColor(pal.bad), "%s", probeStatus_.c_str());
}

void Ui::drawProbesTab(float w, float h) {
  std::vector<std::vector<double>> db(probes_.size());
  std::vector<double> peakHz(probes_.size(), 0.0);
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (std::size_t p = 0; p < probes_.size(); ++p)
      for (std::size_t k = 0; k < probeFreqs_.size(); ++k)
        db[p].push_back(20 *
                        std::log10(std::max(static_cast<double>(std::abs(probeAcc_->phasor(k, p))), 1e-30)));
  }
  double top = -1e30;
  for (const auto& v : db)
    for (double x : v)
      top = std::max(top, x);
  if (!(top > -1e29)) {
    ImGui::TextColored(ImColor(pal.dim), "The probe spectra appear here as the run proceeds.");
    return;
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetCursorScreenPos();
  const float L = 60, B = 40, T = 10, R = 330;
  const ImVec2 p0{o.x + L, o.y + T}, p1{o.x + w - R, o.y + h - B};
  dl->AddRectFilled(o, {o.x + w, o.y + h}, pal.plotBg);
  const double f0 = probeFreqs_.front(), f1 = probeFreqs_.back(), range = 60;
  auto X = [&](double f) { return p0.x + static_cast<float>((f - f0) / (f1 - f0)) * (p1.x - p0.x); };
  auto Y = [&](double d) { return p0.y + static_cast<float>((top - d) / range) * (p1.y - p0.y); };
  const auto& plan = prepared_->plan;
  dl->AddRectFilled({X(plan.bandHz[0]), p0.y}, {X(plan.bandHz[1]), p1.y}, alpha(pal.curve, 30));
  for (double d = 0; d <= range; d += 10) {
    dl->AddLine({p0.x, Y(top - d)}, {p1.x, Y(top - d)}, pal.gridLine);
    dl->AddText({o.x + 8, Y(top - d) - 7}, pal.dim, strf("%.0f", -d).c_str());
  }
  for (int i = 0; i <= 4; ++i) {
    const double f = f0 + (f1 - f0) * i / 4.0;
    dl->AddText({X(f) - 14, p1.y + 6}, pal.dim, ghz(f).c_str());
  }
  dl->AddText({o.x + (w - R) / 2 - 90, p1.y + 22}, pal.dim,
              "frequency (GHz)    |E| at the probe (dB, 0 = the strongest value of any probe)");
  static const ImU32 colours[8] = {IM_COL32(0, 114, 178, 255),  IM_COL32(213, 94, 0, 255),
                                   IM_COL32(0, 158, 115, 255),  IM_COL32(204, 121, 167, 255),
                                   IM_COL32(230, 159, 0, 255),  IM_COL32(86, 180, 233, 255),
                                   IM_COL32(240, 228, 66, 255), IM_COL32(200, 200, 200, 255)};
  float ly = o.y + T + 4;
  std::string spoken;
  for (std::size_t p = 0; p < db.size(); ++p) {
    std::vector<ImVec2> line;
    std::size_t best = 0;
    for (std::size_t k = 0; k < db[p].size(); ++k) {
      line.push_back({X(probeFreqs_[k]), Y(std::max(db[p][k], top - range))});
      if (db[p][k] > db[p][best])
        best = k;
    }
    peakHz[p] = probeFreqs_[best];
    const ImU32 col = colours[p % 8];
    if (p % 2 == 0)
      dl->AddPolyline(line.data(), static_cast<int>(line.size()), col, 0, 2.5f);
    else
      for (std::size_t i = 0; i + 1 < line.size(); i += 2)
        dl->AddLine(line[i], line[i + 1], col, 2.5f);
    dl->AddLine({p1.x + 14, ly + 7}, {p1.x + 40, ly + 7}, col, 2.5f);
    const std::string label = strf("(%.4g, %.4g, %.4g) mm: peak %.3f GHz", probes_[p].xMm, probes_[p].yMm,
                                   probes_[p].zMm, peakHz[p] * 1e-9);
    dl->AddText({p1.x + 46, ly}, pal.text, label.c_str());
    spoken += strf("Probe %zu at %.4g, %.4g, %.4g millimetres: strongest at %.3f GHz. ", p + 1,
                   probes_[p].xMm, probes_[p].yMm, probes_[p].zMm, peakHz[p] * 1e-9);
    ly += 18;
  }
  noteText("Electric field strength at each probe against frequency. " + spoken, viz::access::Role::Image);
  ImGui::Dummy({w, h});
}

// Writes the 3D view's volume for ParaView (legacy VTK, with the phasor's magnitude, real and imaginary
// parts) and, if the isosurface is on, its mesh as OBJ, into the results folder.
void Ui::exportVolume() {
  if (!volPhasors_ || runDir_.empty()) {
    message_ = "Run the simulation first: there is no volume to export.";
    return;
  }
  try {
    const auto dir = runDir_ / "export";
    std::filesystem::create_directories(dir);
    const double fGhz = volPhasors_->frequencyHz(freq_) * 1e-9;
    const std::string stem = strf("volume-%.4gGHz", fGhz);
    std::vector<float> mag(dv_.voxels()), re(dv_.voxels()), im(dv_.voxels());
    {
      std::lock_guard<std::mutex> lock(mu_);
      for (std::size_t v = 0; v < mag.size(); ++v) {
        const auto x = volPhasors_->phasor(freq_, v);
        mag[v] = std::abs(x);
        re[v] = x.real();
        im[v] = x.imag();
      }
    }
    const auto& m = prepared_->model;
    std::size_t kBottomNode = m.grid.nodes(fdtd::Z);
    for (std::size_t i = 0; i < snapshot_.stackup.size(); ++i)
      if (snapshot_.stackup[i].copper)
        kBottomNode = std::min(kBottomNode, m.copperPlaneNode[i]);
    const std::array<double, 3> shift{m.originX, m.originY, -m.grid.edge(fdtd::Z, kBottomNode)};
    std::ofstream out(dir / (stem + ".vtk"));
    viz::writeVtkVolume(out, dv_, {{"E_abs", mag}, {"E_re", re}, {"E_im", im}},
                        strf("kicad-em-fdtd %s field at %.4g GHz, metres: x and y in board coordinates, z "
                             "above the bottom copper",
                             env_.settings.text("field_component").c_str(), fGhz),
                        shift);
    std::string what = stem + ".vtk";
    if (showIso_ && !isoMesh_.empty()) {
      std::ofstream obj(dir / (stem + "-isosurface.obj"));
      viz::writeObj(obj, isoMesh_, shift);
      what += " and " + stem + "-isosurface.obj";
    }
    message_ = "Exported " + what + " to " + dir.string();
  } catch (const std::exception& e) {
    message_ = std::string("Export failed: ") + e.what();
  }
}

void Ui::refreshIsosurface() {
  if (!volPhasors_ || volMag_.size() != dv_.voxels())
    return;
  if (isoVersion_ == statsVersion_ && isoFreq_ == freq_ && isoLevelBuilt_ == isoLevel_ && !isoMesh_.empty())
    return;
  isoMesh_ = viz::isosurface(dv_, volMag_, static_cast<float>(isoLevel_ * volMagScale_));
  isoVersion_ = statsVersion_;
  isoFreq_ = freq_;
  isoLevelBuilt_ = isoLevel_;
}

void Ui::refreshCurrent() {
  if (!currentH_ || (currentVersion_ == phasorVersion_ && currentFreq_ == freq_ && !currentJ_.empty()))
    return;
  std::lock_guard<std::mutex> lock(mu_);
  currentJ_.resize(currentList_.cells.size());
  std::vector<float> mags;
  mags.reserve(currentJ_.size());
  for (std::size_t c = 0; c < currentJ_.size(); ++c) {
    currentJ_[c] = fdtd::surfaceCurrent(currentList_.cells[c], currentH_->phasor(freq_, 2 * c),
                                        currentH_->phasor(freq_, 2 * c + 1));
    mags.push_back(static_cast<float>(std::abs(currentJ_[c])));
  }
  const std::size_t nth = static_cast<std::size_t>(0.92 * static_cast<double>(mags.size() - 1));
  std::nth_element(mags.begin(), mags.begin() + static_cast<std::ptrdiff_t>(nth), mags.end());
  currentScale_ = std::max(static_cast<double>(mags[nth]), 1e-30);
  currentVersion_ = phasorVersion_;
  currentFreq_ = freq_;
}

void Ui::drawCurrent(ImDrawList* dl, const std::function<bool(double, double, double, ImVec2&)>& project,
                     bool light, std::size_t onlyK) {
  refreshCurrent();
  if (currentJ_.empty())
    return;
  const fdtd::Grid& g = prepared_->model.grid;
  const viz::Rgb neg = fieldColour(light, false), pos = fieldColour(light, true);
  const std::complex<double> rot = std::polar(1.0, phase_);
  for (std::size_t c = 0; c < currentJ_.size(); ++c) {
    const fdtd::SurfaceCurrentCell& cell = currentList_.cells[c];
    if (onlyK != static_cast<std::size_t>(-1) && cell.k != onlyK)
      continue;
    const double mag = std::abs(currentJ_[c]) / currentScale_;
    if (mag < 0.04)
      continue;
    // the signed component along the cell's direction at the animation phase shared with the field layers,
    // so both animate together and freeze together when paused
    const double v = (currentJ_[c] * rot).real() / currentScale_;
    const double strength = std::min(1.0, std::abs(v));
    if (strength < 0.04)
      continue;
    const viz::Rgb col = v >= 0 ? pos : neg;
    const double x0 = cell.direction == fdtd::X ? g.edge(fdtd::X, cell.i)
                                                : g.edge(fdtd::X, cell.i) - 0.5 * g.dualSize(fdtd::X, cell.i);
    const double x1 = cell.direction == fdtd::X ? g.edge(fdtd::X, cell.i + 1)
                                                : g.edge(fdtd::X, cell.i) + 0.5 * g.dualSize(fdtd::X, cell.i);
    const double y0 = cell.direction == fdtd::Y ? g.edge(fdtd::Y, cell.j)
                                                : g.edge(fdtd::Y, cell.j) - 0.5 * g.dualSize(fdtd::Y, cell.j);
    const double y1 = cell.direction == fdtd::Y ? g.edge(fdtd::Y, cell.j + 1)
                                                : g.edge(fdtd::Y, cell.j) + 0.5 * g.dualSize(fdtd::Y, cell.j);
    const double z = g.edge(fdtd::Z, cell.k);
    ImVec2 a, b, d, e;
    if (project(x0, y0, z, a) && project(x1, y0, z, b) && project(x1, y1, z, d) && project(x0, y1, z, e))
      dl->AddQuadFilled(a, b, d, e, IM_COL32(col.r, col.g, col.b, static_cast<int>(60 + 195 * strength)));
  }
}

// Phase of the animation, shared by the field slice, the 3D volume and the surface current: while it plays,
// its speed; while it is paused, a slider to scrub through one cycle.
void Ui::animationControl() {
  ImGui::SetNextItemWidth(px(150));
  if (paused_) {
    float deg = static_cast<float>(
        std::fmod(std::fmod(phase_, 2 * std::numbers::pi) + 2 * std::numbers::pi, 2 * std::numbers::pi) *
        180 / std::numbers::pi);
    if (sliderFloat("Phase", "##phase", &deg, 0.0f, 360.0f, "phase %.0f deg"))
      phase_ = static_cast<double>(deg) * std::numbers::pi / 180;
  } else {
    float speed = static_cast<float>(animCyclesPerSecond_);
    if (sliderFloat("Animation speed", "##speed", &speed, 0.05f, 2.0f, "%.2f cycles/s"))
      animCyclesPerSecond_ = static_cast<double>(speed);
  }
}

// The colour table chosen in View, as a viz table, or empty for the two-colour signed view.
std::vector<viz::Rgb> Ui::volumeLut() const {
  const std::string id = env_.settings.text("volume_colour_map");
  const auto it = env_.catalog.colourMaps().find(id);
  std::vector<viz::Rgb> lut;
  if (it == env_.catalog.colourMaps().end())
    return lut;
  for (const auto& c : it->second.colours)
    lut.push_back({c[0], c[1], c[2]});
  return lut;
}

void Ui::refreshPoynting() {
  if (!poyntingH_ || (poyntingVersion_ == phasorVersion_ && poyntingFreq_ == freq_ && !poyntingS_.empty()))
    return;
  std::lock_guard<std::mutex> lock(mu_);
  const std::size_t nodes = plane_.nu * plane_.nv;
  poyntingS_.resize(nodes);
  std::vector<float> lateral;
  lateral.reserve(nodes);
  for (std::size_t n = 0; n < nodes; ++n) {
    viz::Phasor3 e, h;
    for (std::size_t k = 0; k < 3; ++k) {
      e[k] = poyntingH_->phasor(freq_, 6 * n + k);
      h[k] = poyntingH_->phasor(freq_, 6 * n + 3 + k);
    }
    const auto s = viz::poynting(e, h);
    poyntingS_[n] = {static_cast<float>(s[0]), static_cast<float>(s[1]), static_cast<float>(s[2])};
    // the in-plane components, along the plane's own axes u and v
    lateral.push_back(std::hypot(static_cast<float>(s[plane_.u]), static_cast<float>(s[plane_.v])));
  }
  const std::size_t nth = static_cast<std::size_t>(0.95 * static_cast<double>(lateral.size() - 1));
  std::nth_element(lateral.begin(), lateral.begin() + static_cast<std::ptrdiff_t>(nth), lateral.end());
  poyntingScale_ = std::max(static_cast<double>(lateral[nth]), 1e-30);
  poyntingVersion_ = phasorVersion_;
  poyntingFreq_ = freq_;
}

void Ui::drawSliceTab(float w, float h) {
  const PanelColours panelColours(lightSlice_, dark_);
  if (!phasors_) {
    ImGui::TextColored(ImColor(pal.dim), "The field animation appears here once the run starts.");
    return;
  }
  if (smallButton(sliceControlsOpen_ ? "Hide controls" : "Show controls"))
    sliceControlsOpen_ = !sliceControlsOpen_;
  ImGui::SameLine();
  static float brightness =
      0.4f; // default gain: typical field is well below the 90th percentile used for the scale
  if (sliceControlsOpen_) {
    const int n = static_cast<int>(phasors_->frequencies());
    int k = static_cast<int>(freq_);
    ImGui::SetNextItemWidth(px(260));
    if (sliderInt("Frequency index", "##freq", &k, 0, n - 1, "frequency index %d"))
      freq_ = static_cast<std::size_t>(k);
    ImGui::SameLine();
    coloured(ImGui::GetStyleColorVec4(ImGuiCol_Text), strf("%.4g GHz", phasors_->frequencyHz(freq_) * 1e-9));
    ImGui::SameLine();
    checkbox("pause", &paused_);
    ImGui::SameLine();
    animationControl();
    ImGui::SameLine();
    checkbox("board outline", &showOutline_);
    ImGui::SameLine();
    checkbox("field", &showFieldSlice_);
    ImGui::SameLine();
    checkbox("power flow", &showPowerSlice_);
    if (currentH_) {
      ImGui::SameLine();
      checkbox("surface current", &showCurrentSlice_);
      if (showCurrentSlice_) {
        ImGui::SameLine();
        checkbox("bottom layer", &currentBottom_);
      }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(px(160));
    sliderFloat("Brightness", "##b", &brightness, -2.0f, 2.0f, "brightness %.1f");
  } else {
    coloured(ImGui::GetStyleColorVec4(ImGuiCol_Text), strf("%.4g GHz", phasors_->frequencyHz(freq_) * 1e-9));
  }
  scaleFactor_ = std::pow(10.0, -brightness);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  sliceRect_ = {p.x, p.y, w, h - ImGui::GetFrameHeightWithSpacing() - 44};
  if (gTree) {
    gTree->add(
        {.role = viz::access::Role::Image,
         .name = strf("Field slice: the %s field through the board at %.4g GHz, animated. Positive and "
                      "negative values are drawn in the two colours of the legend, strength as "
                      "brightness. Drag to pan, scroll to zoom.",
                      env_.settings.text("field_component").c_str(), phasors_->frequencyHz(freq_) * 1e-9),
         .x = p.x,
         .y = p.y,
         .w = sliceRect_.w,
         .h = sliceRect_.h});
  }
  // drag to pan, wheel to zoom, double-click to reset
  ImGui::SetNextItemAllowOverlap(); // the legends drawn over the slice must still receive their own clicks
  ImGui::InvisibleButton("##slice", {std::max(1.0f, sliceRect_.w), std::max(1.0f, sliceRect_.h)});
  {
    ImGuiIO& sio = ImGui::GetIO();
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f) && sliceMPerPx_ > 0) {
      slicePanU_ -= sio.MouseDelta.x * sliceMPerPx_;
      slicePanV_ += sio.MouseDelta.y * sliceMPerPx_; // screen y points down, v up
    }
    if (ImGui::IsItemHovered() && sio.MouseWheel != 0)
      sliceZoom_ = std::clamp(sliceZoom_ * std::pow(1.15, sio.MouseWheel), 0.5, 40.0);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      sliceZoom_ = 1;
      slicePanU_ = slicePanV_ = 0;
    }
  }
  {
    std::string component = env_.settings.text("field_component");
    component[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(component[0])));
    char title[96];
    if (!showFieldSlice_ && showCurrentSlice_ && currentScale_ > 0)
      std::snprintf(title, sizeof title, "Surface current (full colour = %.2g A/m)", currentScale_);
    else if (sliceScale_ > 0)
      std::snprintf(title, sizeof title, "%s field value (full colour = +/-%.2g, solver units)",
                    component.c_str(), sliceScale_);
    else
      std::snprintf(title, sizeof title, "%s field value", component.c_str());
    const bool light = lightSlice_;
    const viz::Rgb neg = fieldColour(light, false), pos = fieldColour(light, true);
    drawColourBar(
        ImGui::GetWindowDrawList(), {sliceRect_.x + 12, sliceRect_.y + sliceRect_.h - 78}, 300, title,
        [light, neg, pos](float t) {
          return light ? viz::divergingLight(2 * t - 1, 0.5f, neg, pos)
                       : viz::divergingDark(2 * t - 1, 0.5f, neg, pos);
        },
        "negative", "0", "positive", "legendSlice", legendOpen_[2]);
  }
  if (showOutline_ && sliceCrop_[1] > sliceCrop_[0] && snapshot_.outline.size() > 1) {
    // same fit as renderFrame: uniform scale, window centred in the rectangle
    const double lenU = sliceCrop_[1] - sliceCrop_[0], lenV = sliceCrop_[3] - sliceCrop_[2];
    const double ppm = std::min(sliceRect_.w / lenU, sliceRect_.h / lenV);
    const double offX = sliceRect_.x + 0.5 * (sliceRect_.w - lenU * ppm),
                 offY = sliceRect_.y + 0.5 * (sliceRect_.h - lenV * ppm);
    const auto& m = prepared_->model;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect({sliceRect_.x, sliceRect_.y}, {sliceRect_.x + sliceRect_.w, sliceRect_.y + sliceRect_.h},
                     true);
    std::vector<ImVec2> pts;
    for (const auto& pt : snapshot_.outline)
      pts.push_back({static_cast<float>(offX + (pt.x - m.originX - sliceCrop_[0]) * ppm),
                     static_cast<float>(offY + (sliceCrop_[3] - (pt.y - m.originY)) * ppm)});
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), pal.geoOutline, ImDrawFlags_Closed, 2.0f);
    dl->PopClipRect();
  }
  if (showPowerSlice_ && poyntingH_ && sliceCrop_[1] > sliceCrop_[0]) {
    // Time-averaged power flow as arrows on a lattice about 18 points apart: direction is the in-plane
    // direction of S, length and opacity follow |S|, full length at the 95th percentile.
    refreshPoynting();
    const double lenU = sliceCrop_[1] - sliceCrop_[0], lenV = sliceCrop_[3] - sliceCrop_[2];
    const double ppm = std::min(sliceRect_.w / lenU, sliceRect_.h / lenV);
    const double offX = sliceRect_.x + 0.5 * (sliceRect_.w - lenU * ppm),
                 offY = sliceRect_.y + 0.5 * (sliceRect_.h - lenV * ppm);
    const auto& grid = prepared_->model.grid;
    ImDrawList* pdl = ImGui::GetWindowDrawList();
    pdl->PushClipRect({sliceRect_.x, sliceRect_.y},
                      {sliceRect_.x + sliceRect_.w, sliceRect_.y + sliceRect_.h}, true);
    const float spacing = px(18);
    double lastU = -1e30, lastV = -1e30;
    for (std::size_t b = 0; b < plane_.nv; ++b) {
      const double v = grid.edge(plane_.v, b);
      if (v < sliceCrop_[2] || v > sliceCrop_[3] || (v - lastV) * ppm < spacing)
        continue;
      lastV = v;
      lastU = -1e30;
      for (std::size_t a = 0; a < plane_.nu; ++a) {
        const double u = grid.edge(plane_.u, a);
        if (u < sliceCrop_[0] || u > sliceCrop_[1] || (u - lastU) * ppm < spacing)
          continue;
        const auto& s = poyntingS_[a + plane_.nu * b];
        const double su = s[plane_.u], sw = s[plane_.v];
        const double mag = std::hypot(su, sw) / poyntingScale_;
        if (mag < 0.05)
          continue;
        lastU = u;
        const ImVec2 c{static_cast<float>(offX + (u - sliceCrop_[0]) * ppm),
                       static_cast<float>(offY + (sliceCrop_[3] - v) * ppm)};
        const double len = std::min(1.0, mag) * 0.9 * spacing;
        const ImVec2 dir{static_cast<float>(su / std::hypot(su, sw)),
                         static_cast<float>(-sw / std::hypot(su, sw))}; // screen y points down
        const ImVec2 tip{c.x + dir.x * static_cast<float>(len), c.y + dir.y * static_cast<float>(len)};
        const int a8 = static_cast<int>(110 + 145 * std::min(1.0, mag));
        const ImU32 col = lightSlice_ ? IM_COL32(20, 20, 30, a8) : IM_COL32(255, 255, 255, a8);
        pdl->AddLine(c, tip, col, 1.5f);
        const ImVec2 n{-dir.y, dir.x};
        const float head = std::min(5.0f, static_cast<float>(len) * 0.4f);
        pdl->AddTriangleFilled(
            tip, {tip.x - dir.x * head + n.x * head * 0.5f, tip.y - dir.y * head + n.y * head * 0.5f},
            {tip.x - dir.x * head - n.x * head * 0.5f, tip.y - dir.y * head - n.y * head * 0.5f}, col);
      }
    }
    pdl->PopClipRect();
  }
  if (showCurrentSlice_ && currentH_ && sliceCrop_[1] > sliceCrop_[0]) {
    const double lenU = sliceCrop_[1] - sliceCrop_[0], lenV = sliceCrop_[3] - sliceCrop_[2];
    const double ppm = std::min(sliceRect_.w / lenU, sliceRect_.h / lenV);
    const double offX = sliceRect_.x + 0.5 * (sliceRect_.w - lenU * ppm),
                 offY = sliceRect_.y + 0.5 * (sliceRect_.h - lenV * ppm);
    std::size_t layerK = currentBottom_ ? ~std::size_t(0) : 0, lo = ~std::size_t(0);
    for (std::size_t i = 0; i < snapshot_.stackup.size(); ++i)
      if (snapshot_.stackup[i].copper) {
        const std::size_t k = prepared_->model.copperPlaneNode[i];
        layerK = currentBottom_ ? std::min(layerK, k) : std::max(layerK, k);
        lo = std::min(lo, k);
      }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect({sliceRect_.x, sliceRect_.y}, {sliceRect_.x + sliceRect_.w, sliceRect_.y + sliceRect_.h},
                     true);
    drawCurrent(
        dl,
        [&](double gx, double gy, double, ImVec2& out) {
          out = {static_cast<float>(offX + (gx - sliceCrop_[0]) * ppm),
                 static_cast<float>(offY + (sliceCrop_[3] - gy) * ppm)};
          return true;
        },
        lightSlice_, layerK);
    dl->PopClipRect();
  }
  ImGui::TextColored(ImColor(pal.dim),
                     "%s field between the copper layers, animated. Drag to pan, wheel to zoom, double-click "
                     "to reset. Colours: see the bar (change them in Settings, "
                     "View); grey = copper, line = board outline (toggle \"board outline\").",
                     env_.settings.text("field_component").c_str());
}

void Ui::drawVolumeTab(float w, float h) {
  const PanelColours panelColours(lightVolume_, dark_);
  if (!volPhasors_) {
    ImGui::TextColored(ImColor(pal.dim), "The 3D volume appears here once the run starts.");
    return;
  }
  if (smallButton(volControlsOpen_ ? "Hide controls" : "Show controls"))
    volControlsOpen_ = !volControlsOpen_;
  ImGui::SameLine();
  if (volControlsOpen_) {
    const int n = static_cast<int>(volPhasors_->frequencies());
    int k = static_cast<int>(freq_);
    ImGui::SetNextItemWidth(px(220));
    if (sliderInt("Frequency index", "##vfreq", &k, 0, n - 1, "frequency index %d"))
      freq_ = static_cast<std::size_t>(k);
    ImGui::SameLine();
    coloured(ImGui::GetStyleColorVec4(ImGuiCol_Text),
             strf("%.4g GHz", volPhasors_->frequencyHz(freq_) * 1e-9));
    ImGui::SameLine();
    checkbox("pause", &paused_);
    ImGui::SameLine();
    animationControl();
    ImGui::SameLine();
    checkbox("auto-fit", &autoFit_);
    ImGui::SameLine();
    checkbox("copper", &showGeometry_);
    ImGui::SameLine();
    checkbox("field", &showField3d_);
    if (currentH_) {
      ImGui::SameLine();
      checkbox("surface current", &showCurrent3d_);
    }
    ImGui::SameLine();
    checkbox("isosurface", &showIso_);
    if (showIso_) {
      ImGui::SameLine();
      ImGui::SetNextItemWidth(px(120));
      sliderFloat("Isosurface level", "##iso", &isoLevel_, 0.05f, 2.0f, "level %.2f");
    }
    ImGui::SameLine();
    if (smallButton("Export for ParaView"))
      exportVolume();
    float b = static_cast<float>(volBrightness_), d = static_cast<float>(density_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(px(140));
    if (sliderFloat("Brightness", "##vb", &b, -2.0f, 2.0f, "brightness %.1f"))
      volBrightness_ = b;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(px(140));
    if (sliderFloat("Opacity", "##vd", &d, 0.5f, 40.0f, "opacity %.1f"))
      density_ = d;
  } else {
    coloured(ImGui::GetStyleColorVec4(ImGuiCol_Text),
             strf("%.4g GHz", volPhasors_->frequencyHz(freq_) * 1e-9));
  }
  const ImVec2 p = ImGui::GetCursorScreenPos();
  volRect_ = {p.x, p.y, w, h - ImGui::GetFrameHeightWithSpacing() - 24, false};
  if (gTree) {
    gTree->add(
        {.role = viz::access::Role::Image,
         .name = strf("3D volume: the field around the board at %.4g GHz, animated. Brighter and more "
                      "opaque is stronger. Drag to rotate, right-drag or Shift-drag to pan, scroll to "
                      "zoom, double-click to refit. The axis triad in the top-left corner (or keys 1 to 6, 0 "
                      "for isometric) snaps to the +X, -X, +Y, -Y, +Z, -Z and isometric views.",
                      volPhasors_->frequencyHz(freq_) * 1e-9),
         .x = p.x,
         .y = p.y,
         .w = volRect_.w,
         .h = volRect_.h});
  }
  // drag to rotate; right or middle drag, or Shift+drag, to pan; wheel to zoom; double-click to refit
  ImGui::SetNextItemAllowOverlap(); // the legends, the axis triad and the controls drawn over the view must
                                    // still receive their own clicks
  ImGui::InvisibleButton("##volume", {std::max(1.0f, volRect_.w), std::max(1.0f, volRect_.h)},
                         ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                             ImGuiButtonFlags_MouseButtonMiddle);
  ImGuiIO& io = ImGui::GetIO();
  if (ImGui::IsItemActive()) {
    const bool pan = ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f) ||
                     ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f) ||
                     (io.KeyShift && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f));
    if (pan) {
      viz::panCamera(dv_, cam_, io.MouseDelta.x, io.MouseDelta.y, volRect_.h);
      volRect_.dragging = true;
      autoFit_ = false;
    } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
      cam_.azimuth -= 0.008 * io.MouseDelta.x;
      cam_.elevation =
          std::clamp(cam_.elevation + 0.008 * io.MouseDelta.y, -viz::kMaxElevation, viz::kMaxElevation);
      viewGoal_.reset(); // the user took over from any glide
      volRect_.dragging = true;
      autoFit_ = false; // the user took the camera
    }
  }
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    autoFit_ = true; // back to the framed view
  if (ImGui::IsItemHovered() && io.MouseWheel != 0) {
    cam_.distance = std::clamp(cam_.distance * std::pow(0.9, io.MouseWheel), 0.25, 6.0);
    autoFit_ = false;
  }
  // Axis triad: six labelled ends that snap the camera to the +/-X, +/-Y, +/-Z views, plus an isometric
  // button. The keys 1-6 and 0 do the same when no text field has focus.
  {
    const float arm = px(34), r = px(10);
    const ImVec2 centre{volRect_.x + arm + r + px(8), volRect_.y + arm + r + px(8)};
    ImDrawList* tdl = ImGui::GetWindowDrawList();
    auto ends = viz::axisTriad(cam_.azimuth, cam_.elevation);
    std::sort(ends.begin(), ends.end(), [](const auto& a, const auto& b) { return a.depth < b.depth; });
    const ImVec2 saved = ImGui::GetCursorScreenPos();
    for (const viz::AxisEnd& e : ends) { // far ends first so near ones sit on top
      const ImVec2 q{centre.x + static_cast<float>(e.x) * arm, centre.y + static_cast<float>(e.y) * arm};
      const char axisName[2] = {e.axis, 0};
      const ImU32 base = e.axis == 'X' ? pal.geoPort : e.axis == 'Y' ? pal.geoTop : pal.geoVia;
      const float fade = e.depth > 0 ? 1.0f : 0.55f;
      tdl->AddLine(centre, q, alpha(base, static_cast<int>(200 * fade)), e.plus ? 2.0f : 1.0f);
      ImGui::SetCursorScreenPos({q.x - r, q.y - r});
      const std::string id = std::string("##view") + viz::stdViewName(e.view);
      const bool hit = ImGui::InvisibleButton(id.c_str(), {2 * r, 2 * r});
      note(viz::access::Role::Button, strf("View from %s", viz::stdViewName(e.view)), "",
           "Snap the camera to look at the board from this side");
      const bool hot = ImGui::IsItemHovered();
      if (hot)
        ImGui::SetTooltip("View from %s (key %d)", viz::stdViewName(e.view), static_cast<int>(e.view) + 1);
      tdl->AddCircleFilled(q, r, alpha(base, static_cast<int>((e.plus ? 235 : 70) * fade)));
      if (hot || ImGui::IsItemFocused())
        tdl->AddCircle(q, r + 1.5f, pal.text, 0, 1.5f);
      if (e.plus || hot) {
        const std::string label = e.plus ? std::string(axisName) : std::string("-") + axisName;
        const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
        tdl->AddText({q.x - ts.x / 2, q.y - ts.y / 2}, pal.legendText, label.c_str());
      }
      if (hit)
        viewGoal_ = viz::stdViewPose(e.view);
    }
    ImGui::SetCursorScreenPos({centre.x - arm - r, centre.y + arm + r + px(2)});
    if (smallButton("Iso", "Isometric view"))
      viewGoal_ = viz::stdViewPose(viz::StdView::Isometric);
    ImGui::SetCursorScreenPos(saved);
    if (!ImGui::GetIO().WantTextInput && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
      const viz::StdView keyed[6] = {viz::StdView::PlusX,  viz::StdView::MinusX, viz::StdView::PlusY,
                                     viz::StdView::MinusY, viz::StdView::PlusZ,  viz::StdView::MinusZ};
      for (int k = 0; k < 6; ++k)
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_1 + k), false))
          viewGoal_ = viz::stdViewPose(keyed[k]);
      if (ImGui::IsKeyPressed(ImGuiKey_0, false))
        viewGoal_ = viz::stdViewPose(viz::StdView::Isometric);
    }
    if (viewGoal_) {
      autoFit_ = false;
      const viz::Pose next =
          viz::easePose({cam_.azimuth, cam_.elevation}, *viewGoal_, reduceMotion_ ? 1.0 : 0.25);
      cam_.azimuth = next.azimuth;
      cam_.elevation = next.elevation;
      if (std::abs(next.azimuth - viewGoal_->azimuth) < 1e-4 &&
          std::abs(next.elevation - viewGoal_->elevation) < 1e-4)
        viewGoal_.reset();
    }
  }
  // outline of the board on its copper plane and of the displayed box, projected with the same camera
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float sc = ImGui::GetIO().DisplayFramebufferScale.x;
  const auto& m = prepared_->model;
  auto line3 = [&](std::array<double, 3> a, std::array<double, 3> bpt, ImU32 col) {
    float ax, ay, bx, by;
    const auto w1 = static_cast<std::uint32_t>(std::max(1.0f, volRect_.w * sc)),
               h1 = static_cast<std::uint32_t>(std::max(1.0f, volRect_.h * sc));
    if (viz::projectPoint(dv_, cam_, w1, h1, a, ax, ay) && viz::projectPoint(dv_, cam_, w1, h1, bpt, bx, by))
      dl->AddLine({volRect_.x + ax / sc, volRect_.y + ay / sc}, {volRect_.x + bx / sc, volRect_.y + by / sc},
                  col, 1.0f);
  };
  const std::array<double, 3> lo = dv_.origin,
                              hi = {dv_.origin[0] + dv_.extent[0], dv_.origin[1] + dv_.extent[1],
                                    dv_.origin[2] + dv_.extent[2]};
  for (int i = 0; i < 8; ++i)
    for (int a = 0; a < 3; ++a)
      if (!(i >> a & 1)) {
        std::array<double, 3> p0, p1;
        for (int c = 0; c < 3; ++c)
          p0[c] = p1[c] = (i >> c & 1) ? hi[c] : lo[c];
        p1[a] = hi[a];
        line3(p0, p1, pal.geoBox);
      }
  if (showIso_) {
    // The surface where the field magnitude equals the level: translucent triangles with a faint outline, the
    // same camera as everything else (no depth sort: the fill is translucent, so order does not matter much).
    refreshIsosurface();
    const auto w1 = static_cast<std::uint32_t>(std::max(1.0f, volRect_.w * sc)),
               h1 = static_cast<std::uint32_t>(std::max(1.0f, volRect_.h * sc));
    const viz::Rgb col = fieldColour(lightVolume_, true);
    const std::size_t stride =
        std::max<std::size_t>(1, isoMesh_.size() / 40000); // keep the frame time bounded
    for (std::size_t n = 0; n < isoMesh_.size(); n += stride) {
      ImVec2 q[3];
      bool ok = true;
      for (int v = 0; v < 3 && ok; ++v) {
        float qx, qy;
        const auto& vertex = isoMesh_[n].v[static_cast<std::size_t>(v)];
        ok = viz::projectPoint(dv_, cam_, w1, h1, {vertex[0], vertex[1], vertex[2]}, qx, qy);
        q[v] = {volRect_.x + qx / sc, volRect_.y + qy / sc};
      }
      if (!ok)
        continue;
      dl->AddTriangleFilled(q[0], q[1], q[2], IM_COL32(col.r, col.g, col.b, 55));
      dl->AddTriangle(q[0], q[1], q[2], IM_COL32(col.r, col.g, col.b, 110), 1.0f);
    }
  }
  if (showCurrent3d_ && currentH_) {
    const auto w1 = static_cast<std::uint32_t>(std::max(1.0f, volRect_.w * sc)),
               h1 = static_cast<std::uint32_t>(std::max(1.0f, volRect_.h * sc));
    drawCurrent(
        dl,
        [&](double gx, double gy, double gz, ImVec2& out) {
          float qx, qy;
          if (gx < dv_.origin[0] || gx > dv_.origin[0] + dv_.extent[0] || gy < dv_.origin[1] ||
              gy > dv_.origin[1] + dv_.extent[1] ||
              !viz::projectPoint(dv_, cam_, w1, h1, {gx, gy, gz}, qx, qy))
            return false;
          out = {volRect_.x + qx / sc, volRect_.y + qy / sc};
          return true;
        },
        lightVolume_, static_cast<std::size_t>(-1));
  }
  if (showGeometry_) {
    // Copper, vias, the port and the board's thickness, drawn over the volume with the same camera (no depth
    // ordering against the field: the fills are translucent and big pours are outlined only).
    auto z0 = [&](const std::string& layerName) {
      for (std::size_t i = 0; i < snapshot_.stackup.size(); ++i)
        if (snapshot_.stackup[i].copper && snapshot_.stackup[i].name == layerName)
          return m.grid.edge(fdtd::Z, m.copperPlaneNode[i]);
      return 0.0;
    };
    double zTop = -1e9, zBottom = 1e9;
    for (std::size_t i = 0; i < snapshot_.stackup.size(); ++i)
      if (snapshot_.stackup[i].copper) {
        zTop = std::max(zTop, m.grid.edge(fdtd::Z, m.copperPlaneNode[i]));
        zBottom = std::min(zBottom, m.grid.edge(fdtd::Z, m.copperPlaneNode[i]));
      }
    const std::string topName = snapshot_.stackup.empty() ? "" : snapshot_.stackup.front().name;
    auto project = [&](double x, double y, double z, ImVec2& out) {
      float px, py;
      const auto w1 = static_cast<std::uint32_t>(std::max(1.0f, volRect_.w * sc)),
                 h1 = static_cast<std::uint32_t>(std::max(1.0f, volRect_.h * sc));
      if (!viz::projectPoint(dv_, cam_, w1, h1, {x - m.originX, y - m.originY, z}, px, py))
        return false;
      out = {volRect_.x + px / sc, volRect_.y + py / sc};
      return true;
    };
    auto ring = [&](const std::vector<board::Point>& pts, double z, ImU32 line, ImU32 fill, bool filled) {
      std::vector<ImVec2> v;
      for (const auto& pt : pts) {
        ImVec2 q;
        if (!project(pt.x, pt.y, z, q))
          return;
        v.push_back(q);
      }
      if (filled && v.size() >= 3) {
        std::vector<std::array<double, 2>> flat;
        for (const ImVec2& q : v)
          flat.push_back({q.x, q.y});
        for (const auto& t : viz::triangulate(flat))
          dl->AddTriangleFilled(v[t[0]], v[t[1]], v[t[2]], fill);
      }
      dl->AddPolyline(v.data(), static_cast<int>(v.size()), line, ImDrawFlags_Closed, 1.3f);
    };
    // board body: outline at the top and bottom copper planes and the edges between them
    for (double z : {zBottom, zTop})
      ring(snapshot_.outline, z, pal.geoOutline, 0, false);
    for (const auto& pt : snapshot_.outline) {
      ImVec2 a, b;
      if (project(pt.x, pt.y, zBottom, a) && project(pt.x, pt.y, zTop, b))
        dl->AddLine(a, b, alpha(pal.geoOutline, 200), 1.0f);
    }
    // copper
    double bx0 = 1e9, bx1 = -1e9, by0 = 1e9, by1 = -1e9;
    for (const auto& pt : snapshot_.outline) {
      bx0 = std::min(bx0, pt.x);
      bx1 = std::max(bx1, pt.x);
      by0 = std::min(by0, pt.y);
      by1 = std::max(by1, pt.y);
    }
    const double boardArea = std::max((bx1 - bx0) * (by1 - by0), 1e-12);
    for (const auto& c : snapshot_.copper) {
      if (!env_.selection.includes(c.owner))
        continue;
      double x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
      for (const auto& pt : c.outer) {
        x0 = std::min(x0, pt.x);
        x1 = std::max(x1, pt.x);
        y0 = std::min(y0, pt.y);
        y1 = std::max(y1, pt.y);
      }
      const bool top = c.layer == topName, pour = (x1 - x0) * (y1 - y0) > 0.3 * boardArea;
      const ImU32 line = top ? pal.geoTop : alpha(pal.geoOther, 230);
      const ImU32 fill = top ? alpha(pal.geoTop, 90) : alpha(pal.geoOther, 50);
      ring(c.outer, z0(c.layer), line, fill, !pour);
    }
    for (const auto& v : snapshot_.vias) {
      ImVec2 a, b;
      if (project(v.x, v.y, zBottom, a) && project(v.x, v.y, zTop, b)) {
        dl->AddLine(a, b, alpha(pal.geoVia, 200), 1.5f);
        dl->AddCircleFilled(b, 2.0f, pal.geoVia, 8);
      }
    }
    for (const auto& pd : snapshot_.ports) {
      ImVec2 a, b;
      const bool vertical = !pd.layerB.empty();
      const double za = z0(pd.layer), zb2 = vertical ? z0(pd.layerB) : za;
      if (project(pd.a.x, pd.a.y, za, a) &&
          project(vertical ? pd.a.x : pd.b.x, vertical ? pd.a.y : pd.b.y, zb2, b)) {
        dl->AddLine(a, b, pal.geoPort, 3.0f);
        dl->AddCircleFilled(a, 5.0f, pal.geoPort, 12);
        dl->AddText({a.x + 8, a.y - 16}, pal.geoPortText, (std::string("port ") + pd.name).c_str());
      }
    }
  }
  {
    // Field legend (top right): sign by hue, strength by brightness and opacity, down to the faintest level
    // shown.
    const float barW = 230, x = volRect_.x + volRect_.w - barW - 24;
    const bool lightView = lightVolume_;
    const viz::Rgb dn = fieldColour(lightView, false), dp = fieldColour(lightView, true); // the theme's pair
    if (const auto lut = volumeLut(); !lut.empty()) {
      drawColourBar(
          dl, {x, volRect_.y + 8}, barW,
          ("Field magnitude, " + env_.settings.text("volume_colour_map")).c_str(),
          [lut](float t) {
            const float xx = std::clamp(t, 0.0f, 1.0f) * static_cast<float>(lut.size() - 1);
            return lut[static_cast<std::size_t>(std::lround(xx))];
          },
          "faint", "", "strong", "legendVolPos", legendOpen_[3]);
    } else {
      drawColourBar(
          dl, {x, volRect_.y + 8}, barW, "Positive field",
          [dn, dp, lightView](float t) {
            return lightView ? viz::volumeLight(t, dn, dp) : viz::luminous(t, dn, dp);
          },
          "faint", "", "strong", "legendVolPos", legendOpen_[3]);
      drawColourBar(
          dl,
          {x, volRect_.y + 8 +
                  (legendOpen_[3] ? ImGui::GetTextLineHeightWithSpacing() * 2 + 52
                                  : ImGui::GetTextLineHeightWithSpacing() + 12)},
          barW, "Negative field",
          [dn, dp, lightView](float t) {
            return lightView ? viz::volumeLight(-t, dn, dp) : viz::luminous(-t, dn, dp);
          },
          "faint", "", "strong", "legendVolNeg", legendOpen_[3]);
    }
    if (showGeometry_) {
      std::vector<LegendItem> items = {
          {LegendItem::Line, panelColours.saved.geoTop, "top copper"},
          {LegendItem::Line, panelColours.saved.geoOther, "bottom copper"},
          {LegendItem::Dot, panelColours.saved.geoVia, "via"},
          {LegendItem::Dot, panelColours.saved.geoPort, "port"},
          {LegendItem::Line, panelColours.saved.geoOutline, "board (outline and thickness)"}};
      if (showCurrent3d_ && currentScale_ > 0)
        items.push_back({LegendItem::Line, panelColours.saved.geoTop,
                         strf("surface current: field colours, full = %.2g A/m", currentScale_)});
      const float height = 16 + ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(items.size() + 1);
      drawLegend(dl,
                 {volRect_.x + 8, volRect_.y + volRect_.h -
                                      (legendOpen_[4] ? height : ImGui::GetTextLineHeightWithSpacing() + 8) -
                                      8},
                 "Board", items, "legendVolBoard", legendOpen_[4]);
    }
  }
  ImGui::PushStyleColor(ImGuiCol_Text, pal.dim);
  wrapped("Drag to rotate, right-drag (or Shift-drag) to pan, mouse wheel to zoom, double-click to refit. "
          "Brighter and more opaque = stronger field; the faintest level "
          "shown is 28 dB below full scale.");
  ImGui::PopStyleColor();
}

void Ui::drawStatusBar() {
  if (button(settingsOpen_ ? "<< Hide settings" : "Settings >>"))
    settingsOpen_ = !settingsOpen_;
  ImGui::SameLine();
  if (screen_ == Screen::Preview) {
    const std::string reason = blockedReason();
    ImGui::BeginDisabled(!reason.empty());
    if (button("Run simulation", ImVec2(px(180), 0)))
      start();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextColored(ImColor(reason.empty() ? pal.good : pal.warn), "%s",
                       reason.empty() ? "Ready." : reason.c_str());
    noteText(reason.empty() ? "Ready." : reason, viz::access::Role::Status);
  } else if (screen_ == Screen::Running) {
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - runStart_).count();
    const double frac = std::min(0.99, static_cast<double>(steps_) /
                                           std::max<double>(1, static_cast<double>(estimate_.steps)));
    if (button("Stop", ImVec2(px(100), 0)))
      stop_ = true;
    ImGui::SameLine();
    char label[96];
    std::snprintf(label, sizeof label, "step %zu of ~%zu", static_cast<std::size_t>(steps_), estimate_.steps);
    ImGui::ProgressBar(static_cast<float>(frac), ImVec2(px(280), 0), label);
    note(viz::access::Role::ProgressBar, "Simulation progress", label);
    ImGui::SameLine();
    char eta[128];
    std::snprintf(eta, sizeof eta, "elapsed %s, about %s left (estimate)", fmtTime(elapsed).c_str(),
                  fmtTime(frac > 0.02 ? elapsed * (1 - frac) / frac : estimate_.seconds).c_str());
    ImGui::TextUnformatted(eta);
    noteText(eta);
  } else {
    if (!runError_.empty()) {
      ImGui::TextColored(ImColor(pal.bad), "The run failed: %s", runError_.c_str());
      noteText("The run failed: " + runError_, viz::access::Role::Status);
    } else {
      ImGui::TextColored(ImColor(pal.good), "Finished in %s.", fmtTime(runSeconds_).c_str());
      noteText("Finished in " + fmtTime(runSeconds_) + ". Results are in " + runDir_.string(),
               viz::access::Role::Status);
      ImGui::SameLine();
      ImGui::TextUnformatted("Results:");
      ImGui::SameLine();
      ImGui::TextColored(ImColor(pal.accent), "%s", runDir_.string().c_str());
    }
    if (button("Open results folder"))
      app::openFolder(runDir_);
    ImGui::SameLine();
    if (button("Copy path"))
      ImGui::SetClipboardText(runDir_.string().c_str());
    ImGui::SameLine();
    if (button("Adjust settings and run again")) {
      if (worker_.joinable())
        worker_.join();
      screen_ = Screen::Preview;
      dirty_ = true;
      lastEdit_ = -1;
    }
  }
}

// ---- frame -----------------------------------------------------------------------------------------------

// The Vulkan context the viewer's renderers share (not the solver's: its queue is used by the solver thread).
vkcore::Context* Ui::renderContext() {
  if (!backend_.gpu ||
      (headless_ && !adaptive_)) // stills stay on the CPU reference; the benchmark uses the GPU
    return nullptr;
  if (!renderCtx_) {
    try {
      renderCtx_ = std::make_unique<vkcore::Context>();
    } catch (const std::exception& e) {
      std::fprintf(stderr, "3D and slice views on the CPU (no render context: %s)\n", e.what());
      backend_.gpu = false; // do not try again every frame
    }
  }
  return renderCtx_.get();
}

void Ui::frame(double now, std::uint32_t w, std::uint32_t h, std::vector<std::uint8_t>& rgba) {
  using Clock = std::chrono::steady_clock;
  const auto tStart = Clock::now();
  auto ms = [](Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
  };
  now_ = now;
  // Theme: the user's choice, with Auto following the system (checked about once a second: it is cheap but
  // not free).
  if (now - themeCheck_ > 1.0 || themeCheck_ > now) {
    themeCheck_ = now;
    const bool dark =
        viz::useDark(viz::parseThemeMode(env_.settings.text("ui_theme")), viz::systemPrefersDark());
    const viz::SystemAccess sys = viz::systemAccess();
    const bool hc = viz::resolve(viz::parseTri(env_.settings.text("high_contrast")), sys.highContrast);
    const bool motion = viz::resolve(viz::parseTri(env_.settings.text("reduce_motion")), sys.reduceMotion);
    if (motion && !reduceMotion_)
      paused_ = true; // reduce motion: start still; the user can press play
    reduceMotion_ = motion;
    opaquePanels_ = hc || sys.reduceTransparency;
    const double text = viz::resolveTextScale(env_.settings.text("text_scale"), viz::systemTextScale());
    if (std::abs(text - textScale_) > 1e-3) { // the text size changed: rebuild the font atlas and the style
      textScale_ = text;
      ImGuiIO& fio = ImGui::GetIO();
      fio.Fonts->Clear();
      ImFontConfig cfg;
      cfg.SizePixels = static_cast<float>(16 * textScale_);
      cfg.RasterizerDensity = env_.pixelScale;
      fio.Fonts->AddFontDefault(&cfg);
      soft_ = std::make_unique<viz::SoftImGui>(); // reads the new atlas
      if (onFontsChanged_)
        onFontsChanged_();
      themeApplied_ = false;
    }
    if (dark != dark_ || !themeApplied_ || hc != highContrast_) {
      dark_ = dark;
      highContrast_ = hc;
      themeApplied_ = true;
      ImGui::GetStyle() = ImGuiStyle(); // default sizes, then scaled with the text below
      if (dark_)
        ImGui::StyleColorsDark();
      else
        ImGui::StyleColorsLight();
      ImGuiStyle& st = ImGui::GetStyle();
      st.WindowRounding = 0;
      st.FrameRounding = 3;
      st.ItemSpacing = {8, 6};
      if (highContrast_) {
        const ImVec4 fg = dark_ ? ImVec4(1, 1, 1, 1) : ImVec4(0, 0, 0, 1);
        st.Colors[ImGuiCol_Text] = fg;
        st.Colors[ImGuiCol_Border] = fg;
        st.FrameBorderSize = 1;
        st.WindowBorderSize = 1;
      } else {
        st.FrameBorderSize = 0;
        st.WindowBorderSize = 1;
      }
      st.ScaleAllSizes(static_cast<float>(textScale_));
    }
  }
  pal = dark_ ? darkPalette() : lightPalette();
  if (highContrast_)
    pal = highContrast(pal, dark_);
  if (opaquePanels_)
    pal = opaque(pal);
  { // the slice and the 3D view follow the theme unless the user chose a panel in Settings, View
    auto panelLight = [&](const char* key) {
      const std::string v = env_.settings.text(key);
      return v == "follow" ? !dark_ : v == "light";
    };
    lightSlice_ = panelLight("slice_panel");
    lightVolume_ = panelLight("volume_panel");
  }
  ImGuiIO& io = ImGui::GetIO();
  if (headless_)
    io.DeltaTime = 1.0f / 60;
  if (finished_ && screen_ == Screen::Running) {
    if (worker_.joinable())
      worker_.join();
    screen_ = Screen::Done;
  }
  refreshPrepared(now);
  if (screen_ != Screen::Preview && phasors_ && !paused_)
    phase_ += 2 * 3.14159265358979 * animCyclesPerSecond_ * std::max(0.0, now - lastFrame_);
  lastFrame_ = now;
  if (screen_ != Screen::Preview && !jumped_ && !result_.s.s11.empty() &&
      phasors_) { // jump to the best match
    jumped_ = true;
    std::size_t best = 0;
    for (std::size_t i = 1; i < result_.s.s11.size(); ++i)
      if (std::abs(result_.s.s11[i]) < std::abs(result_.s.s11[best]))
        best = i;
    for (std::size_t i = 0; i < phasors_->frequencies(); ++i)
      if (std::abs(phasors_->frequencyHz(i) - result_.s.frequenciesHz[best]) <
          std::abs(phasors_->frequencyHz(freq_) - result_.s.frequenciesHz[best]))
        freq_ = i;
  }

  sliceRect_ = {};
  volRect_ = {};
  access_.clear();
  gTree = accessWanted_ ? &access_ : nullptr;
  if (click_.phase > 0) { // a click asked for by an assistive technology: press, hold a frame, release
    io.AddMousePosEvent(click_.x, click_.y); // every frame: the backend also reports the real pointer
    if (click_.phase == 1)
      io.AddMouseButtonEvent(0, true);
    else if (click_.phase == 3)
      io.AddMouseButtonEvent(0, false);
    click_.phase = click_.phase == 3 ? 0 : click_.phase + 1;
  }
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  // Draw lists use 16-bit indices: without this ImGui wraps them past 65535 vertices (a board with hundreds
  // of copper shapes) and triangles attach to the wrong points. SoftImGui honours the vertex offset.
  io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset; // Tab, arrows and Enter operate every control
  ImGui::NewFrame();
  const ImVec2 size = io.DisplaySize;
  // The field images are drawn under the UI layer, so the panels must not paint an opaque background over
  // them (the default window background is ~94 % opaque and made every render look nearly black). The frame
  // is cleared to the panel colour instead.
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
  ImGui::SetNextWindowPos({0, 0});
  ImGui::SetNextWindowSize(size);
  ImGui::Begin("##root", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);
  if (settingsOpen_) {
    const float leftW = std::min(px(440), size.x * 0.42f);
    ImGui::BeginChild("##left", ImVec2(leftW, 0), ImGuiChildFlags_Borders);
    drawSettings();
    ImGui::EndChild();
    ImGui::SameLine();
  }
  ImGui::BeginChild("##right", ImVec2(0, 0));
  drawStatusBar();
  ImGui::Separator();
  if (ImGui::BeginTabBar("##tabs")) {
    if (tabItem("Preview", nullptr, pendingTab_ == "Preview" ? ImGuiTabItemFlags_SetSelected : 0)) {
      drawPreviewTab(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y);
      ImGui::EndTabItem();
    }
    if (tabItem("S11 plot", nullptr, pendingTab_ == "S11 plot" ? ImGuiTabItemFlags_SetSelected : 0)) {
      drawS11Tab(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y);
      ImGui::EndTabItem();
    }
    if (tabItem("Field slice", nullptr, pendingTab_ == "Field slice" ? ImGuiTabItemFlags_SetSelected : 0)) {
      drawSliceTab(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y);
      ImGui::EndTabItem();
    }
    if (tabItem("Far field", nullptr, pendingTab_ == "Far field" ? ImGuiTabItemFlags_SetSelected : 0)) {
      drawFarFieldTab(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y);
      ImGui::EndTabItem();
    }
    if (probeAcc_ &&
        tabItem("Probes", nullptr, pendingTab_ == "Probes" ? ImGuiTabItemFlags_SetSelected : 0)) {
      drawProbesTab(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y);
      ImGui::EndTabItem();
    }
    if (tabItem("3D volume", nullptr, pendingTab_ == "3D volume" ? ImGuiTabItemFlags_SetSelected : 0)) {
      drawVolumeTab(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y);
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
    pendingTab_.clear();
  }
  ImGui::EndChild();
  ImGui::End();
  ImGui::PopStyleColor(2);
  ImGui::Render();

  const auto tLayout = Clock::now();
  const ImDrawData* dd = ImGui::GetDrawData();
  const float scale = dd->FramebufferScale.x;
  rgba.resize(static_cast<std::size_t>(w) * h * 4);
  {
    const std::uint32_t bg = pal.clear[0] | (std::uint32_t(pal.clear[1]) << 8) |
                             (std::uint32_t(pal.clear[2]) << 16) | (255u << 24);
    std::uint32_t* words = reinterpret_cast<std::uint32_t*>(rgba.data());
    std::fill(words, words + static_cast<std::size_t>(w) * h, bg);
  }
  if (sliceRect_.w > 4 && sliceRect_.h > 4 && phasors_) { // the field image sits under the UI layer
    viz::FrameSpec spec;
    spec.lightTheme = lightSlice_;
    spec.negative = fieldColour(lightSlice_, false);
    spec.positive = fieldColour(lightSlice_, true);
    spec.canvasWidth = static_cast<std::uint32_t>(sliceRect_.w * scale);
    spec.canvasHeight = static_cast<std::uint32_t>(sliceRect_.h * scale);
    std::uint32_t fw = 0, fh = 0;
    std::vector<std::uint8_t> rgb;
    {
      std::lock_guard<std::mutex> lock(mu_);
      // view the board and a margin around it, not the whole free-space domain
      const auto& m = prepared_->model;
      double bx0 = 1e9, bx1 = -1e9, by0 = 1e9, by1 = -1e9;
      for (const auto& pt : snapshot_.outline) {
        bx0 = std::min(bx0, pt.x - m.originX);
        bx1 = std::max(bx1, pt.x - m.originX);
        by0 = std::min(by0, pt.y - m.originY);
        by1 = std::max(by1, pt.y - m.originY);
      }
      const double padX = 0.15 * (bx1 - bx0), padY = 0.15 * (by1 - by0);
      spec.cropU0 = std::max(0.0, bx0 - padX);
      spec.cropU1 = std::min(m.grid.length(fdtd::X), bx1 + padX);
      spec.cropV0 = std::max(0.0, by0 - padY);
      spec.cropV1 = std::min(m.grid.length(fdtd::Y), by1 + padY);
      {
        // user zoom and pan about the default view's centre
        const double cu = 0.5 * (spec.cropU0 + spec.cropU1) + slicePanU_,
                     cv = 0.5 * (spec.cropV0 + spec.cropV1) + slicePanV_;
        const double hu = 0.5 * (spec.cropU1 - spec.cropU0) / sliceZoom_,
                     hv = 0.5 * (spec.cropV1 - spec.cropV0) / sliceZoom_;
        spec.cropU0 = cu - hu;
        spec.cropU1 = cu + hu;
        spec.cropV0 = cv - hv;
        spec.cropV1 = cv + hv;
        // metres per logical pixel, for panning (the image is fitted inside the rectangle)
        sliceCrop_ = {spec.cropU0, spec.cropU1, spec.cropV0, spec.cropV1};
        sliceMPerPx_ = std::max(2 * hu / std::max(1.0f, sliceRect_.w), 2 * hv / std::max(1.0f, sliceRect_.h));
      }
      // Colour scale: the 90th percentile of |field| over the visible nodes, so the single cell that carries
      // the port current does not wash out the rest (the few brightest cells saturate instead).
      std::vector<float> mags;
      for (std::size_t b = 0; b < plane_.nv; ++b)
        for (std::size_t a = 0; a < plane_.nu; ++a) {
          const double u = m.grid.edge(plane_.u, a), v = m.grid.edge(plane_.v, b);
          if (u >= spec.cropU0 && u <= spec.cropU1 && v >= spec.cropV0 && v <= spec.cropV1)
            mags.push_back(std::abs(phasors_->phasor(freq_, a + plane_.nu * b)));
        }
      if (!mags.empty()) {
        const std::size_t nth = static_cast<std::size_t>(0.90 * static_cast<double>(mags.size() - 1));
        std::nth_element(mags.begin(), mags.begin() + static_cast<std::ptrdiff_t>(nth), mags.end());
        spec.colourScale = std::max(static_cast<double>(mags[nth]) * scaleFactor_, 1e-30);
        sliceScale_ = spec.colourScale;
      }
      if (headless_) { // a still image should show the field at its strongest moment, not at a zero crossing
        // dominant phase of the visible field: arg(sum X^2)/2, each cell's weight capped at the colour scale
        std::complex<double> sum = 0;
        for (std::size_t b = 0; b < plane_.nv; ++b)
          for (std::size_t a = 0; a < plane_.nu; ++a) {
            const double u = m.grid.edge(plane_.u, a), v = m.grid.edge(plane_.v, b);
            if (u < spec.cropU0 || u > spec.cropU1 || v < spec.cropV0 || v > spec.cropV1)
              continue;
            std::complex<double> x = phasors_->phasor(freq_, a + plane_.nu * b);
            const double mag = std::abs(x);
            if (mag > spec.colourScale)
              x *= spec.colourScale / mag;
            sum += x * x;
          }
        phase_ = -0.5 * std::arg(sum);
      }
      if (!showFieldSlice_)
        spec.colourScale = 1e30; // field layer off: draws as an empty background
      if (!sliceTried_) {
        sliceTried_ = true;
        if (vkcore::Context* rc = renderContext()) {
          try {
            sliceRenderer_ = std::make_unique<viz::GpuSliceRenderer>(*rc, plane_.nu, plane_.nv);
          } catch (const std::exception& e) {
            std::fprintf(stderr, "slice view on the CPU (GPU renderer not used: %s)\n", e.what());
          }
        }
      }
      if (sliceRenderer_) { // the pixel mapping on the CPU (cheap), the shading on the GPU
        const viz::SliceMap map = viz::makeSliceMap(prepared_->model.grid, plane_, spec);
        fw = map.width;
        fh = map.height;
        sliceRenderer_->setPhasors(phasors_->re(freq_), phasors_->im(freq_), mask_);
        rgb = sliceRenderer_->render(map, spec, phase_);
      } else {
        rgb = viz::renderFrame(prepared_->model.grid, plane_, *phasors_, freq_, phase_, spec, mask_, fw, fh);
      }
    }
    const int ox = static_cast<int>(sliceRect_.x * scale), oy = static_cast<int>(sliceRect_.y * scale);
    for (std::uint32_t y = 0; y < fh && oy + y < h; ++y)
      for (std::uint32_t x = 0; x < fw && ox + x < w; ++x) {
        std::uint8_t* d = &rgba[((oy + y) * w + ox + x) * 4];
        const std::uint8_t* sPx = &rgb[(static_cast<std::size_t>(y) * fw + x) * 3];
        d[0] = sPx[0];
        d[1] = sPx[1];
        d[2] = sPx[2];
      }
  }
  const auto tSlice = Clock::now();
  if (volRect_.w > 4 && volRect_.h > 4 && volPhasors_) { // the volume image sits under the UI layer too
    viz::RenderSpec rs;
    rs.logRangeDb = 28;
    rs.density = density_;
    rs.lightTheme = lightVolume_; // the viewport follows the theme: glow on dark, absorption on light
    rs.negative = fieldColour(lightVolume_, false);
    rs.positive = fieldColour(lightVolume_, true);
    rs.lut = volumeLut(); // magnitude view with a colour table, or empty for the signed two-colour view
    rs.emission = 1.6;
    rs.threshold = 0.08;
    const float scale2 = ImGui::GetIO().DisplayFramebufferScale.x;
    const int ox = static_cast<int>(volRect_.x * scale2), oy = static_cast<int>(volRect_.y * scale2);
    const int ow = static_cast<int>(volRect_.w * scale2), oh = static_cast<int>(volRect_.h * scale2);
    const std::uint64_t version = phasorVersion_.load();
    // Statistics and the camera fit cost several milliseconds: redo them only when the data or the frequency
    // changed (at most four times a second while the run is going).
    if ((statsVersion_ != version || statsFreq_ != freq_) &&
        (screen_ == Screen::Done || statsFreq_ != freq_ || now_ - statsTime_ > 0.25)) {
      std::lock_guard<std::mutex> lock(mu_);
      volMag_.resize(volPhasors_->voxels());
      for (std::size_t v = 0; v < volMag_.size(); ++v)
        volMag_[v] = std::abs(volPhasors_->phasor(freq_, v));
      auto sorted = volMag_;
      // dB scale: full colour at the 92nd percentile (the few strongest cells at the port would otherwise set
      // the scale 30 dB above the lobes), 28 dB of range below it; brightness slides that window
      const std::size_t nth = static_cast<std::size_t>(0.92 * static_cast<double>(sorted.size() - 1));
      std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(nth), sorted.end());
      volMagScale_ = std::max(static_cast<double>(sorted[nth]), 1e-30);
      fitCam_ = cam_;
      viz::fitCamera(dv_, volMag_, volMagScale_ / std::pow(10.0, volBrightness_), 22, fitCam_);
      if (headless_) { // a still image at the field's dominant phase
        std::complex<double> sum = 0;
        for (std::size_t v = 0; v < volPhasors_->voxels(); ++v) {
          std::complex<double> x = volPhasors_->phasor(freq_, v);
          const double m = std::abs(x);
          if (m > sorted[nth])
            x *= sorted[nth] / m;
          sum += x * x;
        }
        phase_ = -0.5 * std::arg(sum);
      }
      statsVersion_ = version;
      statsFreq_ = freq_;
      statsTime_ = now_;
    }
    if (autoFit_) { // follow the activity, easing towards it so the view does not jump
      const double ease = reduceMotion_ ? 1.0 : 0.3; // reduce motion: jump straight there
      for (int a = 0; a < 3; ++a)
        cam_.target[a] += ease * (fitCam_.target[a] - cam_.target[a]);
      cam_.distance += ease * (fitCam_.distance - cam_.distance);
    }
    rs.colourScale = showField3d_ ? volMagScale_ / std::pow(10.0, volBrightness_) : 1e30; // off: empty volume
    // Render size: a fraction of the screen rectangle that adapts to the measured cost (target ~20 ms), so
    // the view stays above 24 fps even on a Retina screen. Still images (headless) render at full size.
    const double res = adaptive_ ? volRes_ : 1.0;
    rs.width = std::max<std::uint32_t>(16, static_cast<std::uint32_t>(ow * res));
    rs.height = std::max<std::uint32_t>(16, static_cast<std::uint32_t>(oh * res));
    // Reuse the last image if nothing that affects it changed (paused, camera still).
    const std::array<double, 20> key{
        static_cast<double>(freq_),
        rs.lut.empty() ? phase_ : 0.0, // a magnitude view does not animate: one image per setting
        cam_.azimuth,
        cam_.elevation,
        cam_.distance,
        cam_.target[0],
        cam_.target[1],
        cam_.target[2],
        volBrightness_,
        density_,
        static_cast<double>(rs.width),
        static_cast<double>(rs.height),
        static_cast<double>(statsVersion_),
        rs.colourScale,
        lightVolume_ ? 0.0 : 1.0,
        static_cast<double>(rs.negative.r << 16 | rs.negative.g << 8 | rs.negative.b),
        static_cast<double>(rs.positive.r << 16 | rs.positive.g << 8 | rs.positive.b),
        static_cast<double>(rs.lut.empty()
                                ? 0
                                : (rs.lut[rs.lut.size() / 2].r << 16 | rs.lut[rs.lut.size() / 2].g << 8 |
                                   rs.lut[rs.lut.size() / 2].b) +
                                      1),
        0,
        0};
    if (key != lastKey_ || volImg_.empty()) {
      const auto tRender = Clock::now();
      if (!renderTried_) {
        renderTried_ = true;
        std::string why;
        if (vkcore::Context* rc = renderContext()) {
          try {
            volRenderer_ = viz::makeVolumeRenderer(rc, dv_, &why);
          } catch (const std::exception& e) {
            why = e.what();
          }
        }
        if (renderCtx_ || !why.empty())
          std::fprintf(stderr, "3D view: %s%s\n", volRenderer_ ? volRenderer_->name().c_str() : "CPU",
                       why.empty() ? "" : (" (GPU renderer not used: " + why + ")").c_str());
        if (volRenderer_ && volRenderer_->name() == "CPU")
          volRenderer_.reset();
      }
      if (volRenderer_) {
        {
          std::lock_guard<std::mutex> lock(mu_);
          if (renderVersion_ != statsVersion_ || renderFreq_ != freq_) { // new phasors: upload once
            volRenderer_->setPhasors(volPhasors_->re(freq_), volPhasors_->im(freq_));
            renderVersion_ = statsVersion_;
            renderFreq_ = freq_;
          }
        }
        volImg_ = volRenderer_->render(cam_, rs, phase_, !rs.lut.empty());
      } else {
        {
          std::lock_guard<std::mutex> lock(mu_);
          if (rs.lut.empty())
            viz::reconstruct(*volPhasors_, freq_, phase_, volField_);
          else
            volField_ = volMag_; // the magnitude of the phasor: no sign, no phase
        }
        volImg_ = viz::renderVolume(dv_, volField_, cam_, rs);
      }
      volImgW_ = rs.width;
      volImgH_ = rs.height;
      lastKey_ = key;
      if (adaptive_) { // keep the render near 20 ms: time grows with the pixel count, i.e. with res squared
        const double took = ms(tRender, Clock::now());
        volRes_ = std::clamp(volRes_ * std::clamp(std::sqrt(16.0 / std::max(took, 1.0)), 0.8, 1.1), 0.2, 1.0);
      }
    }
    // nearest-neighbour blit of the (possibly smaller) image into the rectangle
    std::vector<std::size_t> srcX(static_cast<std::size_t>(std::max(ow, 0)));
    for (int x = 0; x < ow; ++x)
      srcX[static_cast<std::size_t>(x)] =
          static_cast<std::size_t>(x) * volImgW_ / static_cast<std::size_t>(ow) * 3;
    for (int y = 0; y < oh && oy + y >= 0 && oy + y < static_cast<int>(h); ++y) {
      const std::uint8_t* row =
          &volImg_[static_cast<std::size_t>(y) * volImgH_ / static_cast<std::size_t>(oh) * volImgW_ * 3];
      std::uint8_t* d = &rgba[(static_cast<std::size_t>(oy + y) * w + static_cast<std::size_t>(ox)) * 4];
      for (int x = 0; x < ow && ox + x < static_cast<int>(w); ++x, d += 4) {
        const std::uint8_t* sPx = row + srcX[static_cast<std::size_t>(x)];
        d[0] = sPx[0];
        d[1] = sPx[1];
        d[2] = sPx[2];
      }
    }
  }
  const auto tVolume = Clock::now();
  if (!gpuUi_)
    soft_->render(ImGui::GetDrawData(), rgba.data(), w, h);
  const auto tEnd = Clock::now();
  times_ = {ms(tStart, tLayout), ms(tLayout, tSlice), ms(tSlice, tVolume), ms(tVolume, tEnd),
            ms(tStart, tEnd)};
}

} // namespace ui
