#include "Container/renderer/shadow/ShadowPassRecorder.h"

#include "Container/renderer/bim/BimManager.h"
#include "Container/renderer/shadow/ShadowCascadeSecondaryCommandBufferRecorder.h"
#include "Container/renderer/shadow/ShadowPassRasterRecorder.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace container::renderer {

namespace {

constexpr uint32_t kIndirectObjectIndex = std::numeric_limits<uint32_t>::max();
constexpr VkShaderStageFlags kShadowPushStages =
    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

[[nodiscard]] bool hasDrawCommands(const std::vector<DrawCommand> *commands) {
  return commands != nullptr && !commands->empty();
}

[[nodiscard]] uint32_t drawInstanceCount(const DrawCommand &command) {
  return std::max(command.instanceCount, 1u);
}

[[nodiscard]] bool hasReadyGeometry(const ShadowPassGeometryBinding &geometry) {
  return geometry.sceneDescriptorSet != VK_NULL_HANDLE &&
         geometry.vertexSlice.buffer != VK_NULL_HANDLE &&
         geometry.indexSlice.buffer != VK_NULL_HANDLE;
}

[[nodiscard]] VkPipeline pipelineForShadowPassRoute(
    ShadowPassPipeline pipeline, const ShadowPassPipelineHandles &pipelines) {
  switch (pipeline) {
  case ShadowPassPipeline::Primary:
    return pipelines.primary;
  case ShadowPassPipeline::FrontCull:
    return pipelines.frontCull;
  case ShadowPassPipeline::NoCull:
    return pipelines.noCull;
  }
  return pipelines.primary;
}

[[nodiscard]] BimDrawCompactionSlot
bimDrawCompactionSlot(ShadowPassBimGpuSlot slot) {
  switch (slot) {
  case ShadowPassBimGpuSlot::OpaqueSingleSided:
    return BimDrawCompactionSlot::OpaqueSingleSided;
  case ShadowPassBimGpuSlot::OpaqueWindingFlipped:
    return BimDrawCompactionSlot::OpaqueWindingFlipped;
  case ShadowPassBimGpuSlot::OpaqueDoubleSided:
    return BimDrawCompactionSlot::OpaqueDoubleSided;
  }
  return BimDrawCompactionSlot::OpaqueSingleSided;
}

void bindShadowGeometry(VkCommandBuffer cmd,
                        const ShadowPassGeometryBinding &geometry,
                        VkDescriptorSet shadowDescriptorSet,
                        VkPipelineLayout layout) {
  const VkDescriptorSet shadowSets[] = {geometry.sceneDescriptorSet,
                                        shadowDescriptorSet};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0,
                          2u, shadowSets, 0, nullptr);
  const VkDeviceSize offsets[] = {geometry.vertexSlice.offset};
  vkCmdBindVertexBuffers(cmd, 0, 1, &geometry.vertexSlice.buffer, offsets);
  vkCmdBindIndexBuffer(cmd, geometry.indexSlice.buffer,
                       geometry.indexSlice.offset, geometry.indexType);
}

void pushShadowConstants(VkCommandBuffer cmd, VkPipelineLayout layout,
                         container::gpu::ShadowPushConstants &pushConstants) {
  vkCmdPushConstants(cmd, layout, kShadowPushStages, 0,
                     sizeof(container::gpu::ShadowPushConstants),
                     &pushConstants);
}

bool drawShadowList(VkCommandBuffer cmd, VkPipelineLayout layout,
                    const std::vector<DrawCommand> *commands,
                    container::gpu::ShadowPushConstants &pushConstants) {
  if (!hasDrawCommands(commands)) {
    return false;
  }

  pushConstants.objectIndex = kIndirectObjectIndex;
  pushShadowConstants(cmd, layout, pushConstants);
  for (const DrawCommand &command : *commands) {
    vkCmdDrawIndexed(cmd, command.indexCount, drawInstanceCount(command),
                     command.firstIndex, 0, command.objectIndex);
  }
  return true;
}

[[nodiscard]] bool hasSceneGpuIndirectBuffers(
    const ShadowPassGpuIndirectBuffers &buffers) {
  return buffers.drawBuffer != VK_NULL_HANDLE &&
         buffers.countBuffer != VK_NULL_HANDLE && buffers.maxDrawCount > 0u;
}

[[nodiscard]] bool hasRecordableSceneRoute(
    const ShadowPassRecordInputs &inputs) {
  if (!hasReadyGeometry(inputs.scene)) {
    return false;
  }
  if (inputs.plan->sceneGpuRoute.active &&
      hasSceneGpuIndirectBuffers(inputs.sceneGpuIndirect) &&
      pipelineForShadowPassRoute(inputs.plan->sceneGpuRoute.pipeline,
                                 inputs.pipelines) != VK_NULL_HANDLE) {
    return true;
  }
  for (uint32_t routeIndex = 0u; routeIndex < inputs.plan->sceneCpuRouteCount;
       ++routeIndex) {
    const ShadowPassCpuRoute &route = inputs.plan->sceneCpuRoutes[routeIndex];
    if (hasDrawCommands(route.commands) &&
        pipelineForShadowPassRoute(route.pipeline, inputs.pipelines) !=
            VK_NULL_HANDLE) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool hasRecordableBimRoute(const ShadowPassRecordInputs &inputs) {
  if (!hasReadyGeometry(inputs.bim)) {
    return false;
  }
  for (uint32_t routeIndex = 0u; routeIndex < inputs.plan->bimGpuRouteCount;
       ++routeIndex) {
    const ShadowPassBimGpuRoute &route = inputs.plan->bimGpuRoutes[routeIndex];
    const BimDrawCompactionSlot slot = bimDrawCompactionSlot(route.slot);
    if (inputs.bimManager != nullptr &&
        pipelineForShadowPassRoute(route.pipeline, inputs.pipelines) !=
            VK_NULL_HANDLE &&
        inputs.bimManager->drawCompactionReady(slot, inputs.imageIndex)) {
      return true;
    }
  }
  for (uint32_t routeIndex = 0u; routeIndex < inputs.plan->bimCpuRouteCount;
       ++routeIndex) {
    const ShadowPassCpuRoute &route = inputs.plan->bimCpuRoutes[routeIndex];
    if (hasDrawCommands(route.commands) &&
        pipelineForShadowPassRoute(route.pipeline, inputs.pipelines) !=
            VK_NULL_HANDLE) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool hasRecordableRoute(const ShadowPassRecordInputs &inputs) {
  return hasRecordableSceneRoute(inputs) || hasRecordableBimRoute(inputs);
}

bool recordSceneGpuRoute(VkCommandBuffer cmd,
                         const ShadowPassRecordInputs &inputs,
                         container::gpu::ShadowPushConstants &pushConstants) {
  if (!inputs.plan->sceneGpuRoute.active ||
      !hasSceneGpuIndirectBuffers(inputs.sceneGpuIndirect)) {
    return false;
  }

  const VkPipeline pipeline = pipelineForShadowPassRoute(
      inputs.plan->sceneGpuRoute.pipeline, inputs.pipelines);
  if (pipeline == VK_NULL_HANDLE) {
    return false;
  }

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  pushConstants.objectIndex = kIndirectObjectIndex;
  pushShadowConstants(cmd, inputs.pipelineLayout, pushConstants);
  vkCmdDrawIndexedIndirectCount(
      cmd, inputs.sceneGpuIndirect.drawBuffer, 0,
      inputs.sceneGpuIndirect.countBuffer, 0,
      inputs.sceneGpuIndirect.maxDrawCount,
      sizeof(container::gpu::GpuDrawIndexedIndirectCommand));
  return true;
}

bool recordSceneCpuRoutes(VkCommandBuffer cmd,
                          const ShadowPassRecordInputs &inputs,
                          container::gpu::ShadowPushConstants &pushConstants) {
  bool recorded = false;
  for (uint32_t routeIndex = 0u; routeIndex < inputs.plan->sceneCpuRouteCount;
       ++routeIndex) {
    const ShadowPassCpuRoute &route = inputs.plan->sceneCpuRoutes[routeIndex];
    const VkPipeline pipeline =
        pipelineForShadowPassRoute(route.pipeline, inputs.pipelines);
    if (pipeline == VK_NULL_HANDLE || !hasDrawCommands(route.commands)) {
      continue;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    recorded =
        drawShadowList(cmd, inputs.pipelineLayout, route.commands,
                       pushConstants) ||
        recorded;
  }
  return recorded;
}

bool recordBimGpuRoutes(VkCommandBuffer cmd,
                        const ShadowPassRecordInputs &inputs,
                        container::gpu::ShadowPushConstants &pushConstants) {
  bool recorded = false;
  for (uint32_t routeIndex = 0u; routeIndex < inputs.plan->bimGpuRouteCount;
       ++routeIndex) {
    const ShadowPassBimGpuRoute &route = inputs.plan->bimGpuRoutes[routeIndex];
    const BimDrawCompactionSlot slot = bimDrawCompactionSlot(route.slot);
    const VkPipeline pipeline =
        pipelineForShadowPassRoute(route.pipeline, inputs.pipelines);
    if (inputs.bimManager == nullptr || pipeline == VK_NULL_HANDLE ||
        !inputs.bimManager->drawCompactionReady(slot, inputs.imageIndex)) {
      continue;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    pushConstants.objectIndex = kIndirectObjectIndex;
    pushShadowConstants(cmd, inputs.pipelineLayout, pushConstants);
    inputs.bimManager->drawCompacted(slot, cmd, inputs.imageIndex);
    recorded = true;
  }
  return recorded;
}

bool recordBimCpuRoutes(VkCommandBuffer cmd,
                        const ShadowPassRecordInputs &inputs,
                        container::gpu::ShadowPushConstants &pushConstants) {
  bool recorded = false;
  for (uint32_t routeIndex = 0u; routeIndex < inputs.plan->bimCpuRouteCount;
       ++routeIndex) {
    const ShadowPassCpuRoute &route = inputs.plan->bimCpuRoutes[routeIndex];
    const VkPipeline pipeline =
        pipelineForShadowPassRoute(route.pipeline, inputs.pipelines);
    if (pipeline == VK_NULL_HANDLE || !hasDrawCommands(route.commands)) {
      continue;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    recorded =
        drawShadowList(cmd, inputs.pipelineLayout, route.commands,
                       pushConstants) ||
        recorded;
  }
  return recorded;
}

bool recordShadowCascadePassBodyCommands(
    VkCommandBuffer cmd, const ShadowCascadePassRecordInputs &inputs) {
  const ShadowPassDrawPlan shadowPassPlan =
      buildShadowPassDrawPlan(inputs.drawInputs);

  return recordShadowPassCommands(
      cmd, {.plan = &shadowPassPlan,
            .extent = inputs.extent,
            .scene = inputs.scene,
            .bim = inputs.bim,
            .shadowDescriptorSet = inputs.shadowDescriptorSet,
            .pipelines = inputs.pipelines,
            .pipelineLayout = inputs.pipelineLayout,
            .pushConstants = inputs.pushConstants,
            .rasterConstantBias = inputs.rasterConstantBias,
            .rasterSlopeBias = inputs.rasterSlopeBias,
            .sceneGpuIndirect = inputs.sceneGpuIndirect,
            .bimManager = inputs.bimManager,
            .imageIndex = inputs.imageIndex});
}

} // namespace

bool recordShadowPassCommands(VkCommandBuffer cmd,
                              const ShadowPassRecordInputs &inputs) {
  if (inputs.plan == nullptr ||
      inputs.shadowDescriptorSet == VK_NULL_HANDLE ||
      inputs.pipelineLayout == VK_NULL_HANDLE ||
      !hasRecordableRoute(inputs)) {
    return false;
  }

  VkViewport viewport{};
  // Shadow maps intentionally use a positive-height viewport. Their atlas UV
  // mapping therefore differs from scene-buffer UV mapping and does not flip Y.
  VkExtent2D extent = inputs.extent;
  if (extent.width == 0u || extent.height == 0u) {
    extent = {container::gpu::kShadowMapResolution,
              container::gpu::kShadowMapResolution};
  }
  viewport.width = static_cast<float>(extent.width);
  viewport.height = static_cast<float>(extent.height);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(cmd, 0, 1, &viewport);

  VkRect2D scissor{};
  scissor.extent = extent;
  vkCmdSetScissor(cmd, 0, 1, &scissor);

  // Reverse-Z shadow maps need negative caster bias to push written depth
  // toward far (0.0). Keep this dynamic so tuning does not rebuild pipelines.
  vkCmdSetDepthBias(cmd, inputs.rasterConstantBias, 0.0f,
                    inputs.rasterSlopeBias);

  bool recorded = false;
  container::gpu::ShadowPushConstants pushConstants = inputs.pushConstants;
  if (hasReadyGeometry(inputs.scene)) {
    bindShadowGeometry(cmd, inputs.scene, inputs.shadowDescriptorSet,
                       inputs.pipelineLayout);
    recorded = recordSceneGpuRoute(cmd, inputs, pushConstants) || recorded;
    recorded = recordSceneCpuRoutes(cmd, inputs, pushConstants) || recorded;
  }

  if (hasReadyGeometry(inputs.bim)) {
    bindShadowGeometry(cmd, inputs.bim, inputs.shadowDescriptorSet,
                       inputs.pipelineLayout);
    recorded = recordBimGpuRoutes(cmd, inputs, pushConstants) || recorded;
    recorded = recordBimCpuRoutes(cmd, inputs, pushConstants) || recorded;
  }
  return recorded;
}

bool recordShadowCascadePassCommands(
    VkCommandBuffer cmd, const ShadowCascadePassRecordInputs &inputs) {
  const ShadowPassRasterPlan shadowPassPlan =
      buildShadowPassRasterPlan(inputs.raster);

  return recordShadowPassRasterCommands(
      cmd, {.plan = &shadowPassPlan,
            .renderPass = inputs.renderPass,
            .framebuffer = inputs.framebuffer,
            .recordBody =
                [inputs](VkCommandBuffer bodyCmd) {
                  static_cast<void>(
                      recordShadowCascadePassBodyCommands(bodyCmd, inputs));
                }});
}

void recordShadowCascadeSecondaryPassCommands(
    const ShadowCascadeSecondaryPassRecordInputs &inputs) {
  ShadowCascadeSecondaryCommandBufferPlanInputs planInputs{};
  planInputs.secondaryCommandBuffersEnabled =
      inputs.secondaryCommandBuffersEnabled;

  for (uint32_t cascadeIndex = 0u;
       cascadeIndex < container::gpu::kShadowCascadeCount; ++cascadeIndex) {
    const ShadowCascadePassRecordInputs &cascade = inputs.cascades[cascadeIndex];
    planInputs.cascadePassActive[cascadeIndex] = cascade.cascadePassActive;
    planInputs.useSecondaryCommandBuffer[cascadeIndex] =
        cascade.raster.useSecondaryCommandBuffer;
    planInputs.commandBuffers[cascadeIndex] =
        cascade.raster.secondaryCommandBuffer;
  }

  const ShadowCascadeSecondaryCommandBufferRecordPlan plan =
      buildShadowCascadeSecondaryCommandBufferRecordPlan(planInputs);
  recordShadowCascadeSecondaryCommandBufferPlan(
      plan, [&inputs](VkCommandBuffer secondary, uint32_t cascadeIndex) {
        const ShadowCascadePassRecordInputs cascade =
            inputs.cascades[cascadeIndex];
        recordShadowCascadeSecondaryCommandBufferCommands(
            {.commandBuffer = secondary,
             .renderPass = cascade.renderPass,
             .framebuffer = cascade.framebuffer,
             .recordBody =
                 [cascade](VkCommandBuffer bodyCmd) {
                   static_cast<void>(
                       recordShadowCascadePassBodyCommands(bodyCmd, cascade));
                 }});
      });
}

} // namespace container::renderer
