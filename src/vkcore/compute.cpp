#include "vkcore/compute.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <vector>

#include "vkcore/vk_check.hpp"

namespace vkcore {
namespace {

std::vector<std::uint32_t> loadSpirv(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f)
    throw std::runtime_error("cannot open " + path);
  std::vector<std::uint32_t> code(static_cast<std::size_t>(f.tellg()) / 4);
  f.seekg(0);
  f.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(code.size() * 4));
  return code;
}

} // namespace

ComputePipeline::ComputePipeline(Context& ctx, const std::string& spvPath, std::uint32_t bufferCount,
                                 std::uint32_t pushBytes)
    : ctx_(ctx), pushBytes_(pushBytes) {
  VkDevice dev = ctx.device();
  const auto code = loadSpirv(spvPath);
  VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  smci.codeSize = code.size() * 4;
  smci.pCode = code.data();
  VK_CHECK(vkCreateShaderModule(dev, &smci, nullptr, &shader_));

  std::vector<VkDescriptorSetLayoutBinding> bindings(bufferCount);
  for (std::uint32_t i = 0; i < bufferCount; ++i)
    bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dslci.bindingCount = bufferCount;
  dslci.pBindings = bindings.data();
  VK_CHECK(vkCreateDescriptorSetLayout(dev, &dslci, nullptr, &setLayout_));

  VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes};
  VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &setLayout_;
  plci.pushConstantRangeCount = pushBytes ? 1 : 0;
  plci.pPushConstantRanges = &range;
  VK_CHECK(vkCreatePipelineLayout(dev, &plci, nullptr, &layout_));

  VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                nullptr,
                0,
                VK_SHADER_STAGE_COMPUTE_BIT,
                shader_,
                "main",
                nullptr};
  cpci.layout = layout_;
  VK_CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline_));

  VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bufferCount};
  VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpci.maxSets = 1;
  dpci.poolSizeCount = 1;
  dpci.pPoolSizes = &ps;
  VK_CHECK(vkCreateDescriptorPool(dev, &dpci, nullptr, &pool_));
  VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dsai.descriptorPool = pool_;
  dsai.descriptorSetCount = 1;
  dsai.pSetLayouts = &setLayout_;
  VK_CHECK(vkAllocateDescriptorSets(dev, &dsai, &set_));

  VkQueryPoolCreateInfo qpci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
  qpci.queryCount = 2;
  VK_CHECK(vkCreateQueryPool(dev, &qpci, nullptr, &queries_));

  VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cpi.queueFamilyIndex = ctx.queueFamily();
  VK_CHECK(vkCreateCommandPool(dev, &cpi, nullptr, &cmdPool_));
  VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cbai.commandPool = cmdPool_;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  VK_CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd_));
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VK_CHECK(vkCreateFence(dev, &fci, nullptr, &fence_));
}

ComputePipeline::~ComputePipeline() {
  VkDevice dev = ctx_.device();
  vkDestroyFence(dev, fence_, nullptr);
  vkDestroyCommandPool(dev, cmdPool_, nullptr);
  vkDestroyQueryPool(dev, queries_, nullptr);
  vkDestroyDescriptorPool(dev, pool_, nullptr);
  vkDestroyPipeline(dev, pipeline_, nullptr);
  vkDestroyPipelineLayout(dev, layout_, nullptr);
  vkDestroyDescriptorSetLayout(dev, setLayout_, nullptr);
  vkDestroyShaderModule(dev, shader_, nullptr);
}

Timing ComputePipeline::run(std::span<Buffer* const> buffers, const void* push, std::uint32_t groupsX,
                            std::uint32_t repeats) {
  const void* one[] = {push};
  return runSequence(buffers, one, groupsX, repeats);
}

Timing ComputePipeline::runSequence(std::span<Buffer* const> buffers, std::span<const void* const> pushes,
                                    std::uint32_t groupsX, std::uint32_t repeats) {
  VkDevice dev = ctx_.device();
  std::vector<VkDescriptorBufferInfo> infos(buffers.size());
  std::vector<VkWriteDescriptorSet> writes(buffers.size());
  for (std::size_t i = 0; i < buffers.size(); ++i) {
    infos[i] = {buffers[i]->handle(), 0, VK_WHOLE_SIZE};
    writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 nullptr,
                 set_,
                 static_cast<std::uint32_t>(i),
                 0,
                 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                 nullptr,
                 &infos[i],
                 nullptr};
  }
  vkUpdateDescriptorSets(dev, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

  VK_CHECK(vkResetCommandBuffer(cmd_, 0));
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VK_CHECK(vkBeginCommandBuffer(cmd_, &bi));
  vkCmdResetQueryPool(cmd_, queries_, 0, 2);
  vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries_, 0);
  vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
  vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set_, 0, nullptr);

  VkMemoryBarrier toNext{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
  for (std::uint32_t r = 0; r < repeats; ++r) {
    for (const void* push : pushes) {
      if (pushBytes_)
        vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes_, push);
      vkCmdDispatch(cmd_, groupsX, 1, 1);
      vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           0, 1, &toNext, 0, nullptr, 0, nullptr);
    }
  }
  VkMemoryBarrier toHost{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT,
                         VK_ACCESS_HOST_READ_BIT};
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost,
                       0, nullptr, 0, nullptr);
  vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_, 1);
  VK_CHECK(vkEndCommandBuffer(cmd_));

  VK_CHECK(vkResetFences(dev, 1, &fence_));
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd_;
  const auto t0 = std::chrono::steady_clock::now();
  VK_CHECK(vkQueueSubmit(ctx_.queue(), 1, &si, fence_));
  if (vkWaitForFences(dev, 1, &fence_, VK_TRUE, 10'000'000'000ull) != VK_SUCCESS) {
    std::fprintf(stderr, "vkcore: fence wait failed or timed out; context is poisoned, aborting\n");
    std::abort();
  }
  const auto t1 = std::chrono::steady_clock::now();

  std::uint64_t ts[2];
  VK_CHECK(vkGetQueryPoolResults(dev, queries_, 0, 2, sizeof(ts), ts, sizeof(std::uint64_t),
                                 VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
  Timing t;
  t.gpuMs = static_cast<double>(ts[1] - ts[0]) * ctx_.properties().limits.timestampPeriod * 1e-6;
  t.wallMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
  if (ts[1] <= ts[0]) {
    t.timestamps = false;
    t.gpuMs = t.wallMs;
  }
  return t;
}

} // namespace vkcore
