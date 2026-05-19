#pragma once

#include "Container/common/CommonVulkan.h"

namespace container::renderer {

class BimManager;
struct FrameBimResources;

struct BimFrameGpuVisibilityRecordInputs {
  BimManager *manager{nullptr};
  VkCommandBuffer commandBuffer{VK_NULL_HANDLE};
  VkBuffer cameraBuffer{VK_NULL_HANDLE};
  VkDeviceSize cameraBufferSize{0};
  VkBuffer objectBuffer{VK_NULL_HANDLE};
  VkDeviceSize objectBufferSize{0};
};

void prepareBimFrameGpuVisibility(BimManager *manager);
void prepareBimFrameGpuVisibility(BimManager *manager,
                                  const FrameBimResources &bim);

[[nodiscard]] bool recordBimFrameGpuVisibilityCommands(
    const BimFrameGpuVisibilityRecordInputs &inputs);

} // namespace container::renderer
