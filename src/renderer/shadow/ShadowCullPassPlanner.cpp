#include "Container/renderer/shadow/ShadowCullPassPlanner.h"

namespace container::renderer {

namespace {

[[nodiscard]] RenderPassReadiness ready() { return {}; }

[[nodiscard]] RenderPassReadiness notNeeded() {
  RenderPassReadiness readiness{};
  readiness.ready = false;
  readiness.skipReason = RenderPassSkipReason::NotNeeded;
  return readiness;
}

[[nodiscard]] RenderPassReadiness missingCameraBuffer() {
  RenderPassReadiness readiness{};
  readiness.ready = false;
  readiness.skipReason = RenderPassSkipReason::MissingResource;
  readiness.blockingResource = RenderResourceId::CameraBuffer;
  return readiness;
}

} // namespace

ShadowCullPassPlan
buildShadowCullPassPlan(const ShadowCullPassPlanInputs &inputs) {
  ShadowCullPassPlan plan{};
  if (!inputs.shadowAtlasVisible || !inputs.gpuShadowCullEnabled ||
      !inputs.shadowCullManagerReady ||
      !inputs.sceneSingleSidedDrawsAvailable ||
      !inputs.sourceDrawCommandsAllSingleInstance ||
      inputs.sourceDrawCount == 0u || !inputs.cascadeIndexInRange) {
    plan.readiness = notNeeded();
    return plan;
  }

  if (!inputs.cameraBufferReady) {
    plan.readiness = missingCameraBuffer();
    return plan;
  }

  plan.active = true;
  plan.readiness = ready();
  plan.drawCount = inputs.sourceDrawCount;
  return plan;
}

} // namespace container::renderer
