#include "Container/renderer/scene/SceneRasterPassRecorder.h"

#include "Container/renderer/scene/SceneViewport.h"

namespace container::renderer {

namespace {

[[nodiscard]] bool hasRasterPassShell(
    VkCommandBuffer cmd, const SceneRasterPassRecordInputs &inputs) {
  return cmd != VK_NULL_HANDLE && inputs.renderPass != VK_NULL_HANDLE &&
         inputs.framebuffer != VK_NULL_HANDLE && inputs.extent.width > 0u &&
         inputs.extent.height > 0u && inputs.clearValues.count > 0u &&
         inputs.clearValues.count <= inputs.clearValues.values.size() &&
         inputs.plan != nullptr;
}

} // namespace

SceneRasterPassClearValues sceneRasterPassClearValues(
    SceneRasterPassKind kind) {
  SceneRasterPassClearValues clearValues{};
  switch (kind) {
  case SceneRasterPassKind::DepthPrepass:
    clearValues.values[0].depthStencil = {0.0f, 0};
    clearValues.count = 1u;
    break;
  case SceneRasterPassKind::GBuffer:
    clearValues.values[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    clearValues.values[1].color = {{0.5f, 0.5f, 1.0f, 1.0f}};
    clearValues.values[2].color = {{0.0f, 1.0f, 1.0f, 1.0f}};
    clearValues.values[3].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    clearValues.values[4].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    clearValues.values[5].color = {{0u, 0u, 0u, 0u}};
    clearValues.count = 6u;
    break;
  }
  return clearValues;
}

bool recordSceneRasterPassCommands(VkCommandBuffer cmd,
                                   const SceneRasterPassRecordInputs &inputs) {
  if (!hasRasterPassShell(cmd, inputs)) {
    return false;
  }

  RenderingBeginInfo info{};
  info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  info.renderPass = inputs.renderPass;
  info.framebuffer = inputs.framebuffer;
  info.renderArea.offset = {0, 0};
  info.renderArea.extent = inputs.extent;
  info.clearValueCount = inputs.clearValues.count;
  info.pClearValues = inputs.clearValues.values.data();

  beginDynamicRendering(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
  recordSceneViewportAndScissor(cmd, inputs.extent);
  static_cast<void>(recordSceneOpaqueDrawCommands(
      cmd, {.plan = inputs.plan,
            .geometry = inputs.geometry,
            .pipelines = inputs.pipelines,
            .pipelineLayout = inputs.pipelineLayout,
            .pushConstants = inputs.pushConstants,
            .imageIndex = inputs.imageIndex,
            .debugOverlay = inputs.debugOverlay,
            .gpuCullManager = inputs.gpuCullManager}));
  static_cast<void>(
      recordSceneDiagnosticCubeCommands(cmd, inputs.diagnosticCube));
  endDynamicRendering(cmd);
  return true;
}

} // namespace container::renderer
