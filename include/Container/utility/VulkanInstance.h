#pragma once

#include <string>
#include <vector>
#include <memory>
#include <vulkan/vulkan_raii.hpp>
#include "Container/common/CommonVulkan.h"

namespace container::gpu {

struct InstanceCreateInfo {
  std::string applicationName{"VulkanApplication"};
  uint32_t applicationVersion{VK_MAKE_VERSION(1, 0, 0)};
  std::string engineName{"VulkanEngine"};
  uint32_t engineVersion{VK_MAKE_VERSION(1, 0, 0)};
  uint32_t apiVersion{VK_API_VERSION_1_4};

  bool enableValidationLayers{false};
  std::vector<const char*> validationLayers{};
  std::vector<const char*> additionalLayers{};
  std::vector<const char*> requiredExtensions{};
  const void* next{nullptr};
  bool enableDlssUpscaling{false};
  bool enableDlssRayReconstruction{false};
  bool enableSurfaceMaintenance1{false};
};

class VulkanInstance {
 public:
  explicit VulkanInstance(const InstanceCreateInfo& createInfo);
  ~VulkanInstance();

  VulkanInstance(const VulkanInstance&) = delete;
  VulkanInstance& operator=(const VulkanInstance&) = delete;
  VulkanInstance(VulkanInstance&& other) noexcept;
  VulkanInstance& operator=(VulkanInstance&& other) noexcept;

  [[nodiscard]] VkInstance instance() const noexcept { return instance_; }
  [[nodiscard]] const vk::raii::Instance& raii() const noexcept { return ownedInstance_; }
  [[nodiscard]] const std::string& dlssUpscalingStatus() const noexcept { return dlssUpscalingStatus_; }
  [[nodiscard]] const std::string& dlssRayReconstructionStatus() const noexcept { return dlssRayReconstructionStatus_; }
  [[nodiscard]] bool khrSurfaceMaintenance1Enabled() const noexcept { return khrSurfaceMaintenance1Enabled_; }
  [[nodiscard]] bool extSurfaceMaintenance1Enabled() const noexcept { return extSurfaceMaintenance1Enabled_; }

 private:
  static bool checkValidationLayerSupport(
      const std::vector<const char*>& validationLayers);

  std::unique_ptr<vk::raii::Context> context_;
  vk::raii::Instance ownedInstance_{nullptr};
  VkInstance instance_{nullptr};
  std::string dlssUpscalingStatus_{"DLSS Vulkan extension discovery was not requested"};
  std::string dlssRayReconstructionStatus_{"DLSS Ray Reconstruction Vulkan extension discovery was not requested"};
  bool khrSurfaceMaintenance1Enabled_{false};
  bool extSurfaceMaintenance1Enabled_{false};
};

}  // namespace container::gpu
