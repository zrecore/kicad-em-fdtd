// Instance, device selection, one compute-capable queue, optional validation with error counting.
#pragma once
#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "vkcore/device_select.hpp"

namespace vkcore {

struct ContextOptions {
  bool validation = false;    // enable VK_LAYER_KHRONOS_validation if it is installed
  std::uint32_t vendorId = 0; // optional override: pick this vendor/device instead of the default rule
  std::uint32_t deviceId = 0;
  std::string device; // user selector (index or UUID prefix, see device_select.hpp); wins over the rule above
  std::vector<const char*> instanceExtensions; // e.g. the surface extensions a window needs
  bool swapchain = false;                      // enable VK_KHR_swapchain on the device
  // Test hook (also env EMFDTD_STAGED_BUFFERS=1): treat device memory as discrete even on unified memory, so
  // the staged-buffer path can be exercised and compared on a machine without a discrete GPU.
  bool stagedBuffers = false;
};

// Every Vulkan device the loader reports (usable or not), in enumeration order, with its UUID and driver.
// Creates and destroys a bare instance; no logical device.
std::vector<DeviceInfo> listDevices();

class Context {
public:
  explicit Context(const ContextOptions& options = {});
  ~Context();
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;

  VkInstance instance() const { return instance_; }
  VkPhysicalDevice physical() const { return physical_; }
  VkDevice device() const { return device_; }
  VkQueue queue() const { return queue_; }
  std::uint32_t queueFamily() const { return queueFamily_; }
  const VkPhysicalDeviceProperties& properties() const { return props_; }
  VkDeviceSize deviceLocalBytes() const { return deviceLocalBytes_; }
  // What the device promises for 32-bit float arithmetic in shaders (Vulkan 1.2 float controls): whether
  // denormals are kept or flushed and whether signed zeros, infinities and NaNs are preserved. Empty if the
  // device does not report them. For the run summary: a reader checking a precision claim needs it.
  const std::string& floatControls() const { return floatControls_; }

  // True when device-local memory is directly host-visible and large (Apple and other integrated GPUs, or a
  // discrete GPU with resizable BAR): device-resident buffers then need no staging copy. False on a
  // discrete GPU, where field arrays must live in VRAM or every step streams them over PCIe.
  bool unifiedMemory() const { return unified_; }
  const DeviceInfo& deviceInfo() const { return info_; }
  // Synchronous buffer-to-buffer copy on the compute queue (staging uploads and read-backs). Not for hot
  // paths.
  void copyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize srcOffset, VkDeviceSize dstOffset,
                  VkDeviceSize size);

  VkMemoryPropertyFlags memoryTypeFlags(std::uint32_t index) const;
  std::uint32_t findMemoryType(std::uint32_t typeBits, VkMemoryPropertyFlags wanted) const;

  bool validationEnabled() const { return messenger_ != VK_NULL_HANDLE; }
  unsigned validationErrors() const { return validationErrors_.load(); }

private:
  VkInstance instance_ = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  std::uint32_t queueFamily_ = 0;
  VkPhysicalDeviceProperties props_{};
  VkDeviceSize deviceLocalBytes_ = 0;
  std::string floatControls_;
  DeviceInfo info_;
  bool unified_ = true;
  VkCommandPool copyPool_ = VK_NULL_HANDLE;
  VkCommandBuffer copyCmd_ = VK_NULL_HANDLE;
  VkFence copyFence_ = VK_NULL_HANDLE;
  std::atomic<unsigned> validationErrors_{0};
};

} // namespace vkcore
