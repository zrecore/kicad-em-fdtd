// A window with its own Vulkan context and swapchain (GLFW). Frames are CPU-rendered RGBA for now.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "vkcore/swapchain.hpp"

struct GLFWwindow;

namespace viz {

class Window {
public:
  Window(const std::string& title, int width, int height);
  ~Window();
  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  bool shouldClose() const;
  void pollEvents();           // main thread only
  std::vector<int> takeKeys(); // GLFW key codes pressed (or repeated) since the last call
  void setTitle(const std::string& title);
  VkExtent2D extent() { return swapchain_->extent(); }
  bool present(const std::vector<std::uint8_t>& rgb, std::uint32_t width, std::uint32_t height);
  // Frames that are already RGBA (the UI path).
  bool presentRgba(const std::vector<std::uint8_t>& rgba, std::uint32_t width, std::uint32_t height);
  GLFWwindow* handle() const { return window_; }
  vkcore::Context& context() { return *ctx_; }
  vkcore::Swapchain& swapchain() { return *swapchain_; }
  void focus();             // restore, raise and ask for the user's attention (a new export arrived)
  float pixelScale() const; // framebuffer pixels per window point (2 on a Retina display)

private:
  static void onKey(GLFWwindow* w, int key, int scancode, int action, int mods);
  GLFWwindow* window_ = nullptr;
  std::unique_ptr<vkcore::Context> ctx_;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  std::unique_ptr<vkcore::Swapchain> swapchain_;
  std::vector<int> keys_;
  std::vector<std::uint8_t> rgba_;
};

} // namespace viz
