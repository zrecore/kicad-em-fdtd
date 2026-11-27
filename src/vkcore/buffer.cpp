#include "vkcore/buffer.hpp"

#include "vkcore/vk_check.hpp"

namespace vkcore {
namespace {

constexpr VkBufferUsageFlags kUsage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

void make(Context& ctx, VkDeviceSize bytes, VkMemoryPropertyFlags wanted, VkBuffer& buffer,
          VkDeviceMemory& memory, VkMemoryPropertyFlags& flags) {
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = bytes;
  bci.usage = kUsage;
  VK_CHECK(vkCreateBuffer(ctx.device(), &bci, nullptr, &buffer));
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(ctx.device(), buffer, &req);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  try {
    mai.memoryTypeIndex = ctx.findMemoryType(req.memoryTypeBits, wanted);
  } catch (...) {
    vkDestroyBuffer(ctx.device(), buffer, nullptr);
    buffer = VK_NULL_HANDLE;
    throw;
  }
  flags = ctx.memoryTypeFlags(mai.memoryTypeIndex);
  VK_CHECK(vkAllocateMemory(ctx.device(), &mai, nullptr, &memory));
  VK_CHECK(vkBindBufferMemory(ctx.device(), buffer, memory, 0));
}

constexpr VkMemoryPropertyFlags kHostVisible =
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

} // namespace

Buffer::Buffer(Context& ctx, VkDeviceSize bytes, Residency residency)
    : ctx_(ctx), device_(ctx.device()), bytes_(bytes) {
  if (residency == Residency::Device && !ctx.unifiedMemory()) {
    // VRAM for the kernels, host-visible staging for the host side.
    VkMemoryPropertyFlags stagingFlags = 0;
    make(ctx, bytes, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, buffer_, memory_, memoryFlags_);
    make(ctx, bytes, kHostVisible, staging_, stagingMemory_, stagingFlags);
    VK_CHECK(vkMapMemory(device_, stagingMemory_, 0, VK_WHOLE_SIZE, 0, &mapped_));
    ctx_.noteBuffer(static_cast<std::int64_t>(bytes_));
    return;
  }
  // Host-visible; on unified memory prefer the device-local type so the kernels see the fast path.
  try {
    if (residency == Residency::Device)
      make(ctx, bytes, kHostVisible | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, buffer_, memory_, memoryFlags_);
    else
      make(ctx, bytes, kHostVisible, buffer_, memory_, memoryFlags_);
  } catch (const std::runtime_error&) {
    make(ctx, bytes, kHostVisible, buffer_, memory_, memoryFlags_);
  }
  VK_CHECK(vkMapMemory(device_, memory_, 0, VK_WHOLE_SIZE, 0, &mapped_));
  ctx_.noteBuffer(static_cast<std::int64_t>(bytes_));
}

void Buffer::upload(VkDeviceSize offset, VkDeviceSize size) const {
  if (!staged())
    return;
  if (size == VK_WHOLE_SIZE)
    size = bytes_ - offset;
  ctx_.copyBuffer(staging_, buffer_, offset, offset, size);
}

void Buffer::download(VkDeviceSize offset, VkDeviceSize size) const {
  if (!staged())
    return;
  if (size == VK_WHOLE_SIZE)
    size = bytes_ - offset;
  ctx_.copyBuffer(buffer_, staging_, offset, offset, size);
}

Buffer::~Buffer() {
  if (memory_)
    ctx_.noteBuffer(-static_cast<std::int64_t>(bytes_));
  if (mapped_)
    vkUnmapMemory(device_, staged() ? stagingMemory_ : memory_);
  if (staging_)
    vkDestroyBuffer(device_, staging_, nullptr);
  if (stagingMemory_)
    vkFreeMemory(device_, stagingMemory_, nullptr);
  if (buffer_)
    vkDestroyBuffer(device_, buffer_, nullptr);
  if (memory_)
    vkFreeMemory(device_, memory_, nullptr);
}

} // namespace vkcore
