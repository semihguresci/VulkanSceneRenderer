#pragma once

#include "Container/common/CommonVulkan.h"
#include <optional>
#include <span>

namespace container::gpu {
// Sharing the graphics family preserves existing exclusive scene/AS/material
// ownership. A different family requires ownership transfers for every shared
// resource and is deliberately handled by the serialized fallback instead.
[[nodiscard]] inline std::optional<uint32_t> selectAsyncComputeQueueIndex(
    std::span<const VkQueueFamilyProperties> families,
    uint32_t graphicsFamily) {
  if (graphicsFamily >= families.size())
    return {};
  const auto &family = families[graphicsFamily];
  if (family.queueCount < 2 ||
      !(family.queueFlags & VK_QUEUE_COMPUTE_BIT))
    return {};
  return 1u;
}
} // namespace container::gpu
