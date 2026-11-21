// Dear ImGui drawn by the GPU (its Vulkan backend) over a frame that is already composed on the CPU (the
// field images). The window's own context and swapchain are used; SoftImGui stays for headless screenshots.
#pragma once
#include <cstdint>
#include <vector>

#include "viz/window.hpp"

struct ImDrawData;

namespace viz {

class GpuUi {
public:
  // An ImGui context must be current. Throws if the swapchain cannot take an overlay or the backend fails.
  explicit GpuUi(Window& window);
  ~GpuUi();
  GpuUi(const GpuUi&) = delete;
  GpuUi& operator=(const GpuUi&) = delete;

  void newFrame(); // before ImGui::NewFrame()
  // The font atlas was rebuilt (text size changed): the backend's copy is dropped and made again.
  void fontsChanged();
  // Presents `rgba` (width*height*4, top row first) with `draw` drawn over it.
  bool present(const std::vector<std::uint8_t>& rgba, std::uint32_t width, std::uint32_t height,
               ImDrawData* draw);

private:
  Window& window_;
};

} // namespace viz
