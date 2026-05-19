#include "Container/renderer/shadow/ShadowCascadeGpuCullPlanner.h"

namespace container::renderer {

ShadowCascadeGpuCullPlan
buildShadowCascadeGpuCullPlan(const ShadowCascadeGpuCullPlanInputs &inputs) {
  ShadowCascadeGpuCullPlan plan{};
  plan.useGpuCull = inputs.gpuShadowCullEnabled &&
                    inputs.shadowCullPassActive &&
                    inputs.shadowCullManagerReady &&
                    inputs.shadowCullDispatchReady &&
                    inputs.sceneSingleSidedDrawsAvailable &&
                    inputs.sourceDrawCommandsAllSingleInstance &&
                    inputs.cascadeIndexInRange &&
                    inputs.indirectDrawBuffer != VK_NULL_HANDLE &&
                    inputs.drawCountBuffer != VK_NULL_HANDLE &&
                    inputs.maxDrawCount > 0u;
  return plan;
}

ShadowGpuCullSourceUploadPlan buildShadowGpuCullSourceUploadPlan(
    const ShadowGpuCullSourceUploadPlanInputs &inputs) {
  ShadowGpuCullSourceUploadPlan plan{};
  plan.uploadSourceDrawCommands =
      inputs.shadowAtlasVisible && inputs.gpuShadowCullEnabled &&
      inputs.shadowCullManagerReady && inputs.sourceDrawCommandsPresent &&
      inputs.sourceDrawCommandsAllSingleInstance &&
      inputs.sourceDrawCount > 0u;
  if (plan.uploadSourceDrawCommands) {
    plan.requiredDrawCapacity = inputs.sourceDrawCount;
  }
  return plan;
}

} // namespace container::renderer
