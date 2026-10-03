#include "Container/renderer/picking/TransparentPickRasterPassRecorder.h"

#include "Container/renderer/picking/TransparentPickDepthCopyRecorder.h"
#include "Container/renderer/scene/SceneOpaqueDrawRecorder.h"
#include "Container/renderer/scene/SceneViewport.h"

#include <array>
#include <span>

namespace container::renderer {

namespace {

[[nodiscard]] bool hasTransparentPickRasterPassShell(
    VkCommandBuffer cmd, const TransparentPickRasterPassRecordInputs &inputs) {
  return inputs.active && cmd != VK_NULL_HANDLE &&
         inputs.renderPass != VK_NULL_HANDLE &&
         inputs.framebuffer != VK_NULL_HANDLE && inputs.extent.width > 0u &&
         inputs.extent.height > 0u;
}

[[nodiscard]] bool
hasTransparentPickGeometry(const TransparentPickPassGeometryBinding &geometry) {
  return geometry.descriptorSet != VK_NULL_HANDLE &&
         geometry.vertexSlice.buffer != VK_NULL_HANDLE &&
         geometry.indexSlice.buffer != VK_NULL_HANDLE;
}

[[nodiscard]] BimSurfacePassGeometryBinding
bimSurfacePickGeometry(std::span<const VkDescriptorSet> descriptorSets,
                       const TransparentPickPassGeometryBinding &geometry) {
  return {.descriptorSets = descriptorSets,
          .vertexSlice = geometry.vertexSlice,
          .indexSlice = geometry.indexSlice,
          .indexType = geometry.indexType};
}

} // namespace

bool recordTransparentPickRasterPassCommands(
    VkCommandBuffer cmd, const TransparentPickRasterPassRecordInputs &inputs) {
  if (!hasTransparentPickRasterPassShell(cmd, inputs)) {
    return false;
  }

  RenderingBeginInfo info{};
  info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  info.renderPass = inputs.renderPass;
  info.framebuffer = inputs.framebuffer;
  info.renderArea.offset = {0, 0};
  info.renderArea.extent = inputs.extent;
  VkClearValue pickClear{};
  pickClear.color = {{0u, 0u, 0u, 0u}};
  info.clearValueCount = 1u;
  info.pClearValues = &pickClear;

  beginDynamicRendering(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
  recordSceneViewportAndScissor(cmd, inputs.extent);
  static_cast<void>(recordTransparentPickPassCommands(cmd, inputs.pass));
  if (inputs.extraPassWorkActive && inputs.recordAfterGeometry) {
    inputs.recordAfterGeometry(cmd);
  }
  endDynamicRendering(cmd);
  return true;
}

bool recordTransparentPickFramePassCommands(
    VkCommandBuffer cmd, const TransparentPickFramePassRecordInputs &inputs) {
  if (cmd == VK_NULL_HANDLE || inputs.extent.width == 0u ||
      inputs.extent.height == 0u || inputs.renderPass == VK_NULL_HANDLE ||
      inputs.framebuffer == VK_NULL_HANDLE ||
      inputs.sourceDepthStencilImage == VK_NULL_HANDLE ||
      inputs.pickDepthImage == VK_NULL_HANDLE ||
      inputs.pickIdImage == VK_NULL_HANDLE) {
    return false;
  }

  const bool transparentPickStateReady =
      inputs.pipelineLayout != VK_NULL_HANDLE &&
      inputs.pipelines.primary != VK_NULL_HANDLE &&
      inputs.pushConstants != nullptr && inputs.debugOverlay != nullptr;
  const bool sceneRecordReady = transparentPickStateReady &&
                                inputs.scenePassReady &&
                                hasTransparentPickGeometry(inputs.scene);
  const SceneOpaqueDrawPlan opaquePickPlan =
      sceneRecordReady
          ? buildSceneOpaqueDrawPlan({.gpuIndirectAvailable = false,
                                      .draws = inputs.sceneOpaqueDraws})
          : SceneOpaqueDrawPlan{};
  const SceneTransparentDrawPlan transparentPlan =
      sceneRecordReady ? buildSceneTransparentDrawPlan(inputs.sceneDraws)
                       : SceneTransparentDrawPlan{};

  const std::array<VkDescriptorSet, 1> bimDescriptorSets = {
      inputs.bim.descriptorSet};
  const container::gpu::BindlessPushConstants pushConstants =
      inputs.pushConstants != nullptr ? *inputs.pushConstants
                                      : container::gpu::BindlessPushConstants{};
  const BimSurfacePassPlan bimTransparentPickPlan =
      buildBimSurfaceFramePassPlan(
          {.kind = BimSurfacePassKind::TransparentPick,
           .passReady = transparentPickStateReady && inputs.bimPassReady &&
                        hasTransparentPickGeometry(inputs.bim),
           .draws = inputs.bimDraws,
           .geometry = bimSurfacePickGeometry(bimDescriptorSets, inputs.bim),
           .pipelines = {.singleSided = inputs.pipelines.primary},
           .pushConstants = &pushConstants,
           .semanticColorMode = inputs.bimSemanticColorMode});
  const BimSurfacePassPlan bimOpaquePickPlan =
      buildBimSurfaceFramePassPlan(
          {.kind = BimSurfacePassKind::GBuffer,
           .passReady = transparentPickStateReady && inputs.bimPassReady &&
                        hasTransparentPickGeometry(inputs.bim),
           .draws = inputs.bimOpaqueDraws,
           .geometry = bimSurfacePickGeometry(bimDescriptorSets, inputs.bim),
           .pipelines = {.singleSided = inputs.pipelines.primary},
           .pushConstants = &pushConstants,
           .semanticColorMode = inputs.bimSemanticColorMode});

  if (opaquePickPlan.cpuRouteCount == 0u &&
      transparentPlan.routeCount == 0u && !bimOpaquePickPlan.active &&
      !bimTransparentPickPlan.active && !inputs.extraPassWorkActive) {
    return false;
  }

  const TransparentPickDepthCopyPlan depthCopyPlan =
      buildTransparentPickDepthCopyPlan(
          {.sourceDepthStencilImage = inputs.sourceDepthStencilImage,
           .pickDepthImage = inputs.pickDepthImage,
           .extent = inputs.extent});
  if (!recordTransparentPickDepthCopyCommands(cmd, depthCopyPlan)) {
    return false;
  }

  return recordTransparentPickRasterPassCommands(
      cmd, {.active = true,
            .renderPass = inputs.renderPass,
            .framebuffer = inputs.framebuffer,
            .extent = inputs.extent,
            .pass = {.sceneOpaquePlan = &opaquePickPlan,
                     .scenePlan = &transparentPlan,
                     .bimOpaquePlan = &bimOpaquePickPlan,
                     .bimPlan = &bimTransparentPickPlan,
                     .scene = inputs.scene,
                     .bim = inputs.bim,
                     .pipelines = inputs.pipelines,
                     .pipelineLayout = inputs.pipelineLayout,
                     .pushConstants = pushConstants,
                     .debugOverlay = inputs.debugOverlay,
                     .bimManager = inputs.bimManager},
            .extraPassWorkActive = inputs.extraPassWorkActive,
            .recordAfterGeometry = inputs.recordAfterGeometry});
}

} // namespace container::renderer
