#include "Container/renderer/shadow/ShadowCullPassRecorder.h"

#include "Container/renderer/shadow/ShadowCullManager.h"

namespace container::renderer {

bool recordShadowCullPassCommands(
    VkCommandBuffer cmd, const ShadowCullPassRecordInputs &inputs) {
  if (cmd == VK_NULL_HANDLE || inputs.shadowCullManager == nullptr ||
      !inputs.plan.active || !inputs.plan.readiness.ready ||
      inputs.plan.drawCount == 0u) {
    return false;
  }

  return inputs.shadowCullManager->dispatchCascadeCull(
      cmd, inputs.imageIndex, inputs.cascadeIndex, inputs.plan.drawCount);
}

} // namespace container::renderer
