#include "Container/renderer/scene/SceneOpaqueDrawRecorder.h"

#include "Container/renderer/culling/GpuCullManager.h"

#include <limits>

namespace container::renderer {

namespace {

constexpr uint32_t kIndirectObjectIndex = std::numeric_limits<uint32_t>::max();

[[nodiscard]] bool hasDrawCommands(const std::vector<DrawCommand> *commands) {
  return commands != nullptr && !commands->empty();
}

[[nodiscard]] bool hasRequiredInputs(
    const SceneOpaqueDrawRecordInputs &inputs) {
  return inputs.plan != nullptr && inputs.pipelineLayout != VK_NULL_HANDLE &&
         inputs.debugOverlay != nullptr;
}

[[nodiscard]] bool hasReadyGeometry(
    const SceneOpaqueDrawGeometryBinding &geometry) {
  return geometry.descriptorSet != VK_NULL_HANDLE &&
         geometry.vertexSlice.buffer != VK_NULL_HANDLE &&
         geometry.indexSlice.buffer != VK_NULL_HANDLE;
}

[[nodiscard]] VkPipeline pipelineForSceneOpaqueRoute(
    const SceneOpaqueDrawRoute &route,
    const SceneOpaqueDrawPipelineHandles &pipelines) {
  switch (route.pipeline) {
  case SceneOpaqueDrawPipeline::Primary:
    return pipelines.primary;
  case SceneOpaqueDrawPipeline::FrontCull:
    return pipelines.frontCull;
  case SceneOpaqueDrawPipeline::NoCull:
    return pipelines.noCull;
  }
  return pipelines.primary;
}

[[nodiscard]] bool hasRecordableGpuIndirectRoute(
    const SceneOpaqueDrawRecordInputs &inputs) {
  return inputs.plan->useGpuIndirectSingleSided &&
         inputs.gpuCullManager != nullptr &&
         inputs.pipelines.primary != VK_NULL_HANDLE &&
         hasDrawCommands(inputs.plan->gpuIndirectRoute.commands);
}

[[nodiscard]] bool hasRecordableCpuRoute(
    const SceneOpaqueDrawRecordInputs &inputs) {
  for (uint32_t routeIndex = 0u; routeIndex < inputs.plan->cpuRouteCount;
       ++routeIndex) {
    const SceneOpaqueDrawRoute &route = inputs.plan->cpuRoutes[routeIndex];
    if (hasDrawCommands(route.commands) &&
        pipelineForSceneOpaqueRoute(route, inputs.pipelines) !=
            VK_NULL_HANDLE) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool hasRecordableRoute(
    const SceneOpaqueDrawRecordInputs &inputs) {
  return hasRecordableGpuIndirectRoute(inputs) ||
         hasRecordableCpuRoute(inputs);
}

void bindGeometry(VkCommandBuffer cmd,
                  const SceneOpaqueDrawGeometryBinding &geometry,
                  VkPipelineLayout layout) {
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1,
                          &geometry.descriptorSet, 0, nullptr);
  const VkDeviceSize offsets[] = {geometry.vertexSlice.offset};
  vkCmdBindVertexBuffers(cmd, 0, 1, &geometry.vertexSlice.buffer, offsets);
  vkCmdBindIndexBuffer(cmd, geometry.indexSlice.buffer,
                       geometry.indexSlice.offset, geometry.indexType);
}

void pushSceneObjectIndex(VkCommandBuffer cmd, VkPipelineLayout layout,
                          container::gpu::BindlessPushConstants &pc,
                          uint32_t objectIndex) {
  pc.objectIndex = objectIndex;
  vkCmdPushConstants(cmd, layout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, sizeof(container::gpu::BindlessPushConstants), &pc);
}

void drawGpuIndirectRoute(VkCommandBuffer cmd,
                          const SceneOpaqueDrawRoute &route,
                          uint32_t imageIndex,
                          const GpuCullManager &gpuCullManager) {
  switch (route.indirectSource) {
  case SceneOpaqueIndirectSource::FrustumCull:
    gpuCullManager.drawIndirect(cmd, imageIndex);
    return;
  case SceneOpaqueIndirectSource::OcclusionCull:
    gpuCullManager.drawIndirectOccluded(cmd, imageIndex);
    return;
  }
  gpuCullManager.drawIndirect(cmd, imageIndex);
}

} // namespace

bool recordSceneOpaqueDrawCommands(VkCommandBuffer cmd,
                                   const SceneOpaqueDrawRecordInputs &inputs) {
  if (!hasRequiredInputs(inputs) || !hasRecordableRoute(inputs) ||
      !hasReadyGeometry(inputs.geometry)) {
    return false;
  }

  bindGeometry(cmd, inputs.geometry, inputs.pipelineLayout);
  bool recorded = false;
  container::gpu::BindlessPushConstants pushConstants = inputs.pushConstants;

  if (hasRecordableGpuIndirectRoute(inputs)) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      inputs.pipelines.primary);
    pushSceneObjectIndex(cmd, inputs.pipelineLayout, pushConstants,
                         kIndirectObjectIndex);
    drawGpuIndirectRoute(cmd, inputs.plan->gpuIndirectRoute,
                         inputs.imageIndex,
                         *inputs.gpuCullManager);
    recorded = true;
  }

  for (uint32_t routeIndex = 0u; routeIndex < inputs.plan->cpuRouteCount;
       ++routeIndex) {
    const SceneOpaqueDrawRoute &route = inputs.plan->cpuRoutes[routeIndex];
    const VkPipeline pipeline =
        pipelineForSceneOpaqueRoute(route, inputs.pipelines);
    if (!hasDrawCommands(route.commands) || pipeline == VK_NULL_HANDLE) {
      continue;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    inputs.debugOverlay->drawScene(cmd, inputs.pipelineLayout, *route.commands,
                                   pushConstants);
    recorded = true;
  }
  return recorded;
}

} // namespace container::renderer
