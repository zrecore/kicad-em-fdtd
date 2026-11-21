// Presents CPU-rendered RGBA frames to a window surface (the first viewer path). One frame in flight; the
// swapchain is recreated when the surface size changes.
#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
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
  // `overlay`, if given, records draw commands (for example Dear ImGui's) into a render pass over the copied
  // frame (render pass renderPass(), one subpass, colour load op LOAD) before the image is presented.
  using Overlay = std::function<void(VkCommandBuffer)>;
  bool present(const std::uint8_t* rgba, std::uint32_t width, std::uint32_t height,
               const Overlay& overlay = {});
  // For a UI backend that builds pipelines against the pass; the pass lives as long as the swapchain object.
  VkRenderPass renderPass() const { return renderPass_; }
  std::uint32_t imageCount() const { return static_cast<std::uint32_t>(images_.size()); }
  std::uint32_t minImageCount() const { return minImages_; }

private:
  void create();
  void destroy();
  Context& ctx_;
  VkSurfaceKHR surface_;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkExtent2D extent_{};
  VkFormat format_ = VK_FORMAT_UNDEFINED;
  std::vector<VkImage> images_;
  std::vector<VkImageView> views_;
  std::vector<VkFramebuffer> framebuffers_;
  VkRenderPass renderPass_ = VK_NULL_HANDLE;
  std::uint32_t minImages_ = 2;
  bool overlayCapable_ = false; // the surface allows colour-attachment use of its images
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  VkSemaphore acquired_ = VK_NULL_HANDLE;
  std::vector<VkSemaphore>
      done_; // one per swapchain image: a semaphore may still be in use by the presentation
  VkFence fence_ = VK_NULL_HANDLE;
  std::unique_ptr<Buffer> staging_;
  std::size_t stagingBytes_ = 0;
};

} // namespace vkcore
