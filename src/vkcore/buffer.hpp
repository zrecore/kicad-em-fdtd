// A persistently mapped, host-visible storage buffer (on unified memory this is also the fast path).
#pragma once
#include <vulkan/vulkan.h>

#include <span>

#include "vkcore/context.hpp"

namespace vkcore {

class Buffer {
public:
  Buffer(Context& ctx, VkDeviceSize bytes);
  ~Buffer();
  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;

  VkBuffer handle() const { return buffer_; }
  VkDeviceSize size() const { return bytes_; }
  VkMemoryPropertyFlags memoryFlags() const { return memoryFlags_; }
  template <class T> std::span<T> as() const {
    return {static_cast<T*>(mapped_), static_cast<std::size_t>(bytes_ / sizeof(T))};
  }

private:
  VkDevice device_;
  VkDeviceSize bytes_;
  VkBuffer buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory memory_ = VK_NULL_HANDLE;
  VkMemoryPropertyFlags memoryFlags_ = 0;
  void* mapped_ = nullptr;
};

} // namespace vkcore
