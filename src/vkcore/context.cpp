#include "vkcore/context.hpp"

#include <algorithm>
#include <cstdio>
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

// Default rule: Vulkan >= 1.2, prefer discrete, then the most device-local memory. Never "first listed".
VkPhysicalDevice pickDevice(VkInstance inst, const ContextOptions& opt) {
  std::uint32_t n = 0;
  vkEnumeratePhysicalDevices(inst, &n, nullptr);
  std::vector<VkPhysicalDevice> devices(n);
  vkEnumeratePhysicalDevices(inst, &n, devices.data());

  VkPhysicalDevice best = VK_NULL_HANDLE;
  bool bestDiscrete = false;
  VkDeviceSize bestMem = 0;
  for (VkPhysicalDevice d : devices) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(d, &p);
    if (opt.vendorId != 0) {
      if (p.vendorID == opt.vendorId && (opt.deviceId == 0 || p.deviceID == opt.deviceId))
        return d;
      continue;
    }
    if (p.apiVersion < VK_API_VERSION_1_2)
      continue;
    const bool discrete = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    const VkDeviceSize mem = deviceLocalBytes(d);
    if (best == VK_NULL_HANDLE || (discrete && !bestDiscrete) ||
        (discrete == bestDiscrete && mem > bestMem)) {
      best = d;
      bestDiscrete = discrete;
      bestMem = mem;
    }
  }
  if (best == VK_NULL_HANDLE)
    throw std::runtime_error("no suitable Vulkan device");
  return best;
}

} // namespace

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
