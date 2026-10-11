#pragma once

#include "Container/common/CommonVulkan.h"

#include <array>

namespace container::renderer {

struct DeferredDirectionalLightingRecordInputs {
  VkPipeline pipeline{VK_NULL_HANDLE};
  VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
  std::array<VkDescriptorSet, 3> descriptorSets{};
  VkDescriptorSet ddgiDescriptorSet{VK_NULL_HANDLE};
  bool ddgiDescriptorsRequired{true};
};

[[nodiscard]] bool recordDeferredDirectionalLightingCommands(
    VkCommandBuffer cmd,
    const DeferredDirectionalLightingRecordInputs &inputs);

} // namespace container::renderer
