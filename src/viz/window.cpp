#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include "viz/window.hpp"

#include <GLFW/glfw3.h>

#include <cstdlib>
#include <stdexcept>

namespace viz {

Window::Window(const std::string& title, int width, int height) {
  // Give GLFW the loader we link against: its own search misses Homebrew's libvulkan on macOS.
  glfwInitVulkanLoader(vkGetInstanceProcAddr);
  if (!glfwInit())
    throw std::runtime_error("glfwInit failed");
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  window_ = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
  if (!window_)
    throw std::runtime_error("cannot create a window");
  std::uint32_t n = 0;
  const char** exts = glfwGetRequiredInstanceExtensions(&n);
  if (!exts)
    throw std::runtime_error("GLFW reports no Vulkan surface support");
  vkcore::ContextOptions opt;
  opt.instanceExtensions.assign(exts, exts + n);
  opt.swapchain = true;
  opt.validation = std::getenv("EMFDTD_VALIDATE") != nullptr; // debugging aid: the Vulkan validation layer
  ctx_ = std::make_unique<vkcore::Context>(opt);
  if (glfwCreateWindowSurface(ctx_->instance(), window_, nullptr, &surface_) != VK_SUCCESS)
    throw std::runtime_error("cannot create the window surface");
  swapchain_ = std::make_unique<vkcore::Swapchain>(*ctx_, surface_);
  glfwSetWindowUserPointer(window_, this);
  glfwSetKeyCallback(window_, &Window::onKey);
}

Window::~Window() {
  swapchain_.reset();
  if (surface_)
    vkDestroySurfaceKHR(ctx_->instance(), surface_, nullptr);
  ctx_.reset();
  if (window_)
    glfwDestroyWindow(window_);
  glfwTerminate();
}

bool Window::shouldClose() const {
  return glfwWindowShouldClose(window_);
}
void Window::pollEvents() {
  glfwPollEvents();
}
void Window::setTitle(const std::string& title) {
  glfwSetWindowTitle(window_, title.c_str());
}

void Window::onKey(GLFWwindow* w, int key, int, int action, int) {
  if (action == GLFW_PRESS || action == GLFW_REPEAT)
    static_cast<Window*>(glfwGetWindowUserPointer(w))->keys_.push_back(key);
}

std::vector<int> Window::takeKeys() {
  std::vector<int> k;
  k.swap(keys_);
  return k;
}

bool Window::presentRgba(const std::vector<std::uint8_t>& rgba, std::uint32_t width, std::uint32_t height) {
  return swapchain_->present(rgba.data(), width, height);
}

void Window::focus() {
  glfwRestoreWindow(window_);
  glfwFocusWindow(window_);
  glfwRequestWindowAttention(window_);
}

float Window::pixelScale() const {
  int w = 0, h = 0, fw = 0, fh = 0;
  glfwGetWindowSize(window_, &w, &h);
  glfwGetFramebufferSize(window_, &fw, &fh);
  return w > 0 ? static_cast<float>(fw) / static_cast<float>(w) : 1.0f;
}

bool Window::present(const std::vector<std::uint8_t>& rgb, std::uint32_t width, std::uint32_t height) {
  rgba_.resize(static_cast<std::size_t>(width) * height * 4);
  for (std::size_t i = 0, n = static_cast<std::size_t>(width) * height; i < n; ++i) {
    rgba_[4 * i] = rgb[3 * i];
    rgba_[4 * i + 1] = rgb[3 * i + 1];
    rgba_[4 * i + 2] = rgb[3 * i + 2];
    rgba_[4 * i + 3] = 255;
  }
  return swapchain_->present(rgba_.data(), width, height);
}

} // namespace viz
