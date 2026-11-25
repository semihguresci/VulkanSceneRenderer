#include "Container/renderer/bim/BimSurfacePassRecorder.h"

#include "Container/renderer/bim/BimManager.h"

#include <limits>

namespace container::renderer {

namespace {

constexpr uint32_t kIndirectObjectIndex = std::numeric_limits<uint32_t>::max();

[[nodiscard]] bool hasDrawCommands(const std::vector<DrawCommand> *commands) {
  return commands != nullptr && !commands->empty();
}

[[nodiscard]] bool hasReadyDescriptorSets(
    std::span<const VkDescriptorSet> descriptorSets) {
  if (descriptorSets.empty()) {
    return false;
  }
  for (VkDescriptorSet descriptorSet : descriptorSets) {
    if (descriptorSet == VK_NULL_HANDLE) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool hasReadyGeometry(
    const BimSurfacePassGeometryBinding &geometry) {
  return hasReadyDescriptorSets(geometry.descriptorSets) &&
         geometry.vertexSlice.buffer != VK_NULL_HANDLE &&
         geometry.indexSlice.buffer != VK_NULL_HANDLE;
}

VkPipeline pipelineForBimSurfaceRoute(const BimSurfaceDrawRoute &route,
                                      VkPipeline singleSidedPipeline,
                                      VkPipeline windingFlippedPipeline,
                                      VkPipeline doubleSidedPipeline) {
  switch (route.kind) {
  case BimSurfaceDrawRouteKind::SingleSided:
    return singleSidedPipeline;
  case BimSurfaceDrawRouteKind::WindingFlipped:
    return windingFlippedPipeline;
  case BimSurfaceDrawRouteKind::DoubleSided:
    return doubleSidedPipeline;
  }
  return VK_NULL_HANDLE;
}

void pushSceneObjectIndex(VkCommandBuffer cmd, VkPipelineLayout layout,
                          container::gpu::BindlessPushConstants &pc,
                          uint32_t objectIndex) {
  pc.objectIndex = objectIndex;
  vkCmdPushConstants(cmd, layout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, sizeof(container::gpu::BindlessPushConstants), &pc);
}

[[nodiscard]] bool routeMayRecord(const BimSurfaceDrawRoute &route,
                                  VkPipeline pipeline,
                                  const BimSurfacePassRecordInputs &inputs) {
  if (pipeline == VK_NULL_HANDLE) {
    return false;
  }
  return (route.cpuFallbackAllowed && hasDrawCommands(route.cpuCommands)) ||
         (route.gpuCompactionAllowed && inputs.bimManager != nullptr);
}

[[nodiscard]] bool hasRecordableRoute(
    const BimSurfacePassRecordInputs &inputs) {
  if (inputs.plan == nullptr) {
    return false;
  }

  for (uint32_t sourceIndex = 0u; sourceIndex < inputs.plan->sourceCount;
       ++sourceIndex) {
    const BimSurfacePassSourcePlan &sourcePlan =
        inputs.plan->sources[sourceIndex];
    for (uint32_t routeIndex = 0u; routeIndex < sourcePlan.routeCount;
         ++routeIndex) {
      const BimSurfaceDrawRoute &route = sourcePlan.routes[routeIndex];
      const VkPipeline pipeline = pipelineForBimSurfaceRoute(
          route, inputs.singleSidedPipeline, inputs.windingFlippedPipeline,
          inputs.doubleSidedPipeline);
      if (routeMayRecord(route, pipeline, inputs)) {
        return true;
      }
    }
  }
  return false;
}

void bindBimSurfaceGeometry(VkCommandBuffer cmd,
                            const BimSurfacePassGeometryBinding &geometry,
                            VkPipelineLayout layout) {
  vkCmdBindDescriptorSets(
      cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0,
      static_cast<uint32_t>(geometry.descriptorSets.size()),
      geometry.descriptorSets.data(), 0, nullptr);
  const VkDeviceSize offsets[] = {geometry.vertexSlice.offset};
  vkCmdBindVertexBuffers(cmd, 0, 1, &geometry.vertexSlice.buffer, offsets);
  vkCmdBindIndexBuffer(cmd, geometry.indexSlice.buffer,
                       geometry.indexSlice.offset, geometry.indexType);
}

[[nodiscard]] bool drawGpuCompacted(
    VkCommandBuffer cmd, const BimSurfaceDrawRoute &route, VkPipeline pipeline,
    const BimSurfacePassRecordInputs &inputs, BimSurfacePassSourceKind source) {
  if (!inputs.bimManager || pipeline == VK_NULL_HANDLE) {
    return false;
  }
  auto slot = route.gpuSlot;
  const auto viewportSlot = source == BimSurfacePassSourceKind::Mesh
      ? bimViewportCompactionSlot(slot) : std::nullopt;
  if (viewportSlot && hasDrawCommands(route.cpuCommands) &&
      inputs.bimManager->drawCompactionReady(*viewportSlot)) {
    slot = *viewportSlot;
  } else if (!route.gpuCompactionAllowed || !inputs.bimManager->drawCompactionReady(slot)) {
    return false;
  }

  container::gpu::BindlessPushConstants pc = inputs.pushConstants;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  pushSceneObjectIndex(cmd, inputs.pipelineLayout, pc, kIndirectObjectIndex);
  inputs.bimManager->drawCompacted(slot, cmd);
  return true;
}

} // namespace

bool recordBimSurfacePassCommands(VkCommandBuffer cmd,
                                  const BimSurfacePassRecordInputs &inputs) {
  if (inputs.plan == nullptr || !inputs.plan->active ||
      inputs.pipelineLayout == VK_NULL_HANDLE ||
      inputs.debugOverlay == nullptr || !hasReadyGeometry(inputs.geometry) ||
      !hasRecordableRoute(inputs)) {
    return false;
  }

  const BimSurfacePassPlan &plan = *inputs.plan;
  container::gpu::BindlessPushConstants cpuPushConstants =
      inputs.pushConstants;
  bindBimSurfaceGeometry(cmd, inputs.geometry, inputs.pipelineLayout);
  bool recorded = false;
  for (uint32_t sourceIndex = 0u; sourceIndex < plan.sourceCount;
       ++sourceIndex) {
    const BimSurfacePassSourcePlan &sourcePlan = plan.sources[sourceIndex];
    for (uint32_t routeIndex = 0u; routeIndex < sourcePlan.routeCount;
         ++routeIndex) {
      const BimSurfaceDrawRoute &route = sourcePlan.routes[routeIndex];
      const VkPipeline pipeline = pipelineForBimSurfaceRoute(
          route, inputs.singleSidedPipeline, inputs.windingFlippedPipeline,
          inputs.doubleSidedPipeline);
      if (drawGpuCompacted(cmd, route, pipeline, inputs, sourcePlan.source)) {
        recorded = true;
        continue;
      }
      if (route.cpuFallbackAllowed && hasDrawCommands(route.cpuCommands) &&
          pipeline != VK_NULL_HANDLE) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        inputs.debugOverlay->drawScene(cmd, inputs.pipelineLayout,
                                       *route.cpuCommands, cpuPushConstants);
        recorded = true;
      }
    }
  }
  return recorded;
}

} // namespace container::renderer
