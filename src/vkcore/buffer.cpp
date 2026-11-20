#include "vkcore/buffer.hpp"

#include "vkcore/vk_check.hpp"

namespace vkcore {

Buffer::Buffer(Context& ctx, VkDeviceSize bytes) : device_(ctx.device()), bytes_(bytes) {
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = bytes;
  bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
              VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VK_CHECK(vkCreateBuffer(device_, &bci, nullptr, &buffer_));
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(device_, buffer_, &req);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = ctx.findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  memoryFlags_ = ctx.memoryTypeFlags(mai.memoryTypeIndex);
  VK_CHECK(vkAllocateMemory(device_, &mai, nullptr, &memory_));
  VK_CHECK(vkBindBufferMemory(device_, buffer_, memory_, 0));
  VK_CHECK(vkMapMemory(device_, memory_, 0, VK_WHOLE_SIZE, 0, &mapped_));
}

Buffer::~Buffer() {
  if (mapped_)
    vkUnmapMemory(device_, memory_);
  if (buffer_)
    vkDestroyBuffer(device_, buffer_, nullptr);
  if (memory_)
    vkFreeMemory(device_, memory_, nullptr);
}

} // namespace vkcore
