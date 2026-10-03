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
  std::vector<const char*> requiredExtensions{};
  const void* next{nullptr};
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

 private:
  static bool checkValidationLayerSupport(
      const std::vector<const char*>& validationLayers);

  std::unique_ptr<vk::raii::Context> context_;
  vk::raii::Instance ownedInstance_{nullptr};
  VkInstance instance_{nullptr};
};

}  // namespace container::gpu
