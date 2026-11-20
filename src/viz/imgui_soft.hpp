// CPU rasteriser for Dear ImGui draw data: composites the UI over an RGBA frame that the window then
// presents. (The window presents CPU-rendered frames; this keeps the UI on the same path and free of a Vulkan
// UI backend.)
#pragma once
#include <cstdint>

struct ImDrawData;

namespace viz {

class SoftImGui {
public:
  // Reads the font atlas from the current ImGui context; call once after the context exists.
  SoftImGui();
  // Draws `data` over rgba (width*height*4 bytes, top row first), alpha-blending.
  void render(ImDrawData* data, std::uint8_t* rgba, std::uint32_t width, std::uint32_t height) const;

private:
  const std::uint8_t* atlas_ = nullptr; // RGBA32
  int atlasWidth_ = 0, atlasHeight_ = 0;
};

} // namespace viz
