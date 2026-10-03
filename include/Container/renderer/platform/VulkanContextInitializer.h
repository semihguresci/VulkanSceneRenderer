#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/utility/VulkanDevice.h"
#include "Container/utility/VulkanInstance.h"

#include <initializer_list>
#include <memory>
#include <vector>

namespace container::app {
struct AppConfig;
}

struct GLFWwindow;

namespace container::renderer {

// Output produced by VulkanContextInitializer::initialize().
struct VulkanContextResult {
  std::shared_ptr<container::gpu::VulkanInstance> instanceWrapper;
  vk::raii::DebugUtilsMessengerEXT ownedDebugMessenger{nullptr};
  vk::raii::SurfaceKHR ownedSurface{nullptr};
  std::shared_ptr<container::gpu::VulkanDevice>   deviceWrapper;
  VkInstance                                        instance{VK_NULL_HANDLE};
  VkDebugUtilsMessengerEXT                          debugMessenger{VK_NULL_HANDLE};
  VkSurfaceKHR                                      surface{VK_NULL_HANDLE};
  bool wireframeRasterModeSupported{false};
  bool wireframeWideLinesSupported{false};
  bool wireframeSupported{false};
};

// Handles Vulkan instance + device creation, debug messenger, surface.
// Stateless: all output returned via VulkanContextResult.
class VulkanContextInitializer {
 public:
  explicit VulkanContextInitializer(const container::app::AppConfig& config);

  // Creates instance, debug messenger, surface, and logical device.
  // requiredWindowExtensions: from windowManager->getRequiredInstanceExtensions().
  // nativeWindow: used to create the surface.
  VulkanContextResult initialize(
      const std::vector<const char*>& requiredWindowExtensions,
      GLFWwindow* nativeWindow) const;

  // Utility: find the first supported format from candidates.
  [[nodiscard]] static VkFormat findSupportedFormat(
      VkPhysicalDevice physicalDevice,
      std::initializer_list<VkFormat> candidates,
      VkImageTiling tiling,
      VkFormatFeatureFlags features);

  [[nodiscard]] static VkFormat findDepthStencilFormat(
      VkPhysicalDevice physicalDevice);

 private:
  const container::app::AppConfig& config_;
};

}  // namespace container::renderer
