#include "Container/renderer/scene/SceneRasterPassPlanner.h"

namespace container::renderer {

namespace {

[[nodiscard]] VkPipeline choosePipeline(VkPipeline preferred,
                                        VkPipeline fallback) {
  return preferred != VK_NULL_HANDLE ? preferred : fallback;
}

} // namespace

SceneRasterPassPlan
buildSceneRasterPassPlan(const SceneRasterPassPlanInputs &inputs) {
  SceneRasterPassPlan plan{};
  plan.clearValues = sceneRasterPassClearValues(inputs.kind);
  plan.drawPlan = buildSceneOpaqueDrawPlan(
      {.gpuIndirectAvailable = inputs.gpuIndirectAvailable,
       .occludedGpuIndirectAvailable = inputs.occludedGpuIndirectAvailable,
       .preferOccludedGpuIndirect = inputs.preferOccludedGpuIndirect,
       .draws = inputs.draws});
  plan.pipelines = {.primary = inputs.pipelines.primary,
                    .frontCull = choosePipeline(inputs.pipelines.frontCull,
                                                inputs.pipelines.primary),
                    .noCull = choosePipeline(inputs.pipelines.noCull,
                                             inputs.pipelines.primary)};
  return plan;
}

} // namespace container::renderer
