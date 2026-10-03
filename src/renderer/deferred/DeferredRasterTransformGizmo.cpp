#include "Container/renderer/deferred/DeferredRasterTransformGizmo.h"

#include "Container/renderer/scene/SceneViewport.h"
#include "Container/utility/GuiManager.h"

namespace container::renderer {

namespace {

constexpr uint32_t kTransformGizmoLineMode = 0u;
constexpr uint32_t kTransformGizmoSolidArrowMode = 1u;
constexpr uint32_t kTransformGizmoViewBoxVertexCount = 24u;
constexpr uint32_t kTransformGizmoTranslateAxisVertexCount = 2u;
constexpr uint32_t kTransformGizmoTranslateHeadVertexCount = 9u;
constexpr uint32_t kTransformGizmoScaleAxisVertexCount = 26u;
constexpr uint32_t kTransformGizmoAxisVertexCount =
    3u * kTransformGizmoTranslateAxisVertexCount +
    kTransformGizmoViewBoxVertexCount;
constexpr uint32_t kTransformGizmoScaleVertexCount =
    3u * kTransformGizmoScaleAxisVertexCount +
    kTransformGizmoViewBoxVertexCount;
constexpr uint32_t kTransformGizmoRingSegments = 64u;
constexpr uint32_t kTransformGizmoRotateVertexCount =
    3u * kTransformGizmoRingSegments * 2u +
    kTransformGizmoViewBoxVertexCount;

[[nodiscard]] bool hasDeferredTransformGizmoDrawWork(
    const DeferredTransformGizmoDrawInputs& inputs) {
  return inputs.gizmo.visible &&
         inputs.commandBuffer != VK_NULL_HANDLE &&
         inputs.lightingDescriptorSet != VK_NULL_HANDLE &&
         inputs.pipelineLayout != VK_NULL_HANDLE &&
         inputs.pipeline != VK_NULL_HANDLE &&
         inputs.pushConstants != nullptr;
}

void recordDeferredTransformGizmoSolidArrowheads(
    const DeferredTransformGizmoDrawInputs& inputs) {
  if (!deferredTransformGizmoUsesSolidArrowheads(inputs.gizmo.tool) ||
      inputs.solidPipeline == VK_NULL_HANDLE) {
    return;
  }

  TransformGizmoPushConstants& pc = *inputs.pushConstants;
  updateDeferredTransformGizmoPushConstants(pc, inputs.gizmo);
  pc.padding0 = kTransformGizmoSolidArrowMode;

  vkCmdBindPipeline(inputs.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    inputs.solidPipeline);
  vkCmdBindDescriptorSets(inputs.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          inputs.pipelineLayout, 0, 1,
                          &inputs.lightingDescriptorSet, 0, nullptr);
  vkCmdPushConstants(inputs.commandBuffer, inputs.pipelineLayout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, sizeof(TransformGizmoPushConstants), &pc);
  vkCmdDraw(inputs.commandBuffer, kTransformGizmoTranslateHeadVertexCount, 1, 0,
            0);
  pc.padding0 = kTransformGizmoLineMode;
}

void recordDeferredTransformGizmoDraw(
    const DeferredTransformGizmoDrawInputs& inputs) {
  if (!hasDeferredTransformGizmoDrawWork(inputs)) {
    return;
  }

  vkCmdBindPipeline(inputs.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    inputs.pipeline);
  vkCmdBindDescriptorSets(inputs.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          inputs.pipelineLayout, 0, 1,
                          &inputs.lightingDescriptorSet, 0, nullptr);
  recordSceneViewportAndScissor(inputs.commandBuffer, inputs.extent);
  vkCmdSetLineWidth(inputs.commandBuffer,
                    inputs.wideLinesSupported ? 2.0f : 1.0f);

  TransformGizmoPushConstants& pc = *inputs.pushConstants;
  updateDeferredTransformGizmoPushConstants(pc, inputs.gizmo);
  vkCmdPushConstants(inputs.commandBuffer, inputs.pipelineLayout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, sizeof(TransformGizmoPushConstants), &pc);
  vkCmdDraw(inputs.commandBuffer,
            deferredTransformGizmoVertexCount(inputs.gizmo.tool), 1, 0, 0);
  recordDeferredTransformGizmoSolidArrowheads(inputs);
}

}  // namespace

uint32_t deferredTransformGizmoVertexCount(container::ui::ViewportTool tool) {
  if (tool == container::ui::ViewportTool::Rotate) {
    return kTransformGizmoRotateVertexCount;
  }
  if (tool == container::ui::ViewportTool::Scale) {
    return kTransformGizmoScaleVertexCount;
  }
  return kTransformGizmoAxisVertexCount;
}

bool deferredTransformGizmoUsesSolidArrowheads(
    container::ui::ViewportTool tool) {
  return tool == container::ui::ViewportTool::Translate;
}

void updateDeferredTransformGizmoPushConstants(
    TransformGizmoPushConstants& pushConstants,
    const FrameTransformGizmoState& gizmo) {
  pushConstants.originScale = glm::vec4(gizmo.origin, gizmo.scale);
  pushConstants.axisX = glm::vec4(gizmo.axisX, 0.0f);
  pushConstants.axisY = glm::vec4(gizmo.axisY, 0.0f);
  pushConstants.axisZ = glm::vec4(gizmo.axisZ, 0.0f);
  pushConstants.tool = static_cast<uint32_t>(gizmo.tool);
  pushConstants.activeAxis = static_cast<uint32_t>(gizmo.activeAxis);
  pushConstants.transformSpace = static_cast<uint32_t>(gizmo.transformSpace);
  pushConstants.padding0 = kTransformGizmoLineMode;
}

void recordDeferredTransformGizmoPass(
    const DeferredTransformGizmoPassInputs& inputs) {
  if (!hasDeferredTransformGizmoDrawWork(inputs.draw) ||
      inputs.renderPass == VK_NULL_HANDLE ||
      inputs.framebuffer == VK_NULL_HANDLE) {
    return;
  }

  RenderingBeginInfo info{};
  info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  info.renderPass = inputs.renderPass;
  info.framebuffer = inputs.framebuffer;
  info.renderArea.offset = {0, 0};
  info.renderArea.extent = inputs.draw.extent;
  info.clearValueCount = 0;
  info.pClearValues = nullptr;

  beginDynamicRendering(inputs.draw.commandBuffer, &info,
                       VK_SUBPASS_CONTENTS_INLINE);
  recordDeferredTransformGizmoDraw(inputs.draw);
  endDynamicRendering(inputs.draw.commandBuffer);
}

void recordDeferredTransformGizmoOverlay(
    const DeferredTransformGizmoDrawInputs& inputs) {
  recordDeferredTransformGizmoDraw(inputs);
}

}  // namespace container::renderer
