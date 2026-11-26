#include "vkcore/context.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "vkcore/vk_check.hpp"

namespace vkcore {
namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL onValidation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                            VkDebugUtilsMessageTypeFlagsEXT,
                                            const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
  if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    ++*static_cast<std::atomic<unsigned>*>(user);
    std::fprintf(stderr, "[vulkan validation] %s\n", data->pMessage);
  }
  return VK_FALSE;
}

template <class Prop, class Fn> bool hasName(const std::vector<Prop>& items, const char* name, Fn getName) {
  return std::any_of(items.begin(), items.end(),
                     [&](const Prop& p) { return !std::strcmp(getName(p), name); });
}

std::vector<VkExtensionProperties> instanceExtensions() {
  std::uint32_t n = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> v(n);
  vkEnumerateInstanceExtensionProperties(nullptr, &n, v.data());
  return v;
}

bool layerAvailable(const char* name) {
  std::uint32_t n = 0;
  vkEnumerateInstanceLayerProperties(&n, nullptr);
  std::vector<VkLayerProperties> v(n);
  vkEnumerateInstanceLayerProperties(&n, v.data());
  return hasName(v, name, [](const VkLayerProperties& p) { return p.layerName; });
}

VkDeviceSize deviceLocalBytes(VkPhysicalDevice d) {
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(d, &mp);
  VkDeviceSize best = 0;
  for (std::uint32_t i = 0; i < mp.memoryHeapCount; ++i)
    if (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
      best = std::max(best, mp.memoryHeaps[i].size);
  return best;
}

DeviceInfo describe(VkPhysicalDevice d) {
  DeviceInfo info;
  VkPhysicalDeviceProperties p;
  vkGetPhysicalDeviceProperties(d, &p);
  info.name = p.deviceName;
  info.vendorId = p.vendorID;
  info.deviceId = p.deviceID;
  info.apiVersion = p.apiVersion;
  info.discrete = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
  info.deviceLocalBytes = deviceLocalBytes(d);
  if (p.apiVersion >= VK_API_VERSION_1_2) { // device id and driver properties are core in 1.1 and 1.2
    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    id.pNext = &drv;
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &id;
    vkGetPhysicalDeviceProperties2(d, &p2);
    char hex[3];
    for (unsigned char b : id.deviceUUID) {
      std::snprintf(hex, sizeof hex, "%02x", b);
      info.uuid += hex;
    }
    info.driver = std::string(drv.driverName) + " " + drv.driverInfo;
  }
  return info;
}

std::vector<VkPhysicalDevice> physicalDevices(VkInstance inst) {
  std::uint32_t n = 0;
  vkEnumeratePhysicalDevices(inst, &n, nullptr);
  std::vector<VkPhysicalDevice> devices(n);
  vkEnumeratePhysicalDevices(inst, &n, devices.data());
  return devices;
}

// Rule when the user names nothing: see defaultDevice (Vulkan >= 1.2, discrete, newest, most memory).
VkPhysicalDevice pickDevice(VkInstance inst, const ContextOptions& opt) {
  const std::vector<VkPhysicalDevice> devices = physicalDevices(inst);
  std::vector<DeviceInfo> infos;
  for (VkPhysicalDevice d : devices)
    infos.push_back(describe(d));
  if (!opt.device.empty())
    return devices[selectDevice(infos, opt.device)];
  if (opt.vendorId != 0) {
    for (std::size_t i = 0; i < devices.size(); ++i)
      if (infos[i].vendorId == opt.vendorId && (opt.deviceId == 0 || infos[i].deviceId == opt.deviceId))
        return devices[i];
    throw std::runtime_error("no Vulkan device with the requested vendor and device id");
  }
  return devices[defaultDevice(infos)];
}

} // namespace

std::vector<DeviceInfo> listDevices() {
  const auto exts = instanceExtensions();
  std::vector<const char*> instExt;
  VkInstanceCreateFlags flags = 0;
  if (hasName(exts, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
              [](const VkExtensionProperties& p) { return p.extensionName; })) {
    instExt.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
  }
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_2;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.flags = flags;
  ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = static_cast<std::uint32_t>(instExt.size());
  ici.ppEnabledExtensionNames = instExt.data();
  VkInstance inst = VK_NULL_HANDLE;
  VK_CHECK(vkCreateInstance(&ici, nullptr, &inst));
  std::vector<DeviceInfo> out;
  for (VkPhysicalDevice d : physicalDevices(inst))
    out.push_back(describe(d));
  vkDestroyInstance(inst, nullptr);
  return out;
}

Context::Context(const ContextOptions& options) {
  const auto exts = instanceExtensions();
  auto has = [&](const char* name) {
    return hasName(exts, name, [](const VkExtensionProperties& p) { return p.extensionName; });
  };

  std::vector<const char*> instExt, layers;
  VkInstanceCreateFlags flags = 0;
  if (has(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) { // MoltenVK is a portability implementation
    instExt.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
  }
  for (const char* e : options.instanceExtensions)
    if (has(e))
      instExt.push_back(e);
    else
      throw std::runtime_error(std::string("required Vulkan instance extension missing: ") + e);
  bool wantValidation = options.validation && layerAvailable("VK_LAYER_KHRONOS_validation") &&
                        has(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  if (wantValidation) {
    layers.push_back("VK_LAYER_KHRONOS_validation");
    instExt.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  }

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_2;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.flags = flags;
  ici.pApplicationInfo = &app;
  ici.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
  ici.ppEnabledLayerNames = layers.data();
  ici.enabledExtensionCount = static_cast<std::uint32_t>(instExt.size());
  ici.ppEnabledExtensionNames = instExt.data();
  VkResult created = vkCreateInstance(&ici, nullptr, &instance_);
  if (created == VK_ERROR_LAYER_NOT_PRESENT && wantValidation) { // manifest found but library not loadable
    wantValidation = false;
    ici.enabledLayerCount = 0;
    instExt.erase(
        std::remove(instExt.begin(), instExt.end(), std::string_view(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)),
        instExt.end());
    ici.enabledExtensionCount = static_cast<std::uint32_t>(instExt.size());
    ici.ppEnabledExtensionNames = instExt.data();
    created = vkCreateInstance(&ici, nullptr, &instance_);
  }
  VK_CHECK(created);

  if (wantValidation) {
    auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
    VkDebugUtilsMessengerCreateInfoEXT mci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    mci.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    mci.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
    mci.pfnUserCallback = onValidation;
    mci.pUserData = &validationErrors_;
    VK_CHECK(create(instance_, &mci, nullptr, &messenger_));
  }

  physical_ = pickDevice(instance_, options);
  vkGetPhysicalDeviceProperties(physical_, &props_);
  deviceLocalBytes_ = vkcore::deviceLocalBytes(physical_);
  info_ = describe(physical_);
  {
    // Unified: a memory type that is device-local, host-visible and coherent in a heap that is a large part
    // of the device memory (a discrete GPU without resizable BAR only has a 256 MB window of that kind).
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(physical_, &mp);
    constexpr VkMemoryPropertyFlags kWant = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    unified_ = false;
    for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i)
      if ((mp.memoryTypes[i].propertyFlags & kWant) == kWant &&
          mp.memoryHeaps[mp.memoryTypes[i].heapIndex].size >= deviceLocalBytes_ / 2)
        unified_ = true;
    const char* forced = std::getenv("EMFDTD_STAGED_BUFFERS");
    if (options.stagedBuffers || (forced && forced[0] == '1'))
      unified_ = false;
  }
  if (props_.apiVersion >= VK_API_VERSION_1_2) { // float controls are core in 1.2
    VkPhysicalDeviceFloatControlsProperties fc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &fc;
    vkGetPhysicalDeviceProperties2(physical_, &p2);
    auto yn = [](VkBool32 b) { return b ? "yes" : "no"; };
    floatControls_ = std::string("fp32 denormals preserved ") + yn(fc.shaderDenormPreserveFloat32) +
                     ", flushed " + yn(fc.shaderDenormFlushToZeroFloat32) +
                     ", signed zero/inf/nan preserved " + yn(fc.shaderSignedZeroInfNanPreserveFloat32) +
                     ", rounding RTE " + yn(fc.shaderRoundingModeRTEFloat32);
  }

  std::uint32_t nq = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical_, &nq, nullptr);
  std::vector<VkQueueFamilyProperties> families(nq);
  vkGetPhysicalDeviceQueueFamilyProperties(physical_, &nq, families.data());
  bool found = false;
  for (std::uint32_t i = 0; i < nq && !found; ++i)
    if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
      queueFamily_ = i;
      found = true;
    }
  if (!found)
    throw std::runtime_error("no compute queue family");

  std::uint32_t nd = 0;
  vkEnumerateDeviceExtensionProperties(physical_, nullptr, &nd, nullptr);
  std::vector<VkExtensionProperties> dexts(nd);
  vkEnumerateDeviceExtensionProperties(physical_, nullptr, &nd, dexts.data());
  std::vector<const char*> devExt;
  if (hasName(dexts, "VK_KHR_portability_subset",
              [](const VkExtensionProperties& p) { return p.extensionName; }))
    devExt.push_back("VK_KHR_portability_subset");

  if (options.swapchain) {
    if (!hasName(dexts, VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                 [](const VkExtensionProperties& p) { return p.extensionName; }))
      throw std::runtime_error("the device does not support VK_KHR_swapchain");
    devExt.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  }

  const float priority = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = queueFamily_;
  qci.queueCount = 1;
  qci.pQueuePriorities = &priority;
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = static_cast<std::uint32_t>(devExt.size());
  dci.ppEnabledExtensionNames = devExt.data();
  VK_CHECK(vkCreateDevice(physical_, &dci, nullptr, &device_));
  vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
}

Context::~Context() {
  if (copyFence_)
    vkDestroyFence(device_, copyFence_, nullptr);
  if (copyPool_)
    vkDestroyCommandPool(device_, copyPool_, nullptr);
  if (device_)
    vkDestroyDevice(device_, nullptr);
  if (messenger_) {
    auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
    destroy(instance_, messenger_, nullptr);
  }
  if (instance_)
    vkDestroyInstance(instance_, nullptr);
}

void Context::copyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize srcOffset, VkDeviceSize dstOffset,
                         VkDeviceSize size) {
  if (!copyPool_) {
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = queueFamily_;
    VK_CHECK(vkCreateCommandPool(device_, &cpi, nullptr, &copyPool_));
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = copyPool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(device_, &cbai, &copyCmd_));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CHECK(vkCreateFence(device_, &fci, nullptr, &copyFence_));
  }
  VK_CHECK(vkResetCommandBuffer(copyCmd_, 0));
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VK_CHECK(vkBeginCommandBuffer(copyCmd_, &bi));
  const VkBufferCopy region{srcOffset, dstOffset, size};
  vkCmdCopyBuffer(copyCmd_, src, dst, 1, &region);
  // Make the copy visible to later shader work and to the host (the next submit also waits on the fence).
  const VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                                    VK_ACCESS_HOST_READ_BIT};
  vkCmdPipelineBarrier(copyCmd_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0,
                       nullptr, 0, nullptr);
  VK_CHECK(vkEndCommandBuffer(copyCmd_));
  VK_CHECK(vkResetFences(device_, 1, &copyFence_));
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &copyCmd_;
  VK_CHECK(vkQueueSubmit(queue_, 1, &si, copyFence_));
  if (vkWaitForFences(device_, 1, &copyFence_, VK_TRUE, 60'000'000'000ull) != VK_SUCCESS) {
    std::fprintf(stderr, "vkcore: copy fence wait failed or timed out; context is poisoned, aborting\n");
    std::abort();
  }
}

VkMemoryPropertyFlags Context::memoryTypeFlags(std::uint32_t index) const {
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(physical_, &mp);
  return mp.memoryTypes[index].propertyFlags;
}

std::uint32_t Context::findMemoryType(std::uint32_t typeBits, VkMemoryPropertyFlags wanted) const {
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(physical_, &mp);
  for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i)
    if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & wanted) == wanted)
      return i;
  throw std::runtime_error("no suitable memory type");
}

} // namespace vkcore
