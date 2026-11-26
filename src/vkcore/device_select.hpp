// Choosing one GPU by a user selector (index or UUID prefix), without Vulkan types so it can be tested
// anywhere. `emfdtd --list-devices` prints what the selector matches against.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace vkcore {

struct DeviceInfo {
  std::string name;
  std::string uuid; // 32 lowercase hex digits (VkPhysicalDeviceIDProperties::deviceUUID), empty if unknown
  std::uint32_t vendorId = 0;
  std::uint32_t deviceId = 0;
  std::uint32_t apiVersion = 0;
  bool discrete = false;
  std::uint64_t deviceLocalBytes = 0;
  std::string driver; // driver name and info, if the device reports it
};

// The devices a solver run can use: Vulkan 1.2 or newer. Indices count only these, in enumeration order.
bool usable(const DeviceInfo& d);

// The device a run uses when the user names none, as an index into `devices`: among the usable devices a
// discrete GPU first, then the newest Vulkan version (so the newest driver wins when one GPU is offered
// through two drivers, e.g. KosmicKrisp 1.4 over MoltenVK 1.2), then the most device-local memory. Throws
// std::runtime_error if none is usable. Never "first listed".
std::size_t defaultDevice(const std::vector<DeviceInfo>& devices);

// Index into `devices` (all of them, not only the usable ones) that `selector` names. The selector is a
// decimal index into the usable devices (fewer than four digits) or a UUID prefix of at least four hex
// digits (case-insensitive) that matches exactly one usable device. Throws std::runtime_error otherwise.
std::size_t selectDevice(const std::vector<DeviceInfo>& devices, const std::string& selector);

} // namespace vkcore
