#include "Container/renderer/deferred/DeferredRasterTileCullRecorder.h"

#include "Container/renderer/lighting/LightingManager.h"

namespace container::renderer {

bool recordDeferredRasterTileCullCommands(
    VkCommandBuffer cmd,
    const DeferredRasterTileCullRecordInputs &inputs) {
  const DeferredRasterTileCullPlan &plan = inputs.plan;
  if (cmd == VK_NULL_HANDLE || inputs.lightingManager == nullptr ||
      !plan.active || !plan.readiness.ready) {
    return false;
  }

  inputs.lightingManager->beginClusterCullTimer(cmd);
  inputs.lightingManager->dispatchTileCull(
      cmd, inputs.imageIndex, plan.screenExtent, plan.cameraBuffer, plan.cameraBufferSize,
      plan.depthSamplingView, plan.cameraNear, plan.cameraFar);
  inputs.lightingManager->endClusterCullTimer(cmd);
  return true;
}

} // namespace container::renderer
