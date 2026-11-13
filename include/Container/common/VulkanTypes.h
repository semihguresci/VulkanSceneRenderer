#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "Container/common/CommonVulkan.h"

namespace container::gpu {

// Vulkan queue family indices discovered during physical device selection.
// Shared between VulkanDevice, SwapChainManager, and other subsystems.
struct QueueFamilyIndices {
  // The renderer records both graphics and compute commands on this family.
  std::optional<uint32_t> graphicsFamily;
  std::optional<uint32_t> presentFamily;

  [[nodiscard]] bool isComplete() const {
    return graphicsFamily.has_value() && presentFamily.has_value();
  }
};

// Swap-chain capabilities, formats, and present modes for a physical device.
struct SwapChainSupportDetails {
  VkSurfaceCapabilitiesKHR capabilities{};
  std::vector<VkSurfaceFormatKHR> formats;
  std::vector<VkPresentModeKHR> presentModes;
};

}  // namespace container::gpu
