#include "ui.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

#include "app/phasor_feed.hpp"
#include "app/results.hpp"
#include "imgui.h"
#include "viz/a11y.hpp"
#include "viz/colormap.hpp"
#include "viz/theme.hpp"
#include "viz/triangulate.hpp"
#include "viz/view_axes.hpp"

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
  ImGui::DestroyContext(ctx_);
}

bool Ui::setSnapshot(board::Snapshot s) {
  if (screen_ == Screen::Running)
    return false;
  if (worker_.joinable())
    worker_.join();
  snapshot_ = std::move(s);
  phasors_.reset();
  volPhasors_.reset();
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
    opt.onSample = [&](sim::Engine& eng, double, double dtSample) {
      const bool a = sliceFeed.sample(eng, dtSample, mu_);
      const bool b = volumeFeed.sample(eng, dtSample, mu_);
      if (a || b)
        ++phasorVersion_;
    };
    opt.onFinish = [&](sim::Engine& eng) {
      sliceFeed.sync(eng, mu_);
      volumeFeed.sync(eng, mu_);
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
    {
      std::lock_guard<std::mutex> lock(mu_);
      liveS11_ = r.s;
      result_ = std::move(r);
    }
    app::writeResults(runDir_, env_.snapshotPath, snapshot_, env_.settings, *prepared_, estimate_, result_,
                      backend_.name);
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

void Ui::drawPreviewTab(float w, float h) {
  const board::Snapshot& s = snapshot_;
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
      if (!env_.selection.includes(c.owner))
        continue;
      pts.clear();
      for (const auto& p : c.outer)
        pts.push_back(P(p.x, p.y));
      const bool top = !s.stackup.empty() && c.layer == s.stackup.front().name;
      dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), top ? pal.geoTop : alpha(pal.geoOther, 160),
                      ImDrawFlags_Closed, 1.0f);
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
                {LegendItem::Line, pal.geoBox, "domain (free space around the board)"}},
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

void Ui::drawS11Tab(float w, float h) {
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
    const std::vector<LegendItem> items = {
        {LegendItem::Line, pal.curve, "|S11| in dB (lower = more power accepted)"},
        {LegendItem::Swatch, alpha(pal.curve, 90), "band of interest"},
        {LegendItem::Dashed, pal.good, "-10 dB: a good match"},
        {LegendItem::Line, pal.geoBox, "channel centres (profile)"},
        {LegendItem::Dot, pal.text, "best match in the sweep"}};
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
    checkbox("board outline", &showOutline_);
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
    if (sliceScale_ > 0)
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
    checkbox("auto-fit", &autoFit_);
    ImGui::SameLine();
    checkbox("copper", &showGeometry_);
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
    if (showGeometry_) {
      const std::vector<LegendItem> items = {
          {LegendItem::Line, panelColours.saved.geoTop, "top copper"},
          {LegendItem::Line, panelColours.saved.geoOther, "bottom copper"},
          {LegendItem::Dot, panelColours.saved.geoVia, "via"},
          {LegendItem::Dot, panelColours.saved.geoPort, "port"},
          {LegendItem::Line, panelColours.saved.geoOutline, "board (outline and thickness)"}};
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
    phase_ += 2 * 3.14159265358979 * 0.5 * std::max(0.0, now - lastFrame_);
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
      rgb = viz::renderFrame(prepared_->model.grid, plane_, *phasors_, freq_, phase_, spec, mask_, fw, fh);
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
    rs.colourScale = volMagScale_ / std::pow(10.0, volBrightness_);
    // Render size: a fraction of the screen rectangle that adapts to the measured cost (target ~20 ms), so
    // the view stays above 24 fps even on a Retina screen. Still images (headless) render at full size.
    const double res = adaptive_ ? volRes_ : 1.0;
    rs.width = std::max<std::uint32_t>(16, static_cast<std::uint32_t>(ow * res));
    rs.height = std::max<std::uint32_t>(16, static_cast<std::uint32_t>(oh * res));
    // Reuse the last image if nothing that affects it changed (paused, camera still).
    const std::array<double, 20> key{
        static_cast<double>(freq_),
        phase_,
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
        0,
        0,
        0};
    if (key != lastKey_ || volImg_.empty()) {
      const auto tRender = Clock::now();
      {
        std::lock_guard<std::mutex> lock(mu_);
        viz::reconstruct(*volPhasors_, freq_, phase_, volField_);
      }
      volImg_ = viz::renderVolume(dv_, volField_, cam_, rs);
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
  soft_->render(ImGui::GetDrawData(), rgba.data(), w, h);
  const auto tEnd = Clock::now();
  times_ = {ms(tStart, tLayout), ms(tLayout, tSlice), ms(tSlice, tVolume), ms(tVolume, tEnd),
            ms(tStart, tEnd)};
}

} // namespace ui
