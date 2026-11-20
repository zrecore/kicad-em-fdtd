// The one place Vulkan result codes become exceptions. Setup code throws; hot loops check results.
#pragma once
#include <vulkan/vulkan.h>

#include <stdexcept>
#include <string>

namespace vkcore {

class VkError : public std::runtime_error {
public:
  VkError(VkResult result, const char* expr, const char* file, int line)
      : std::runtime_error(std::string(file) + ":" + std::to_string(line) + " " + expr + " -> " +
                           std::to_string(static_cast<int>(result))),
        result_(result) {}
  VkResult result() const { return result_; }

private:
  VkResult result_;
};

} // namespace vkcore

#define VK_CHECK(expr)                                                                                       \
  do {                                                                                                       \
    const VkResult vk_check_r_ = (expr);                                                                     \
    if (vk_check_r_ != VK_SUCCESS) {                                                                         \
      throw ::vkcore::VkError(vk_check_r_, #expr, __FILE__, __LINE__);                                       \
    }                                                                                                        \
  } while (0)
