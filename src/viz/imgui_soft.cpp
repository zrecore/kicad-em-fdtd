#include "viz/imgui_soft.hpp"

#include <algorithm>
#include <cmath>

#include "imgui.h"

namespace viz {

SoftImGui::SoftImGui() {
  unsigned char* pixels = nullptr;
  ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &atlasWidth_, &atlasHeight_);
  atlas_ = pixels;
}

void SoftImGui::render(ImDrawData* data, std::uint8_t* rgba, std::uint32_t width,
                       std::uint32_t height) const {
  if (!data)
    return;
  const float sx = data->FramebufferScale.x, sy = data->FramebufferScale.y; // points to pixels
  for (int n = 0; n < data->CmdListsCount; ++n) {
    const ImDrawList* list = data->CmdLists[n];
    for (const ImDrawCmd& cmd : list->CmdBuffer) {
      if (cmd.UserCallback)
        continue;
      const int x0 = std::max(0, static_cast<int>(std::floor(cmd.ClipRect.x * sx))),
                y0 = std::max(0, static_cast<int>(std::floor(cmd.ClipRect.y * sy)));
      const int x1 = std::min<int>(width, static_cast<int>(std::ceil(cmd.ClipRect.z * sx)));
      const int y1 = std::min<int>(height, static_cast<int>(std::ceil(cmd.ClipRect.w * sy)));
      if (x0 >= x1 || y0 >= y1)
        continue;
      const ImDrawIdx* idx = list->IdxBuffer.Data + cmd.IdxOffset;
      const ImDrawVert* vtx = list->VtxBuffer.Data + cmd.VtxOffset;
      for (unsigned i = 0; i + 2 < cmd.ElemCount; i += 3) {
        ImDrawVert a = vtx[idx[i]], b = vtx[idx[i + 1]], c = vtx[idx[i + 2]];
        for (ImDrawVert* v : {&a, &b, &c}) { // draw data is in points; the frame is in pixels
          v->pos.x *= sx;
          v->pos.y *= sy;
        }
        const float minX = std::min({a.pos.x, b.pos.x, c.pos.x}),
                    maxX = std::max({a.pos.x, b.pos.x, c.pos.x});
        const float minY = std::min({a.pos.y, b.pos.y, c.pos.y}),
                    maxY = std::max({a.pos.y, b.pos.y, c.pos.y});
        const int px0 = std::max(x0, static_cast<int>(std::floor(minX))),
                  px1 = std::min(x1, static_cast<int>(std::ceil(maxX)));
        const int py0 = std::max(y0, static_cast<int>(std::floor(minY))),
                  py1 = std::min(y1, static_cast<int>(std::ceil(maxY)));
        const float det =
            (b.pos.y - c.pos.y) * (a.pos.x - c.pos.x) + (c.pos.x - b.pos.x) * (a.pos.y - c.pos.y);
        if (std::abs(det) < 1e-12f)
          continue;
        const float inv = 1.0f / det;
        for (int y = py0; y < py1; ++y)
          for (int x = px0; x < px1; ++x) {
            // sample a hair off the pixel centre so a pixel on the shared diagonal of a quad belongs to one
            // triangle only
            const float fx = x + 0.5013f, fy = y + 0.5021f;
            const float w0 =
                ((b.pos.y - c.pos.y) * (fx - c.pos.x) + (c.pos.x - b.pos.x) * (fy - c.pos.y)) * inv;
            const float w1 =
                ((c.pos.y - a.pos.y) * (fx - c.pos.x) + (a.pos.x - c.pos.x) * (fy - c.pos.y)) * inv;
            const float w2 = 1.0f - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0)
              continue;
            const float u = w0 * a.uv.x + w1 * b.uv.x + w2 * c.uv.x,
                        v = w0 * a.uv.y + w1 * b.uv.y + w2 * c.uv.y;
            const int tx = std::clamp(static_cast<int>(u * atlasWidth_), 0, atlasWidth_ - 1);
            const int ty = std::clamp(static_cast<int>(v * atlasHeight_), 0, atlasHeight_ - 1);
            const std::uint8_t* t = atlas_ + 4 * (static_cast<std::size_t>(ty) * atlasWidth_ + tx);
            // vertex colour (flat in practice for widgets; interpolated for gradients) times the texel
            auto channel = [&](int shift) {
              const float ca = (a.col >> shift & 0xFF) * w0, cb = (b.col >> shift & 0xFF) * w1,
                          cc = (c.col >> shift & 0xFF) * w2;
              return (ca + cb + cc) / 255.0f;
            };
            const float r = channel(IM_COL32_R_SHIFT) * t[0] / 255.0f,
                        g = channel(IM_COL32_G_SHIFT) * t[1] / 255.0f,
                        bl = channel(IM_COL32_B_SHIFT) * t[2] / 255.0f,
                        al = channel(IM_COL32_A_SHIFT) * t[3] / 255.0f;
            std::uint8_t* d = rgba + 4 * (static_cast<std::size_t>(y) * width + x);
            d[0] = static_cast<std::uint8_t>(std::clamp(d[0] * (1 - al) + r * 255.0f * al, 0.0f, 255.0f));
            d[1] = static_cast<std::uint8_t>(std::clamp(d[1] * (1 - al) + g * 255.0f * al, 0.0f, 255.0f));
            d[2] = static_cast<std::uint8_t>(std::clamp(d[2] * (1 - al) + bl * 255.0f * al, 0.0f, 255.0f));
            d[3] = 255;
          }
      }
    }
  }
}

} // namespace viz
