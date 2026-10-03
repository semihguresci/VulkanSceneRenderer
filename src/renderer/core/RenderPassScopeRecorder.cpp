#include "Container/renderer/core/RenderPassScopeRecorder.h"

namespace container::renderer {

namespace {

[[nodiscard]] bool hasReadyRenderPassScope(
    VkCommandBuffer cmd, const RenderPassScopeRecordInputs &inputs) {
  return cmd != VK_NULL_HANDLE && inputs.renderPass != VK_NULL_HANDLE &&
         inputs.framebuffer != VK_NULL_HANDLE &&
         inputs.renderArea.extent.width > 0u &&
         inputs.renderArea.extent.height > 0u;
}

[[nodiscard]] bool hasReadySecondaryCommands(
    std::span<const VkCommandBuffer> secondaryCommands) {
  if (secondaryCommands.empty()) {
    return false;
  }
  for (VkCommandBuffer secondaryCommand : secondaryCommands) {
    if (secondaryCommand == VK_NULL_HANDLE) {
      return false;
    }
  }
  return true;
}

} // namespace

bool recordRenderPassBeginCommands(
    VkCommandBuffer cmd, const RenderPassScopeRecordInputs &inputs) {
  if (!hasReadyRenderPassScope(cmd, inputs)) {
    return false;
  }

  RenderingBeginInfo info{};
  info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  info.renderPass = inputs.renderPass;
  info.framebuffer = inputs.framebuffer;
  info.renderArea = inputs.renderArea;
  info.clearValueCount = static_cast<uint32_t>(inputs.clearValues.size());
  info.pClearValues = inputs.clearValues.data();

  beginDynamicRendering(cmd, &info, inputs.contents);
  return true;
}

bool recordRenderPassExecuteSecondaryCommands(
    VkCommandBuffer cmd, std::span<const VkCommandBuffer> secondaryCommands) {
  if (cmd == VK_NULL_HANDLE || !hasReadySecondaryCommands(secondaryCommands)) {
    return false;
  }

  vkCmdExecuteCommands(cmd, static_cast<uint32_t>(secondaryCommands.size()),
                       secondaryCommands.data());
  return true;
}

bool recordRenderPassEndCommands(VkCommandBuffer cmd) {
  if (cmd == VK_NULL_HANDLE) {
    return false;
  }

  endDynamicRendering(cmd);
  return true;
}

} // namespace container::renderer
