#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/common/VulkanTypes.h"
#include "Container/utility/RayQuerySupport.h"
#include "Container/utility/VulkanInstance.h"

#include <vector>

namespace container::gpu {

struct DeviceCreateInfo {
  std::vector<const char*> requiredExtensions{};
  std::vector<const char*> optionalExtensions{};
  std::vector<const char*> validationLayers{};
  bool enableValidationLayers{false};
  bool enableRayQueries{false}; // Optional: never disqualifies a raster device.
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
  [[nodiscard]] const vk::raii::Device& raii() const noexcept { return ownedDevice_; }
  [[nodiscard]] VkQueue graphicsQueue() const noexcept { return graphicsQueue_; }
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

 private:
  bool isDeviceSuitable(VkPhysicalDevice device) const;
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
  VkQueue presentQueue_{VK_NULL_HANDLE};
  QueueFamilyIndices queueFamilyIndices_{};
  VkPhysicalDeviceFeatures enabledFeatures_{};
  VkPhysicalDeviceVulkan12Features enabledVulkan12Features_{};
  RayQuerySupport rayQuerySupport_{};
  bool rayQueriesEnabled_{false};
};

}  // namespace container::gpu
