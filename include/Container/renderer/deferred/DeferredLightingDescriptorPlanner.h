#pragma once

#include "Container/common/CommonVulkan.h"

#include <array>

namespace container::renderer {

struct DeferredLightingDescriptorPlanInputs {
  std::array<VkDescriptorSet, 2> lightingDescriptorSets{};
  VkDescriptorSet frameLightingDescriptorSet{VK_NULL_HANDLE};
  VkDescriptorSet tiledDescriptorSet{VK_NULL_HANDLE};
  VkDescriptorSet sceneDescriptorSet{VK_NULL_HANDLE};
};

struct DeferredLightingDescriptorPlan {
  std::array<VkDescriptorSet, 3> directionalLightingDescriptorSets{};
  std::array<VkDescriptorSet, 3> pointLightingDescriptorSets{};
  std::array<VkDescriptorSet, 4> tiledLightingDescriptorSets{};
};

[[nodiscard]] DeferredLightingDescriptorPlan
buildDeferredLightingDescriptorPlan(
    const DeferredLightingDescriptorPlanInputs &inputs);

} // namespace container::renderer
