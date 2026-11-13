#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/common/VulkanTypes.h"
#include "Container/utility/RayQuerySupport.h"
#include "Container/utility/VulkanInstance.h"

#include <optional>
#include <string>
#include <vector>

namespace container::gpu {

struct DeviceCreateInfo {
  std::vector<const char*> requiredExtensions{};
  std::vector<const char*> optionalExtensions{};
  std::vector<const char*> validationLayers{};
  bool enableValidationLayers{false};
  bool enableRayQueries{false}; // Optional: never disqualifies a raster device.
  bool enableOpacityMicromaps{false};
  bool enableReferenceOidn{false};
  bool enableFsrUpscaling{false};
  VkPhysicalDeviceFeatures enabledFeatures{};  // Required features.
  VkPhysicalDeviceFeatures optionalFeatures{}; // Enabled when supported.
  const void* next{nullptr};
};

class VulkanDevice {
 public:
  VulkanDevice(const VulkanInstance& instance, VkSurfaceKHR surface,
               const DeviceCreateInfo& createInfo);
  ~VulkanDevice();

  VulkanDevice(const VulkanDevice&) = delete;
  VulkanDevice& operator=(const VulkanDevice&) = delete;
  VulkanDevice(VulkanDevice&& other) = delete;
  VulkanDevice& operator=(VulkanDevice&& other) = delete;

  [[nodiscard]] VkPhysicalDevice physicalDevice() const noexcept {
    return physicalDevice_;
  }
  [[nodiscard]] VkDevice device() const noexcept { return device_; }
  [[nodiscard]] VkInstance instance() const noexcept { return instance_; }
  [[nodiscard]] const vk::raii::Device& raii() const noexcept { return ownedDevice_; }
  [[nodiscard]] VkQueue graphicsQueue() const noexcept { return graphicsQueue_; }
  [[nodiscard]] VkQueue asyncComputeQueue() const noexcept { return asyncComputeQueue_; }
  [[nodiscard]] VkQueue presentQueue() const noexcept { return presentQueue_; }
  [[nodiscard]] QueueFamilyIndices queueFamilyIndices() const noexcept {
    return queueFamilyIndices_;
  }
  [[nodiscard]] const VkPhysicalDeviceFeatures& enabledFeatures() const noexcept {
    return enabledFeatures_;
  }
  [[nodiscard]] const VkPhysicalDeviceVulkan12Features&
  enabledVulkan12Features() const noexcept {
    return enabledVulkan12Features_;
  }
  [[nodiscard]] const RayQuerySupport &rayQuerySupport() const noexcept {
    return rayQuerySupport_;
  }
  [[nodiscard]] bool rayQueriesEnabled() const noexcept {
    return rayQueriesEnabled_;
  }
  [[nodiscard]] bool opacityMicromapsEnabled() const noexcept { return opacityMicromapsEnabled_; }
  [[nodiscard]] bool referenceOidnEnabled() const noexcept { return referenceOidnEnabled_; }
  [[nodiscard]] bool referenceOidnCooperativeMatrix() const noexcept { return referenceOidnCooperativeMatrix_; }
  [[nodiscard]] bool fsrUpscalingEnabled() const noexcept { return fsrUpscalingEnabled_; }
  [[nodiscard]] bool dlssUpscalingEnabled() const noexcept { return dlssUpscalingStatus_.empty(); }
  [[nodiscard]] const std::string& dlssUpscalingStatus() const noexcept { return dlssUpscalingStatus_; }
  [[nodiscard]] bool dlssRayReconstructionEnabled() const noexcept { return dlssRayReconstructionStatus_.empty(); }
  [[nodiscard]] const std::string& dlssRayReconstructionStatus() const noexcept { return dlssRayReconstructionStatus_; }

 private:
  std::optional<QueueFamilyIndices> suitableDeviceQueues(VkPhysicalDevice device) const;
  bool checkDeviceExtensionSupport(VkPhysicalDevice device) const;
  bool supportsRequestedFeatures(VkPhysicalDevice device) const;

  void pickPhysicalDevice();
  void createLogicalDevice();

  VkInstance instance_{VK_NULL_HANDLE};
  const VulkanInstance& instanceOwner_;
  vk::raii::Device ownedDevice_{nullptr};
  VkSurfaceKHR surface_{VK_NULL_HANDLE};
  DeviceCreateInfo createInfo_{};

  VkPhysicalDevice physicalDevice_{VK_NULL_HANDLE};
  VkDevice device_{VK_NULL_HANDLE};
  VkQueue graphicsQueue_{VK_NULL_HANDLE};
  VkQueue asyncComputeQueue_{VK_NULL_HANDLE};
  VkQueue presentQueue_{VK_NULL_HANDLE};
  QueueFamilyIndices queueFamilyIndices_{};
  VkPhysicalDeviceFeatures enabledFeatures_{};
  VkPhysicalDeviceVulkan12Features enabledVulkan12Features_{};
  RayQuerySupport rayQuerySupport_{};
  bool rayQueriesEnabled_{false};
  bool opacityMicromapsEnabled_{false};
  bool referenceOidnEnabled_{false};
  bool referenceOidnCooperativeMatrix_{false};
  bool fsrUpscalingEnabled_{false};
  std::string dlssUpscalingStatus_;
  std::string dlssRayReconstructionStatus_;
};

}  // namespace container::gpu
