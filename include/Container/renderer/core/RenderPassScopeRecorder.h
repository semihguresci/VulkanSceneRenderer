#pragma once

#include "Container/common/CommonVulkan.h"

#include <span>

namespace container::renderer {

struct RenderPassScopeRecordInputs {
  RenderingPassHandle renderPass{VK_NULL_HANDLE};
  RenderingTargetHandle framebuffer{VK_NULL_HANDLE};
  VkRect2D renderArea{};
  std::span<const VkClearValue> clearValues{};
  VkSubpassContents contents{VK_SUBPASS_CONTENTS_INLINE};
};

[[nodiscard]] bool recordRenderPassBeginCommands(
    VkCommandBuffer cmd, const RenderPassScopeRecordInputs &inputs);

[[nodiscard]] bool recordRenderPassExecuteSecondaryCommands(
    VkCommandBuffer cmd, std::span<const VkCommandBuffer> secondaryCommands);

[[nodiscard]] bool recordRenderPassEndCommands(VkCommandBuffer cmd);

} // namespace container::renderer
