#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/deferred/DeferredRasterFrustumCullPassPlanner.h"

#include <vector>

namespace container::renderer {

class GpuCullManager;
struct DrawCommand;

struct DeferredRasterFrustumCullPassRecordInputs {
  GpuCullManager *gpuCullManager{nullptr};
  DeferredRasterFrustumCullPassPlan plan{};
  const std::vector<DrawCommand> *drawCommands{nullptr};
  uint32_t imageIndex{0};
  VkBuffer cameraBuffer{VK_NULL_HANDLE};
  VkDeviceSize cameraBufferSize{0};
  VkBuffer objectBuffer{VK_NULL_HANDLE};
  VkDeviceSize objectBufferSize{0};
  uint64_t drawSourceRevision{0};
};

[[nodiscard]] bool recordDeferredRasterFrustumCullPassCommands(
    VkCommandBuffer cmd,
    const DeferredRasterFrustumCullPassRecordInputs &inputs);

} // namespace container::renderer
