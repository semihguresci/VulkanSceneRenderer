#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/deferred/DeferredPointLightingDrawPlanner.h"

#include <array>

namespace container::renderer {

class LightingManager;
struct LightPushConstants;

struct DeferredPointLightingRecordInputs {
  const DeferredPointLightingDrawPlan *plan{nullptr};
  VkPipeline tiledPointLightPipeline{VK_NULL_HANDLE};
  VkPipeline stencilVolumePipeline{VK_NULL_HANDLE};
  VkPipeline pointLightPipeline{VK_NULL_HANDLE};
  VkPipeline pointLightStencilDebugPipeline{VK_NULL_HANDLE};
  VkPipelineLayout lightingLayout{VK_NULL_HANDLE};
  VkPipelineLayout tiledLightingLayout{VK_NULL_HANDLE};
  std::array<VkDescriptorSet, 3> pointLightingDescriptorSets{};
  std::array<VkDescriptorSet, 4> tiledLightingDescriptorSets{};
  VkExtent2D framebufferExtent{};
  LightPushConstants *lightPushConstants{nullptr};
  const LightingManager *lightingManager{nullptr};
};

[[nodiscard]] bool recordDeferredPointLightingCommands(
    VkCommandBuffer cmd, const DeferredPointLightingRecordInputs &inputs);

} // namespace container::renderer
