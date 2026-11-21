#include "viz/gpu_ui.hpp"

#include <stdexcept>

#include "imgui.h"
#include "imgui_impl_vulkan.h"

namespace viz {

GpuUi::GpuUi(Window& window) : window_(window) {
  vkcore::Context& ctx = window.context();
  vkcore::Swapchain& sc = window.swapchain();
  if (sc.renderPass() == VK_NULL_HANDLE)
    throw std::runtime_error("the surface does not allow drawing on its images");
  ImGui_ImplVulkan_InitInfo info{};
  info.ApiVersion = ctx.properties().apiVersion;
  info.Instance = ctx.instance();
  info.PhysicalDevice = ctx.physical();
  info.Device = ctx.device();
  info.QueueFamily = ctx.queueFamily();
  info.Queue = ctx.queue();
  info.DescriptorPoolSize = 8;
  info.RenderPass = sc.renderPass();
  info.MinImageCount = sc.minImageCount();
  info.ImageCount = sc.imageCount();
  info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
  if (!ImGui_ImplVulkan_Init(&info))
    throw std::runtime_error("the Dear ImGui Vulkan backend did not start");
}

GpuUi::~GpuUi() {
  vkDeviceWaitIdle(window_.context().device());
  ImGui_ImplVulkan_Shutdown();
}

void GpuUi::newFrame() {
  ImGui_ImplVulkan_NewFrame();
}

void GpuUi::fontsChanged() {
  vkDeviceWaitIdle(window_.context().device());
  // Made now, not by the next NewFrame: the rebuild happens inside the frame that already called NewFrame,
  // and that frame's draw data refers to the new texture (a null descriptor set otherwise).
  ImGui_ImplVulkan_DestroyFontsTexture();
  ImGui_ImplVulkan_CreateFontsTexture();
}

bool GpuUi::present(const std::vector<std::uint8_t>& rgba, std::uint32_t width, std::uint32_t height,
                    ImDrawData* draw) {
  return window_.swapchain().present(rgba.data(), width, height, [draw](VkCommandBuffer cmd) {
    ImGui_ImplVulkan_RenderDrawData(draw, cmd);
  });
}

} // namespace viz
