#pragma once

#include "Container/renderer/scene/SceneRasterPassRecorder.h"

namespace container::renderer {

struct DeferredRasterSceneGpuCullRoutePlanInputs {
  SceneRasterPassKind kind{SceneRasterPassKind::DepthPrepass};
  bool gpuCullManagerReady{false};
  bool frustumCullActive{false};
  bool frustumDrawsValid{false};
  bool occlusionCullActive{false};
  bool occlusionDrawsValid{false};
};

struct DeferredRasterSceneGpuCullRoutePlan {
  bool gpuIndirectAvailable{false};
  bool occludedGpuIndirectAvailable{false};
  bool preferOccludedGpuIndirect{false};
};

[[nodiscard]] inline DeferredRasterSceneGpuCullRoutePlan
buildDeferredRasterSceneGpuCullRoutePlan(
    const DeferredRasterSceneGpuCullRoutePlanInputs &inputs) {
  DeferredRasterSceneGpuCullRoutePlan plan{};
  plan.gpuIndirectAvailable = inputs.gpuCullManagerReady &&
                              inputs.frustumCullActive &&
                              inputs.frustumDrawsValid;
  plan.occludedGpuIndirectAvailable =
      inputs.kind == SceneRasterPassKind::GBuffer &&
      inputs.gpuCullManagerReady &&
      inputs.occlusionCullActive &&
      inputs.occlusionDrawsValid;
  plan.preferOccludedGpuIndirect =
      inputs.kind == SceneRasterPassKind::GBuffer;
  return plan;
}

} // namespace container::renderer
