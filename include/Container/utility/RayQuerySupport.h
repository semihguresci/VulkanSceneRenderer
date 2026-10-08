#pragma once

#include <string_view>

namespace container::gpu {

// Extension names alone do not guarantee that the corresponding features work.
struct RayQuerySupport {
  bool accelerationStructureExtension{false};
  bool rayQueryExtension{false};
  bool deferredHostOperationsExtension{false};
  bool accelerationStructureFeature{false};
  bool rayQueryFeature{false};
  bool bufferDeviceAddressEnabled{false};

  [[nodiscard]] bool supported() const noexcept {
    return accelerationStructureExtension && rayQueryExtension &&
           deferredHostOperationsExtension && accelerationStructureFeature &&
           rayQueryFeature && bufferDeviceAddressEnabled;
  }
  [[nodiscard]] std::string_view unavailableReason() const noexcept {
    if (!accelerationStructureExtension)
      return "missing VK_KHR_acceleration_structure";
    if (!rayQueryExtension)
      return "missing VK_KHR_ray_query";
    if (!deferredHostOperationsExtension)
      return "missing VK_KHR_deferred_host_operations";
    if (!accelerationStructureFeature)
      return "accelerationStructure feature unavailable";
    if (!rayQueryFeature)
      return "rayQuery feature unavailable";
    if (!bufferDeviceAddressEnabled)
      return "bufferDeviceAddress not enabled";
    return {};
  }
};

} // namespace container::gpu
