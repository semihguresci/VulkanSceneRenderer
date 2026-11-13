#pragma once

#include "Container/common/VulkanTypes.h"

#include <cstdint>
#include <span>

namespace container::gpu {

struct QueueFamilySupport {
  VkQueueFlags flags;
  uint32_t queueCount;
  bool present;
};

[[nodiscard]] inline QueueFamilyIndices selectQueueFamilies(
    std::span<const QueueFamilySupport> families) {
  constexpr VkQueueFlags required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
  QueueFamilyIndices fallback{};
  for (uint32_t i = 0; i < families.size(); ++i) {
    const auto& family = families[i];
    if (family.queueCount == 0) continue;

    if ((family.flags & required) == required) {
      if (family.present) return {i, i};
      if (!fallback.graphicsFamily) fallback.graphicsFamily = i;
    }
    if (family.present && !fallback.presentFamily) fallback.presentFamily = i;
  }
  return fallback;
}

}  // namespace container::gpu
