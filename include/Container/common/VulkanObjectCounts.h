#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>

namespace container::gpu {

// Counts objects retained by the C-handle RAII registry. Managers using
// Vulkan-Hpp RAII directly retain their own ownership.
struct VulkanObjectCounts {
  uint64_t pipelines{}, pipelineLayouts{}, pipelineCaches{};
  uint64_t descriptorPools{}, descriptorSetLayouts{};
  uint64_t imageViews{}, samplers{}, shaderModules{};
};

[[nodiscard]] VulkanObjectCounts ownedVulkanObjectCounts(VkDevice device);

} // namespace container::gpu
