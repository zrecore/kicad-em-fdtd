#include "vkcore/device_select.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <tuple>

namespace vkcore {

bool usable(const DeviceInfo& d) {
  return d.apiVersion >= ((1u << 22) | (2u << 12));
} // VK_API_VERSION_1_2

std::size_t defaultDevice(const std::vector<DeviceInfo>& devices) {
  std::size_t best = devices.size();
  const auto key = [](const DeviceInfo& d) {
    return std::tuple(d.discrete, d.apiVersion, d.deviceLocalBytes);
  };
  for (std::size_t i = 0; i < devices.size(); ++i)
    if (usable(devices[i]) && (best == devices.size() || key(devices[i]) > key(devices[best])))
      best = i;
  if (best == devices.size())
    throw std::runtime_error("no suitable Vulkan device");
  return best;
}

std::size_t selectDevice(const std::vector<DeviceInfo>& devices, const std::string& selector) {
  if (selector.empty())
    throw std::runtime_error("empty device selector (use --list-devices)");
  const auto isDigit = [](unsigned char c) { return std::isdigit(c) != 0; };
  const auto isHex = [](unsigned char c) { return std::isxdigit(c) != 0; };

  if (selector.size() < 4 && std::all_of(selector.begin(), selector.end(), isDigit)) {
    const std::size_t want = std::stoul(selector);
    std::size_t seen = 0;
    for (std::size_t i = 0; i < devices.size(); ++i)
      if (usable(devices[i]) && seen++ == want)
        return i;
    throw std::runtime_error("no usable device with index " + selector + " (use --list-devices)");
  }
  if (selector.size() < 4 || !std::all_of(selector.begin(), selector.end(), isHex))
    throw std::runtime_error("device selector '" + selector +
                             "' is neither an index nor a UUID prefix of at least four hex digits");
  std::string prefix = selector;
  std::transform(prefix.begin(), prefix.end(), prefix.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  std::size_t found = devices.size();
  for (std::size_t i = 0; i < devices.size(); ++i) {
    if (!usable(devices[i]) || devices[i].uuid.compare(0, prefix.size(), prefix) != 0)
      continue;
    if (found != devices.size())
      throw std::runtime_error("device selector '" + selector + "' is ambiguous; give more UUID digits");
    found = i;
  }
  if (found == devices.size())
    throw std::runtime_error("no usable device matches '" + selector + "' (use --list-devices)");
  return found;
}

} // namespace vkcore
