#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/deferred/DeferredRasterTileCullPlanner.h"

namespace container::renderer {

class LightingManager;

struct DeferredRasterTileCullRecordInputs {
  const LightingManager *lightingManager{nullptr};
  uint32_t imageIndex{};
  DeferredRasterTileCullPlan plan{};
};

[[nodiscard]] bool recordDeferredRasterTileCullCommands(
    VkCommandBuffer cmd,
    const DeferredRasterTileCullRecordInputs &inputs);

} // namespace container::renderer
