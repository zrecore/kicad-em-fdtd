// Presents CPU-rendered RGBA frames to a window surface (the first viewer path). One frame in flight; the
// swapchain is recreated when the surface size changes.
#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "vkcore/buffer.hpp"
#include "vkcore/context.hpp"

namespace vkcore {

class Swapchain {
public:
  // The surface must belong to ctx.instance(); ctx must have been created with swapchain support.
  Swapchain(Context& ctx, VkSurfaceKHR surface);
  ~Swapchain();
  Swapchain(const Swapchain&) = delete;
  Swapchain& operator=(const Swapchain&) = delete;

  // Surface extent in pixels (what frames should be rendered at).
  VkExtent2D extent();
  // Copies tightly packed RGBA8 (width*height*4 bytes, top row first) to the next image and presents it.
  // Frames whose size differs from the current extent are skipped (returns false); so is a minimised window.
  bool present(const std::uint8_t* rgba, std::uint32_t width, std::uint32_t height);

private:
  void create();
  void destroy();
  Context& ctx_;
  VkSurfaceKHR surface_;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkExtent2D extent_{};
  VkFormat format_ = VK_FORMAT_UNDEFINED;
  std::vector<VkImage> images_;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  VkSemaphore acquired_ = VK_NULL_HANDLE, done_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  std::unique_ptr<Buffer> staging_;
  std::size_t stagingBytes_ = 0;
};

} // namespace vkcore
