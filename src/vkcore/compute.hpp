// A compute pipeline over N storage buffers plus push constants, and a timed dispatch.
#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>
#include <span>
#include <string>

#include "vkcore/buffer.hpp"
#include "vkcore/context.hpp"

namespace vkcore {

struct Timing {
  double gpuMs = 0;  // from timestamp queries (device time)
  double wallMs = 0; // submit to fence, includes CPU-side costs
  // False when the driver returned equal timestamps (KosmicKrisp does): gpuMs is then the wall time, which
  // includes the submit floor, so it is only meaningful for submits much longer than that.
  bool timestamps = true;
};

class ComputePipeline {
public:
  // spvPath: SPIR-V file. bufferCount: storage buffers at bindings 0..N-1. pushBytes: push constant size.
  ComputePipeline(Context& ctx, const std::string& spvPath, std::uint32_t bufferCount,
                  std::uint32_t pushBytes);
  ~ComputePipeline();
  ComputePipeline(const ComputePipeline&) = delete;
  ComputePipeline& operator=(const ComputePipeline&) = delete;

  // Records `repeats` dispatches in one submission (barrier between them) and waits with a finite timeout.
  // A timed-out wait aborts: work may still be running, so nothing is freed (see PLAN.md lessons).
  Timing run(std::span<Buffer* const> buffers, const void* push, std::uint32_t groupsX,
             std::uint32_t repeats = 1);
  // Each repeat dispatches once per entry of `pushes` in order (with a barrier between), e.g. {H phase, E
  // phase}.
  Timing runSequence(std::span<Buffer* const> buffers, std::span<const void* const> pushes,
                     std::uint32_t groupsX, std::uint32_t repeats);

private:
  Context& ctx_;
  std::uint32_t pushBytes_;
  VkShaderModule shader_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  VkDescriptorSet set_ = VK_NULL_HANDLE;
  VkQueryPool queries_ = VK_NULL_HANDLE;
  VkCommandPool cmdPool_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
};

} // namespace vkcore
