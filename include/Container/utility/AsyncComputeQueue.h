#pragma once

#include "Container/common/VulkanTypes.h"

#include <optional>
#include <span>
#include <string_view>

namespace container::gpu {

enum class AsyncComputeQueuePreference { DedicatedFirst, SameFamilyOnly, Disabled };

struct AsyncComputeQueueSelection {
  uint32_t familyIndex;
  uint32_t queueIndex;
  uint32_t timestampValidBits;
};

[[nodiscard]] constexpr std::optional<AsyncComputeQueuePreference>
parseAsyncComputeQueuePreference(std::string_view value) {
  if (value == "auto") return AsyncComputeQueuePreference::DedicatedFirst;
  if (value == "same-family") return AsyncComputeQueuePreference::SameFamilyOnly;
  if (value == "serialized") return AsyncComputeQueuePreference::Disabled;
  return std::nullopt;
}

[[nodiscard]] inline std::optional<AsyncComputeQueueSelection>
selectAsyncComputeQueue(
    std::span<const VkQueueFamilyProperties> families,
    const QueueFamilyIndices& renderFamilies,
    AsyncComputeQueuePreference preference) {
  if (preference == AsyncComputeQueuePreference::Disabled ||
      !renderFamilies.graphicsFamily || *renderFamilies.graphicsFamily >= families.size())
    return std::nullopt;

  const uint32_t graphics = *renderFamilies.graphicsFamily;
  const auto& graphicsFamily = families[graphics];
  constexpr VkQueueFlags graphicsCompute = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
  if (graphicsFamily.queueCount == 0 ||
      (graphicsFamily.queueFlags & graphicsCompute) != graphicsCompute)
    return std::nullopt;

  if (preference == AsyncComputeQueuePreference::DedicatedFirst) {
    for (uint32_t i = 0; i < families.size(); ++i) {
      const auto& family = families[i];
      if ((family.queueFlags & graphicsCompute) != VK_QUEUE_COMPUTE_BIT) continue;
      // Presentation owns queue zero on its family. Preserve an independent
      // writer so waiting for DDGI does not occupy the presentation queue.
      const uint32_t index = renderFamilies.presentFamily == i ? 1u : 0u;
      if (family.queueCount > index) return {{i, index, family.timestampValidBits}};
    }
  }

  if (graphicsFamily.queueCount >= 2) return {{graphics, 1, graphicsFamily.timestampValidBits}};
  return std::nullopt;
}

}  // namespace container::gpu
