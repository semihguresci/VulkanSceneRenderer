#pragma once

#include "Container/common/CommonVulkan.h"

namespace container::renderer {

struct CommandBufferBeginRecordInputs {
  VkCommandBufferUsageFlags flags{0u};
};

struct SecondaryCommandBufferBeginRecordInputs {
  RenderingPassHandle renderPass{VK_NULL_HANDLE};
  RenderingTargetHandle framebuffer{VK_NULL_HANDLE};
  uint32_t subpass{0u};
  VkCommandBufferUsageFlags flags{
      VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT};
};

[[nodiscard]] bool recordCommandBufferResetCommands(
    VkCommandBuffer cmd, VkCommandBufferResetFlags flags = 0u);

[[nodiscard]] bool recordCommandBufferBeginCommands(
    VkCommandBuffer cmd, const CommandBufferBeginRecordInputs &inputs);

[[nodiscard]] bool recordSecondaryCommandBufferBeginCommands(
    VkCommandBuffer cmd, const SecondaryCommandBufferBeginRecordInputs &inputs);

[[nodiscard]] bool recordCommandBufferEndCommands(VkCommandBuffer cmd);

} // namespace container::renderer
