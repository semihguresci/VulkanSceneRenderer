#include "Container/renderer/deferred/DeferredDirectionalLightingRecorder.h"

namespace container::renderer {

namespace {

[[nodiscard]] bool hasReadyDescriptorSets(
    const std::array<VkDescriptorSet, 3> &descriptorSets) {
  for (VkDescriptorSet descriptorSet : descriptorSets) {
    if (descriptorSet == VK_NULL_HANDLE) {
      return false;
    }
  }
  return true;
}

} // namespace

bool recordDeferredDirectionalLightingCommands(
    VkCommandBuffer cmd,
    const DeferredDirectionalLightingRecordInputs &inputs) {
  if (inputs.pipeline == VK_NULL_HANDLE ||
      inputs.pipelineLayout == VK_NULL_HANDLE ||
      (inputs.ddgiDescriptorsRequired && inputs.ddgiDescriptorSet == VK_NULL_HANDLE) ||
      !hasReadyDescriptorSets(inputs.descriptorSets)) {
    return false;
  }

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, inputs.pipeline);
  vkCmdBindDescriptorSets(
      cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, inputs.pipelineLayout, 0,
      static_cast<uint32_t>(inputs.descriptorSets.size()),
      inputs.descriptorSets.data(), 0, nullptr);
  if (inputs.ddgiDescriptorsRequired)
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            inputs.pipelineLayout, 4, 1,
                            &inputs.ddgiDescriptorSet, 0, nullptr);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  return true;
}

} // namespace container::renderer
