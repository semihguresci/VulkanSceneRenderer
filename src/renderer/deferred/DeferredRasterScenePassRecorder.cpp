#include "Container/renderer/deferred/DeferredRasterScenePassRecorder.h"

#include "Container/renderer/culling/GpuCullManager.h"
#include "Container/renderer/deferred/DeferredRasterSceneGpuCullRoutePlanner.h"

namespace container::renderer {

namespace {

DeferredRasterSceneGpuCullRoutePlan sceneOpaqueGpuCullRoutePlan(
    const DeferredRasterScenePassRecordInputs &inputs) {
  const GpuCullManager *gpuCullManager = inputs.gpuCullManager;
  const bool gpuCullManagerReady =
      gpuCullManager != nullptr && gpuCullManager->isReady();
  return buildDeferredRasterSceneGpuCullRoutePlan(
      {.kind = inputs.kind,
       .gpuCullManagerReady = gpuCullManagerReady,
       .frustumCullActive = inputs.frustumCullActive,
       .frustumDrawsValid =
           gpuCullManagerReady &&
           gpuCullManager->frustumDrawsValid(inputs.imageIndex),
       .occlusionCullActive = inputs.occlusionCullActive,
       .occlusionDrawsValid =
           gpuCullManagerReady &&
           gpuCullManager->occlusionDrawsValid(inputs.imageIndex)});
}

} // namespace

bool recordDeferredRasterScenePassCommands(
    VkCommandBuffer cmd, const DeferredRasterScenePassRecordInputs &inputs) {
  if (inputs.pushConstants == nullptr) {
    return false;
  }

  const DeferredRasterSceneGpuCullRoutePlan gpuCullRoutePlan =
      sceneOpaqueGpuCullRoutePlan(inputs);
  const bool sceneOpaqueGpuIndirectAvailable =
      gpuCullRoutePlan.gpuIndirectAvailable;
  const bool sceneOpaqueOccludedGpuIndirectAvailable =
      gpuCullRoutePlan.occludedGpuIndirectAvailable;

  const SceneRasterPassPlan sceneRasterPlan = buildSceneRasterPassPlan(
      {.kind = inputs.kind,
       .gpuIndirectAvailable = sceneOpaqueGpuIndirectAvailable,
       .occludedGpuIndirectAvailable =
           sceneOpaqueOccludedGpuIndirectAvailable,
       .preferOccludedGpuIndirect =
           gpuCullRoutePlan.preferOccludedGpuIndirect,
       .draws = inputs.draws,
       .pipelines = inputs.pipelines});

  return recordSceneRasterPassCommands(
      cmd, {.renderPass = inputs.renderPass,
            .framebuffer = inputs.framebuffer,
            .extent = inputs.extent,
            .clearValues = sceneRasterPlan.clearValues,
            .plan = &sceneRasterPlan.drawPlan,
            .geometry = inputs.geometry,
            .pipelines = sceneRasterPlan.pipelines,
            .pipelineLayout = inputs.pipelineLayout,
            .pushConstants = *inputs.pushConstants,
            .imageIndex = inputs.imageIndex,
            .debugOverlay = inputs.debugOverlay,
            .gpuCullManager = inputs.gpuCullManager,
            .diagnosticCube = inputs.diagnosticCube});
}

} // namespace container::renderer
