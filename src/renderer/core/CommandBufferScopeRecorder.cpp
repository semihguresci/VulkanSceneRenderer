#include "Container/renderer/core/CommandBufferScopeRecorder.h"

namespace container::renderer {

bool recordCommandBufferResetCommands(VkCommandBuffer cmd,
                                      VkCommandBufferResetFlags flags) {
  if (cmd == VK_NULL_HANDLE) {
    return false;
  }

  return vkResetCommandBuffer(cmd, flags) == VK_SUCCESS;
}

bool recordCommandBufferBeginCommands(
    VkCommandBuffer cmd, const CommandBufferBeginRecordInputs &inputs) {
  if (cmd == VK_NULL_HANDLE) {
    return false;
  }

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = inputs.flags;

  return vkBeginCommandBuffer(cmd, &beginInfo) == VK_SUCCESS;
}

bool recordSecondaryCommandBufferBeginCommands(
    VkCommandBuffer cmd, const SecondaryCommandBufferBeginRecordInputs &inputs) {
  if (cmd == VK_NULL_HANDLE || inputs.renderPass == VK_NULL_HANDLE ||
      inputs.framebuffer == VK_NULL_HANDLE) {
    return false;
  }

  VkCommandBufferInheritanceInfo inheritanceInfo{};
  inheritanceInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
  auto renderingInheritance = inputs.renderPass->inheritanceInfo();
  inheritanceInfo.pNext = &renderingInheritance;

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = inputs.flags;
  beginInfo.pInheritanceInfo = &inheritanceInfo;

  return vkBeginCommandBuffer(cmd, &beginInfo) == VK_SUCCESS;
}

bool recordCommandBufferEndCommands(VkCommandBuffer cmd) {
  if (cmd == VK_NULL_HANDLE) {
    return false;
  }

  return vkEndCommandBuffer(cmd) == VK_SUCCESS;
}

} // namespace container::renderer
