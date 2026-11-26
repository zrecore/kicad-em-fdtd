// A persistently mapped storage buffer. Residency::Host (default) is host-visible memory: right for small
// buffers the host reads or writes often, and the fast path on unified memory. Residency::Device is for the
// large state a kernel streams through every step: on a discrete GPU it lives in VRAM (a host staging buffer
// carries the data in and out), on unified memory it is the same host-visible buffer and the sync calls do
// nothing. See docs/PLAN.md, GPU farm, first Windows eGPU runs.
#pragma once
#include <vulkan/vulkan.h>

#include <span>

#include "vkcore/context.hpp"

namespace vkcore {

enum class Residency { Host, Device };

class Buffer {
public:
  Buffer(Context& ctx, VkDeviceSize bytes, Residency residency = Residency::Host);
  ~Buffer();
  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;

  // The buffer the kernels bind (the VRAM copy when staged).
  VkBuffer handle() const { return buffer_; }
  VkDeviceSize size() const { return bytes_; }
  VkMemoryPropertyFlags memoryFlags() const { return memoryFlags_; }
  // The host view: the buffer itself, or its staging copy when staged (then call upload() after writing and
  // download() before reading).
  template <class T> std::span<T> as() const {
    return {static_cast<T*>(mapped_), static_cast<std::size_t>(bytes_ / sizeof(T))};
  }
  bool staged() const { return staging_ != VK_NULL_HANDLE; }
  // Host view to device (bytes of the range; whole buffer by default). No-ops when not staged.
  void upload(VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) const;
  // Device to host view.
  void download(VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) const;

private:
  Context& ctx_;
  VkDevice device_;
  VkDeviceSize bytes_;
  VkBuffer buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory memory_ = VK_NULL_HANDLE;
  VkBuffer staging_ = VK_NULL_HANDLE;
  VkDeviceMemory stagingMemory_ = VK_NULL_HANDLE;
  VkMemoryPropertyFlags memoryFlags_ = 0;
  void* mapped_ = nullptr;
};

} // namespace vkcore
