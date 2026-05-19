#include "Container/renderer/deferred/DeferredRasterFrustumCullPassRecorder.h"

#include "Container/renderer/culling/GpuCullManager.h"

namespace container::renderer {

bool recordDeferredRasterFrustumCullPassCommands(
    VkCommandBuffer cmd,
    const DeferredRasterFrustumCullPassRecordInputs &inputs) {
  const DeferredRasterFrustumCullPassPlan &plan = inputs.plan;
  if (cmd == VK_NULL_HANDLE || inputs.gpuCullManager == nullptr ||
      inputs.drawCommands == nullptr || !plan.active || !plan.readiness.ready ||
      plan.drawCount == 0u || inputs.drawCommands->empty()) {
    return false;
  }

  inputs.gpuCullManager->ensureBufferCapacity(plan.drawCount);
  if (plan.updateObjectDescriptor) {
    inputs.gpuCullManager->updateObjectSsboDescriptor(inputs.imageIndex,
                                                      inputs.objectBuffer,
                                                      inputs.objectBufferSize);
  }
  inputs.gpuCullManager->uploadDrawCommands(inputs.imageIndex,
                                            *inputs.drawCommands,
                                            inputs.drawSourceRevision);

  switch (plan.freezeAction) {
  case DeferredRasterFrustumCullFreezeAction::Freeze:
    inputs.gpuCullManager->freezeCulling(inputs.imageIndex, cmd,
                                         inputs.cameraBuffer,
                                         inputs.cameraBufferSize);
    break;
  case DeferredRasterFrustumCullFreezeAction::Unfreeze:
    inputs.gpuCullManager->unfreezeCulling();
    break;
  case DeferredRasterFrustumCullFreezeAction::None:
    break;
  }

  inputs.gpuCullManager->dispatchFrustumCull(cmd, inputs.imageIndex,
                                             inputs.cameraBuffer,
                                             inputs.cameraBufferSize,
                                             plan.drawCount);
  return true;
}

} // namespace container::renderer
