#include "Container/renderer/bim/BimPrimitivePassRecorder.h"

#include "Container/renderer/bim/BimManager.h"

#include <limits>

namespace container::renderer {

namespace {

constexpr uint32_t kIndirectObjectIndex = std::numeric_limits<uint32_t>::max();
constexpr VkShaderStageFlags kWireframePushStages =
    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

[[nodiscard]] bool hasDrawCommands(const std::vector<DrawCommand> *commands) {
  return commands != nullptr && !commands->empty();
}

[[nodiscard]] bool
hasRequiredInputs(const BimPrimitivePassRecordInputs &inputs) {
  return inputs.plan != nullptr && inputs.plan->active &&
         inputs.geometryReady && inputs.wireframeLayout != VK_NULL_HANDLE &&
         inputs.sceneDescriptorSet != VK_NULL_HANDLE &&
         inputs.vertexSlice.buffer != VK_NULL_HANDLE &&
         inputs.indexSlice.buffer != VK_NULL_HANDLE &&
         inputs.pushConstants != nullptr && inputs.debugOverlay != nullptr;
}

void bindGeometry(VkCommandBuffer cmd, container::gpu::BufferSlice vertex,
                  container::gpu::BufferSlice index, VkIndexType indexType) {
  const VkDeviceSize offsets[] = {vertex.offset};
  vkCmdBindVertexBuffers(cmd, 0, 1, &vertex.buffer, offsets);
  vkCmdBindIndexBuffer(cmd, index.buffer, index.offset, indexType);
}

void pushGpuPrimitiveConstants(VkCommandBuffer cmd, VkPipelineLayout layout,
                               const BimPrimitivePassRecordInputs &inputs,
                               const BimPrimitivePassPlan &plan,
                               WireframePushConstants baseConstants) {
  baseConstants.objectIndex = kIndirectObjectIndex;
  baseConstants.colorIntensity = glm::vec4(inputs.color, plan.opacity);
  baseConstants.lineWidth = plan.primitiveSize;
  vkCmdPushConstants(cmd, layout, kWireframePushStages, 0,
                     sizeof(WireframePushConstants), &baseConstants);
}

} // namespace

bool hasBimPrimitiveFramePassGeometry(
    const BimPrimitivePassGeometryBinding &geometry) {
  return geometry.sceneDescriptorSet != VK_NULL_HANDLE &&
         geometry.vertexSlice.buffer != VK_NULL_HANDLE &&
         geometry.indexSlice.buffer != VK_NULL_HANDLE;
}

BimPrimitivePassPlanInputs buildBimPrimitiveFramePassPlanInputs(
    const BimPrimitiveFramePassRecordInputs &inputs) {
  return {.kind = inputs.style.kind,
          .enabled = inputs.style.enabled,
          .depthTest = inputs.style.depthTest,
          .placeholderRangePreviewEnabled =
              inputs.style.placeholderRangePreviewEnabled,
          .nativeDrawsUseGpuVisibility =
              inputs.style.nativeDrawsUseGpuVisibility,
          .opacity = inputs.style.opacity,
          .primitiveSize = inputs.style.primitiveSize,
          .placeholderDraws = inputs.placeholderDraws,
          .nativeDraws = inputs.nativeDraws};
}

bool recordBimPrimitivePassCommands(
    VkCommandBuffer cmd, const BimPrimitivePassRecordInputs &inputs) {
  if (!hasRequiredInputs(inputs)) {
    return false;
  }

  const BimPrimitivePassPlan &plan = *inputs.plan;
  const VkPipeline pipeline =
      plan.depthTest ? inputs.depthPipeline : inputs.noDepthPipeline;
  if (pipeline == VK_NULL_HANDLE) {
    return false;
  }

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  if (inputs.recordLineWidth) {
    vkCmdSetLineWidth(cmd,
                      inputs.wideLinesSupported ? plan.primitiveSize : 1.0f);
  }
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          inputs.wireframeLayout, 0, 1,
                          &inputs.sceneDescriptorSet, 0, nullptr);
  bindGeometry(cmd, inputs.vertexSlice, inputs.indexSlice, inputs.indexType);

  WireframePushConstants pc = *inputs.pushConstants;
  pc.useObjectColor = plan.useObjectColor ? 1u : 0u;
  if (plan.gpuCompaction) {
    for (uint32_t slotIndex = 0; slotIndex < plan.gpuSlotCount; ++slotIndex) {
      const BimDrawCompactionSlot slot = plan.gpuSlots[slotIndex];
      if (inputs.bimManager == nullptr ||
          !inputs.bimManager->drawCompactionReady(slot)) {
        continue;
      }
      pushGpuPrimitiveConstants(cmd, inputs.wireframeLayout, inputs, plan, pc);
      inputs.bimManager->drawCompacted(slot, cmd);
    }
    return true;
  }

  for (const std::vector<DrawCommand> *commands : plan.cpuDrawSources) {
    if (hasDrawCommands(commands)) {
      inputs.debugOverlay->drawWireframe(cmd, inputs.wireframeLayout, *commands,
                                         inputs.color, plan.opacity,
                                         plan.primitiveSize, pc);
    }
  }
  return true;
}

bool recordBimPrimitiveFramePassCommands(
    VkCommandBuffer cmd, const BimPrimitiveFramePassRecordInputs &inputs) {
  if (cmd == VK_NULL_HANDLE) {
    return false;
  }

  const BimPrimitivePassPlan plan =
      buildBimPrimitivePassPlan(buildBimPrimitiveFramePassPlanInputs(inputs));
  if (!plan.active) {
    return false;
  }

  return recordBimPrimitivePassCommands(
      cmd, {.plan = &plan,
            .geometryReady = hasBimPrimitiveFramePassGeometry(inputs.geometry),
            .depthPipeline = inputs.pipelines.depth,
            .noDepthPipeline = inputs.pipelines.noDepth,
            .wireframeLayout = inputs.wireframeLayout,
            .sceneDescriptorSet = inputs.geometry.sceneDescriptorSet,
            .vertexSlice = inputs.geometry.vertexSlice,
            .indexSlice = inputs.geometry.indexSlice,
            .indexType = inputs.geometry.indexType,
            .pushConstants = inputs.pushConstants,
            .debugOverlay = inputs.debugOverlay,
            .bimManager = inputs.bimManager,
            .color = inputs.style.color,
            .recordLineWidth = inputs.style.recordLineWidth,
            .wideLinesSupported = inputs.style.wideLinesSupported});
}

} // namespace container::renderer
