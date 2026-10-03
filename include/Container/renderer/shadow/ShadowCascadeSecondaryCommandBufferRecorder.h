#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/utility/SceneData.h"

#include <array>
#include <cstdint>
#include <functional>

namespace container::renderer {

struct ShadowCascadeSecondaryCommandBufferPlanInputs {
  bool secondaryCommandBuffersEnabled{false};
  std::array<bool, container::gpu::kShadowCascadeCount> cascadePassActive{};
  std::array<bool, container::gpu::kShadowCascadeCount>
      useSecondaryCommandBuffer{};
  std::array<VkCommandBuffer, container::gpu::kShadowCascadeCount>
      commandBuffers{};
};

struct ShadowCascadeSecondaryCommandBufferRecordPlan {
  std::array<uint32_t, container::gpu::kShadowCascadeCount> cascadeIndices{};
  std::array<VkCommandBuffer, container::gpu::kShadowCascadeCount>
      commandBuffers{};
  uint32_t cascadeCount{0u};

  [[nodiscard]] bool empty() const { return cascadeCount == 0u; }
};

struct ShadowCascadeSecondaryCommandBufferCommands {
  VkCommandBuffer commandBuffer{VK_NULL_HANDLE};
  RenderingPassHandle renderPass{VK_NULL_HANDLE};
  RenderingTargetHandle framebuffer{VK_NULL_HANDLE};
  std::function<void(VkCommandBuffer)> recordBody{};
};

using ShadowCascadeSecondaryCommandBufferRecordCallback =
    std::function<void(VkCommandBuffer, uint32_t)>;

[[nodiscard]] ShadowCascadeSecondaryCommandBufferRecordPlan
buildShadowCascadeSecondaryCommandBufferRecordPlan(
    const ShadowCascadeSecondaryCommandBufferPlanInputs &inputs);

void recordShadowCascadeSecondaryCommandBufferCommands(
    const ShadowCascadeSecondaryCommandBufferCommands &commands);

void recordShadowCascadeSecondaryCommandBufferPlan(
    const ShadowCascadeSecondaryCommandBufferRecordPlan &plan,
    const ShadowCascadeSecondaryCommandBufferRecordCallback &recordCascade);

} // namespace container::renderer
