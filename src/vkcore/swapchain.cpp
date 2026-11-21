#include "vkcore/swapchain.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "vkcore/vk_check.hpp"

namespace vkcore {

Swapchain::Swapchain(Context& ctx, VkSurfaceKHR surface) : ctx_(ctx), surface_(surface) {
  VkBool32 supported = VK_FALSE;
  VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(ctx.physical(), ctx.queueFamily(), surface, &supported));
  if (!supported)
    throw std::runtime_error("the compute queue family cannot present to this surface");
  VkDevice dev = ctx.device();
  VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cpi.queueFamilyIndex = ctx.queueFamily();
  VK_CHECK(vkCreateCommandPool(dev, &cpi, nullptr, &pool_));
  VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cbai.commandPool = pool_;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  VK_CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd_));
  VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  VK_CHECK(vkCreateSemaphore(dev, &sci, nullptr, &acquired_));
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  VK_CHECK(vkCreateFence(dev, &fci, nullptr, &fence_));
  create();
}

Swapchain::~Swapchain() {
  VkDevice dev = ctx_.device();
  vkDeviceWaitIdle(dev);
  destroy();
  if (renderPass_)
    vkDestroyRenderPass(dev, renderPass_, nullptr);
  vkDestroyFence(dev, fence_, nullptr);
  vkDestroySemaphore(dev, acquired_, nullptr);
  vkDestroyCommandPool(dev, pool_, nullptr);
}

void Swapchain::create() {
  VkSurfaceCapabilitiesKHR caps;
  VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx_.physical(), surface_, &caps));
  extent_ = caps.currentExtent;
  if (extent_.width == 0xFFFFFFFFu) // the surface lets us choose
    extent_ = caps.minImageExtent;
  if (extent_.width == 0 || extent_.height == 0)
    return; // minimised: nothing to create yet

  std::uint32_t nf = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_.physical(), surface_, &nf, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(nf);
  vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_.physical(), surface_, &nf, formats.data());
  VkSurfaceFormatKHR chosen = formats.at(0);
  for (const auto& f : formats) // prefer a plain 8-bit RGBA/BGRA UNORM format: frames are already sRGB-coded
    if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) {
      chosen = f;
      break;
    }
  format_ = chosen.format;
  if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
    throw std::runtime_error("the surface does not support transfer-destination images");
  overlayCapable_ = (caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;

  VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  sci.surface = surface_;
  sci.minImageCount = std::max(caps.minImageCount, 2u);
  if (caps.maxImageCount)
    sci.minImageCount = std::min(sci.minImageCount, caps.maxImageCount);
  sci.imageFormat = format_;
  sci.imageColorSpace = chosen.colorSpace;
  sci.imageExtent = extent_;
  sci.imageArrayLayers = 1;
  sci.imageUsage =
      VK_IMAGE_USAGE_TRANSFER_DST_BIT | (overlayCapable_ ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0u);
  sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  sci.preTransform = caps.currentTransform;
  sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  sci.presentMode = VK_PRESENT_MODE_FIFO_KHR; // always available; vsync
  sci.clipped = VK_TRUE;
  sci.oldSwapchain = VK_NULL_HANDLE;
  VK_CHECK(vkCreateSwapchainKHR(ctx_.device(), &sci, nullptr, &swapchain_));
  std::uint32_t n = 0;
  vkGetSwapchainImagesKHR(ctx_.device(), swapchain_, &n, nullptr);
  images_.resize(n);
  vkGetSwapchainImagesKHR(ctx_.device(), swapchain_, &n, images_.data());
  minImages_ = sci.minImageCount;
  done_.resize(n);
  for (VkSemaphore& d : done_) {
    VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VK_CHECK(vkCreateSemaphore(ctx_.device(), &semi, nullptr, &d));
  }

  if (!overlayCapable_)
    return;
  VkDevice dev = ctx_.device();
  if (!renderPass_) { // the format does not change when the swapchain is recreated
    VkAttachmentDescription att{};
    att.format = format_;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; // the frame is already in the image
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1;
    rpi.pAttachments = &att;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    rpi.dependencyCount = 1;
    rpi.pDependencies = &dep;
    VK_CHECK(vkCreateRenderPass(dev, &rpi, nullptr, &renderPass_));
  }
  views_.resize(n);
  framebuffers_.resize(n);
  for (std::uint32_t i = 0; i < n; ++i) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = images_[i];
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format_;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(dev, &vi, nullptr, &views_[i]));
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = renderPass_;
    fi.attachmentCount = 1;
    fi.pAttachments = &views_[i];
    fi.width = extent_.width;
    fi.height = extent_.height;
    fi.layers = 1;
    VK_CHECK(vkCreateFramebuffer(dev, &fi, nullptr, &framebuffers_[i]));
  }
}

void Swapchain::destroy() {
  for (VkSemaphore d : done_)
    vkDestroySemaphore(ctx_.device(), d, nullptr);
  done_.clear();
  for (VkFramebuffer f : framebuffers_)
    vkDestroyFramebuffer(ctx_.device(), f, nullptr);
  for (VkImageView v : views_)
    vkDestroyImageView(ctx_.device(), v, nullptr);
  framebuffers_.clear();
  views_.clear();
  if (swapchain_)
    vkDestroySwapchainKHR(ctx_.device(), swapchain_, nullptr);
  swapchain_ = VK_NULL_HANDLE;
  images_.clear();
}

VkExtent2D Swapchain::extent() {
  VkSurfaceCapabilitiesKHR caps;
  VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx_.physical(), surface_, &caps));
  return caps.currentExtent.width == 0xFFFFFFFFu ? extent_ : caps.currentExtent;
}

bool Swapchain::present(const std::uint8_t* rgba, std::uint32_t width, std::uint32_t height,
                        const Overlay& overlay) {
  VkDevice dev = ctx_.device();
  const VkExtent2D now = extent();
  if (now.width == 0 || now.height == 0)
    return false;
  if (!swapchain_ || now.width != extent_.width || now.height != extent_.height) {
    vkDeviceWaitIdle(dev);
    destroy();
    create();
    if (!swapchain_)
      return false;
  }
  if (width != extent_.width || height != extent_.height)
    return false; // the caller renders at extent(); this frame is for an old size

  vkWaitForFences(dev, 1, &fence_, VK_TRUE, UINT64_MAX);
  const std::size_t bytes = static_cast<std::size_t>(width) * height * 4;
  if (!staging_ || stagingBytes_ < bytes) {
    staging_ = std::make_unique<Buffer>(ctx_, bytes);
    stagingBytes_ = bytes;
  }
  // The swapchain format may be BGRA: swap channels while copying into the staging buffer.
  std::uint8_t* dst = staging_->as<std::uint8_t>().data();
  if (format_ == VK_FORMAT_B8G8R8A8_UNORM) {
    for (std::size_t i = 0; i < bytes; i += 4) {
      dst[i] = rgba[i + 2];
      dst[i + 1] = rgba[i + 1];
      dst[i + 2] = rgba[i];
      dst[i + 3] = rgba[i + 3];
    }
  } else {
    std::memcpy(dst, rgba, bytes);
  }

  std::uint32_t index = 0;
  VkResult r = vkAcquireNextImageKHR(dev, swapchain_, UINT64_MAX, acquired_, VK_NULL_HANDLE, &index);
  if (r == VK_ERROR_OUT_OF_DATE_KHR)
    return false; // recreated on the next call
  VK_CHECK(r);

  vkResetFences(dev, 1, &fence_);
  VK_CHECK(vkResetCommandBuffer(cmd_, 0));
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VK_CHECK(vkBeginCommandBuffer(cmd_, &bi));
  VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  toDst.srcAccessMask = 0;
  toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toDst.srcQueueFamilyIndex = toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toDst.image = images_[index];
  toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                       0, nullptr, 1, &toDst);
  VkBufferImageCopy copy{};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {width, height, 1};
  vkCmdCopyBufferToImage(cmd_, staging_->handle(), images_[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                         &copy);
  if (overlay &&
      overlayCapable_) { // the render pass takes the image from colour-attachment layout to present
    VkImageMemoryBarrier toAtt = toDst;
    toAtt.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toAtt.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toAtt.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toAtt.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toAtt);
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = renderPass_;
    rb.framebuffer = framebuffers_[index];
    rb.renderArea = {{0, 0}, extent_};
    vkCmdBeginRenderPass(cmd_, &rb, VK_SUBPASS_CONTENTS_INLINE);
    overlay(cmd_);
    vkCmdEndRenderPass(cmd_);
  } else {
    VkImageMemoryBarrier toPresent = toDst;
    toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toPresent.dstAccessMask = 0;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &toPresent);
  }
  VK_CHECK(vkEndCommandBuffer(cmd_));

  const VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.waitSemaphoreCount = 1;
  si.pWaitSemaphores = &acquired_;
  si.pWaitDstStageMask = &wait;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd_;
  si.signalSemaphoreCount = 1;
  si.pSignalSemaphores = &done_[index];
  VK_CHECK(vkQueueSubmit(ctx_.queue(), 1, &si, fence_));
  VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  pi.waitSemaphoreCount = 1;
  pi.pWaitSemaphores = &done_[index];
  pi.swapchainCount = 1;
  pi.pSwapchains = &swapchain_;
  pi.pImageIndices = &index;
  r = vkQueuePresentKHR(ctx_.queue(), &pi);
  if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR && r != VK_ERROR_OUT_OF_DATE_KHR)
    VK_CHECK(r);
  return true;
}

} // namespace vkcore
