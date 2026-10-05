#include "Container/renderer/forward/ForwardRasterLightingPassRecorder.h"

#include "Container/renderer/bim/BimLightingOverlayRecorder.h"
#include "Container/renderer/bim/BimPrimitivePassRecorder.h"
#include "Container/renderer/bim/BimSurfaceRasterPassRecorder.h"
#include "Container/renderer/core/RenderPassScopeRecorder.h"
#include "Container/renderer/culling/GpuCullManager.h"
#include "Container/renderer/debug/DebugOverlayRenderer.h"
#include "Container/renderer/forward/ForwardRasterPipelineBridge.h"
#include "Container/renderer/forward/ForwardRasterResourceBridge.h"
#include "Container/renderer/scene/SceneOpaqueDrawPlanner.h"
#include "Container/renderer/scene/SceneOpaqueDrawRecorder.h"
#include "Container/renderer/scene/SceneTransparentDrawPlanner.h"
#include "Container/renderer/scene/SceneTransparentDrawRecorder.h"
#include "Container/renderer/scene/SceneViewport.h"

#include <array>
#include <span>

namespace container::renderer {

namespace {

[[nodiscard]] bool hasDrawCommands(const std::vector<DrawCommand>* commands) {
  return commands != nullptr && !commands->empty();
}

[[nodiscard]] RenderPassReadiness ready() { return {}; }

[[nodiscard]] RenderPassReadiness notNeeded() {
  RenderPassReadiness readiness{};
  readiness.ready = false;
  readiness.skipReason = RenderPassSkipReason::NotNeeded;
  return readiness;
}

[[nodiscard]] RenderPassReadiness missing(RenderResourceId resource) {
  RenderPassReadiness readiness{};
  readiness.ready = false;
  readiness.skipReason = RenderPassSkipReason::MissingResource;
  readiness.blockingResource = resource;
  return readiness;
}

[[nodiscard]] SceneOpaqueDrawLists sceneOpaqueDrawLists(
    const FrameDrawLists& draws) {
  return {.aggregate = draws.opaqueDrawCommands,
          .singleSided = draws.opaqueSingleSidedDrawCommands,
          .windingFlipped = draws.opaqueWindingFlippedDrawCommands,
          .doubleSided = draws.opaqueDoubleSidedDrawCommands};
}

[[nodiscard]] SceneTransparentDrawLists sceneTransparentDrawLists(
    const FrameDrawLists& draws) {
  return {.aggregate = draws.transparentDrawCommands,
          .singleSided = draws.transparentSingleSidedDrawCommands,
          .windingFlipped = draws.transparentWindingFlippedDrawCommands,
          .doubleSided = draws.transparentDoubleSidedDrawCommands};
}

[[nodiscard]] BimSurfaceDrawLists bimSurfaceDrawLists(
    const FrameDrawLists& draws) {
  return {
      .opaqueDrawCommands = draws.opaqueDrawCommands,
      .opaqueSingleSidedDrawCommands = draws.opaqueSingleSidedDrawCommands,
      .opaqueWindingFlippedDrawCommands =
          draws.opaqueWindingFlippedDrawCommands,
      .opaqueDoubleSidedDrawCommands = draws.opaqueDoubleSidedDrawCommands,
      .transparentDrawCommands = draws.transparentDrawCommands,
      .transparentSingleSidedDrawCommands =
          draws.transparentSingleSidedDrawCommands,
      .transparentWindingFlippedDrawCommands =
          draws.transparentWindingFlippedDrawCommands,
      .transparentDoubleSidedDrawCommands =
          draws.transparentDoubleSidedDrawCommands,
  };
}

[[nodiscard]] BimSurfaceFramePassDrawSources bimSurfaceDrawSources(
    const FrameBimResources& bim) {
  return {.mesh = bimSurfaceDrawLists(bim.draws),
          .pointPlaceholders = bimSurfaceDrawLists(bim.pointDraws),
          .curvePlaceholders = bimSurfaceDrawLists(bim.curveDraws),
          .opaqueMeshDrawsUseGpuVisibility =
              bim.opaqueMeshDrawsUseGpuVisibility,
          .transparentMeshDrawsUseGpuVisibility =
              bim.transparentMeshDrawsUseGpuVisibility};
}

[[nodiscard]] bool hasSceneOpaqueDraws(const FrameRecordParams& p) {
  return hasDrawCommands(p.draws.opaqueDrawCommands) ||
         hasDrawCommands(p.draws.opaqueSingleSidedDrawCommands) ||
         hasDrawCommands(p.draws.opaqueWindingFlippedDrawCommands) ||
         hasDrawCommands(p.draws.opaqueDoubleSidedDrawCommands);
}

[[nodiscard]] bool hasSceneTransparentDraws(const FrameRecordParams& p) {
  return hasDrawCommands(p.draws.transparentDrawCommands) ||
         hasDrawCommands(p.draws.transparentSingleSidedDrawCommands) ||
         hasDrawCommands(p.draws.transparentWindingFlippedDrawCommands) ||
         hasDrawCommands(p.draws.transparentDoubleSidedDrawCommands);
}

[[nodiscard]] bool hasBimOpaqueDraws(const FrameBimResources& bim) {
  const BimSurfaceFramePassDrawSources sources = bimSurfaceDrawSources(bim);
  return hasBimSurfaceOpaqueDrawCommands(sources.mesh) ||
         hasBimSurfaceOpaqueDrawCommands(sources.pointPlaceholders) ||
         hasBimSurfaceOpaqueDrawCommands(sources.curvePlaceholders);
}

[[nodiscard]] bool hasBimTransparentDraws(const FrameBimResources& bim) {
  const BimSurfaceFramePassDrawSources sources = bimSurfaceDrawSources(bim);
  return hasBimSurfaceTransparentDrawCommands(sources.mesh) ||
         hasBimSurfaceTransparentDrawCommands(sources.pointPlaceholders) ||
         hasBimSurfaceTransparentDrawCommands(sources.curvePlaceholders);
}

[[nodiscard]] bool hasAnyDraws(const FrameRecordParams& p) {
  return hasSceneOpaqueDraws(p) || hasSceneTransparentDraws(p) ||
         hasBimOpaqueDraws(p.bim) || hasBimTransparentDraws(p.bim) ||
         hasForwardRasterNativePrimitiveDraws(p);
}

[[nodiscard]] BimPrimitivePassDrawLists
primitiveDrawLists(const FrameDrawLists &draws) {
  return {.opaqueDrawCommands = draws.opaqueDrawCommands,
          .opaqueSingleSidedDrawCommands = draws.opaqueSingleSidedDrawCommands,
          .opaqueWindingFlippedDrawCommands =
              draws.opaqueWindingFlippedDrawCommands,
          .opaqueDoubleSidedDrawCommands = draws.opaqueDoubleSidedDrawCommands,
          .transparentDrawCommands = draws.transparentDrawCommands,
          .transparentSingleSidedDrawCommands =
              draws.transparentSingleSidedDrawCommands,
          .transparentWindingFlippedDrawCommands =
              draws.transparentWindingFlippedDrawCommands,
          .transparentDoubleSidedDrawCommands =
              draws.transparentDoubleSidedDrawCommands};
}

[[nodiscard]] bool hasNativePoints(const FrameRecordParams &p) {
  return p.bim.primitivePasses.pointCloud.enabled &&
         hasBimPrimitivePassDrawCommands(
             primitiveDrawLists(p.bim.nativePointDraws));
}

[[nodiscard]] bool hasNativeCurves(const FrameRecordParams &p) {
  return p.bim.primitivePasses.curves.enabled &&
         hasBimPrimitivePassDrawCommands(
             primitiveDrawLists(p.bim.nativeCurveDraws));
}

void recordNativePrimitives(VkCommandBuffer cmd, const FrameRecordParams &p,
                            const DebugOverlayRenderer &overlay,
                            VkExtent2D extent) {
  const auto layout =
      forwardRasterPipelineLayout(p, ForwardRasterPipelineLayoutId::Wireframe);
  const BimPrimitivePassGeometryBinding geometry{
      .sceneDescriptorSet =
          forwardRasterDescriptorSet(p, ForwardRasterDescriptorSetId::BimScene),
      .vertexSlice = p.bim.scene.vertexSlice,
      .indexSlice = p.bim.scene.indexSlice,
      .indexType = p.bim.scene.indexType};
  const auto &points = p.bim.primitivePasses.pointCloud;
  const auto &curves = p.bim.primitivePasses.curves;
  const auto pointDepth = forwardRasterPipelineHandle(
      p, ForwardRasterPipelineId::BimPointCloudDepth);
  const auto curveDepth =
      forwardRasterPipelineHandle(p, ForwardRasterPipelineId::BimCurveDepth);
  (void)recordBimPrimitiveFramePassCommands(
      cmd,
      {.style = {.kind = BimPrimitivePassKind::Points,
                 .enabled = points.enabled,
                 .depthTest = points.depthTest,
                 .nativeDrawsUseGpuVisibility =
                     p.bim.nativePointDrawsUseGpuVisibility,
                 .opacity = points.opacity,
                 .primitiveSize = points.pointSize,
                 .color = points.color},
       .nativeDraws = primitiveDrawLists(p.bim.nativePointDraws),
       .geometry = geometry,
       .pipelines = {.depth = pointDepth,
                     .noDepth = forwardRasterPipelineHandle(
                         p, ForwardRasterPipelineId::BimPointCloudNoDepth)},
       .wireframeLayout = layout,
       .pushConstants = p.pushConstants.wireframe,
       .debugOverlay = &overlay,
       .bimManager = p.services.bimManager});
  (void)recordBimPrimitiveFramePassCommands(
      cmd,
      {.style = {.kind = BimPrimitivePassKind::Curves,
                 .enabled = curves.enabled,
                 .depthTest = curves.depthTest,
                 .nativeDrawsUseGpuVisibility =
                     p.bim.nativeCurveDrawsUseGpuVisibility,
                 .opacity = curves.opacity,
                 .primitiveSize = curves.lineWidth,
                 .color = curves.color,
                 .recordLineWidth = true,
                 .wideLinesSupported = p.debug.wireframeWideLinesSupported},
       .nativeDraws = primitiveDrawLists(p.bim.nativeCurveDraws),
       .geometry = geometry,
       .pipelines = {.depth = curveDepth,
                     .noDepth = forwardRasterPipelineHandle(
                         p, ForwardRasterPipelineId::BimCurveNoDepth)},
       .wireframeLayout = layout,
       .pushConstants = p.pushConstants.wireframe,
       .debugOverlay = &overlay,
       .bimManager = p.services.bimManager});
  (void)recordBimLightingOverlayFrameCommands(
      cmd,
      {.bimGeometryReady = hasBimPrimitiveFramePassGeometry(geometry),
       .framebufferExtent = extent,
       .draws = {.nativePointHover = p.bim.nativePointDraws.hoveredDrawCommands,
                 .nativeCurveHover = p.bim.nativeCurveDraws.hoveredDrawCommands,
                 .nativePointSelection =
                     p.bim.nativePointDraws.selectedDrawCommands,
                 .nativeCurveSelection =
                     p.bim.nativeCurveDraws.selectedDrawCommands},
       .nativePointSize = points.pointSize,
       .nativeCurveLineWidth = curves.lineWidth,
       .pipelines = {.bimPointCloudDepth = pointDepth,
                     .bimCurveDepth = curveDepth},
       .wireframeLayout = layout,
       .bim = {.descriptorSet = geometry.sceneDescriptorSet,
               .vertexSlice = geometry.vertexSlice,
               .indexSlice = geometry.indexSlice,
               .indexType = geometry.indexType},
       .wireframePushConstants = p.pushConstants.wireframe,
       .debugOverlay = &overlay,
       .wireframeWideLinesSupported = p.debug.wireframeWideLinesSupported});
}

[[nodiscard]] bool hasLightingFramebuffer(const FrameRecordParams& p,
                                         ForwardRasterFramebufferId id) {
  const FrameFramebufferBinding* binding =
      forwardRasterFramebufferBinding(p, id);
  return binding != nullptr && binding->framebuffer != VK_NULL_HANDLE &&
         binding->renderPass != VK_NULL_HANDLE && binding->extent.width > 0u &&
         binding->extent.height > 0u;
}

[[nodiscard]] bool hasForwardLightSets(const FrameRecordParams& p) {
  return forwardRasterDescriptorSetReady(p, ForwardRasterDescriptorSetId::Light) &&
         forwardRasterDescriptorSetReady(
             p, ForwardRasterDescriptorSetId::FrameLighting);
}

[[nodiscard]] bool hasForwardTransparentSets(const FrameRecordParams& p) {
  return hasForwardLightSets(p) &&
         forwardRasterDescriptorSetReady(p, ForwardRasterDescriptorSetId::Oit);
}

[[nodiscard]] bool hasSceneGeometry(const FrameRecordParams& p) {
  return p.pushConstants.bindless != nullptr &&
         p.scene.vertexSlice.buffer != VK_NULL_HANDLE &&
         p.scene.indexSlice.buffer != VK_NULL_HANDLE &&
         forwardRasterDescriptorSetReady(p, ForwardRasterDescriptorSetId::Scene);
}

[[nodiscard]] bool hasBimGeometry(const FrameRecordParams& p) {
  return p.pushConstants.bindless != nullptr &&
         p.bim.scene.vertexSlice.buffer != VK_NULL_HANDLE &&
         p.bim.scene.indexSlice.buffer != VK_NULL_HANDLE &&
         forwardRasterDescriptorSetReady(p,
                                         ForwardRasterDescriptorSetId::BimScene);
}

[[nodiscard]] bool hasForwardOpaquePipeline(const FrameRecordParams& p) {
  return forwardRasterPipelineReady(p,
                                    ForwardRasterPipelineId::ForwardOpaque) &&
         forwardRasterPipelineReady(
             p, ForwardRasterPipelineId::ForwardOpaqueFrontCull) &&
         forwardRasterPipelineReady(
             p, ForwardRasterPipelineId::ForwardOpaqueNoCull);
}

[[nodiscard]] bool hasForwardTransparentPipelines(const FrameRecordParams& p) {
  return forwardRasterPipelineReady(p, ForwardRasterPipelineId::Transparent) &&
         forwardRasterPipelineReady(p,
                                    ForwardRasterPipelineId::TransparentFrontCull) &&
         forwardRasterPipelineReady(p,
                                    ForwardRasterPipelineId::TransparentNoCull);
}

[[nodiscard]] std::array<VkClearValue, 2> lightingClearValues() {
  std::array<VkClearValue, 2> clearValues{};
  clearValues[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
  clearValues[1].depthStencil = {0.0f, 0u};
  return clearValues;
}

[[nodiscard]] container::gpu::BindlessPushConstants bindlessPushConstants(
    const FrameRecordParams& p) {
  return p.pushConstants.bindless != nullptr
             ? *p.pushConstants.bindless
             : container::gpu::BindlessPushConstants{};
}

[[nodiscard]] std::array<VkDescriptorSet, 4> forwardDescriptorSets(
    const FrameRecordParams& p, ForwardRasterDescriptorSetId sceneSet) {
  return {forwardRasterDescriptorSet(p, sceneSet),
          forwardRasterDescriptorSet(p, ForwardRasterDescriptorSetId::Light),
          forwardRasterDescriptorSet(p, ForwardRasterDescriptorSetId::Oit),
          forwardRasterDescriptorSet(p,
                                     ForwardRasterDescriptorSetId::FrameLighting)};
}

[[nodiscard]] std::array<VkDescriptorSet, 2> forwardOpaqueDescriptorSets(
    const FrameRecordParams& p, ForwardRasterDescriptorSetId sceneSet) {
  return {forwardRasterDescriptorSet(p, sceneSet),
          forwardRasterDescriptorSet(p, ForwardRasterDescriptorSetId::Light)};
}

void bindForwardOpaqueLightingSets(VkCommandBuffer cmd, VkPipelineLayout layout,
                                   const FrameRecordParams& p) {
  const VkDescriptorSet lightSet =
      forwardRasterDescriptorSet(p, ForwardRasterDescriptorSetId::Light);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1u, 1u,
                          &lightSet, 0u, nullptr);

  const VkDescriptorSet frameLightingSet = forwardRasterDescriptorSet(
      p, ForwardRasterDescriptorSetId::FrameLighting);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 3u, 1u,
                          &frameLightingSet, 0u, nullptr);
}

void recordSceneOpaque(VkCommandBuffer cmd, const FrameRecordParams& p,
                       VkPipelineLayout layout,
                       const DebugOverlayRenderer& debugOverlay) {
  if (!hasSceneOpaqueDraws(p)) {
    return;
  }

  const SceneOpaqueDrawPlan plan =
      buildSceneOpaqueDrawPlan({
          .gpuIndirectAvailable = p.services.gpuCullManager != nullptr &&
              p.services.gpuCullManager->frustumDrawsValid(p.runtime.imageIndex),
          .occludedGpuIndirectAvailable = p.services.gpuCullManager != nullptr &&
              p.services.gpuCullManager->occlusionDrawsValid(p.runtime.imageIndex),
          .preferOccludedGpuIndirect = true,
          .draws = sceneOpaqueDrawLists(p.draws)});
  const VkPipeline pipeline =
      forwardRasterPipelineHandle(p, ForwardRasterPipelineId::ForwardOpaque);
  bindForwardOpaqueLightingSets(cmd, layout, p);
  (void)recordSceneOpaqueDrawCommands(
      cmd,
      {.plan = &plan,
       .geometry = {.descriptorSet = forwardRasterDescriptorSet(
                        p, ForwardRasterDescriptorSetId::Scene),
                    .vertexSlice = p.scene.vertexSlice,
                    .indexSlice = p.scene.indexSlice,
                    .indexType = p.scene.indexType},
       .pipelines = {.primary = pipeline,
                     .frontCull = forwardRasterPipelineHandle(
                         p, ForwardRasterPipelineId::ForwardOpaqueFrontCull),
                     .noCull = forwardRasterPipelineHandle(
                         p, ForwardRasterPipelineId::ForwardOpaqueNoCull)},
       .pipelineLayout = layout,
       .pushConstants = bindlessPushConstants(p),
       .imageIndex = p.runtime.imageIndex,
       .debugOverlay = &debugOverlay,
       .gpuCullManager = p.services.gpuCullManager});
}

void recordSceneTransparent(VkCommandBuffer cmd, const FrameRecordParams& p,
                            VkPipelineLayout layout,
                            const DebugOverlayRenderer& debugOverlay) {
  if (!hasSceneTransparentDraws(p)) {
    return;
  }

  const std::array<VkDescriptorSet, 4> descriptorSets =
      forwardDescriptorSets(p, ForwardRasterDescriptorSetId::Scene);
  const SceneTransparentDrawPlan plan =
      buildSceneTransparentDrawPlan(sceneTransparentDrawLists(p.draws));
  (void)recordSceneTransparentDrawCommands(
      cmd, {.plan = &plan,
            .geometry = {.descriptorSets = descriptorSets,
                         .vertexSlice = p.scene.vertexSlice,
                         .indexSlice = p.scene.indexSlice,
                         .indexType = p.scene.indexType},
            .pipelines = {.primary = forwardRasterPipelineHandle(
                              p, ForwardRasterPipelineId::Transparent),
                          .frontCull = forwardRasterPipelineHandle(
                              p, ForwardRasterPipelineId::TransparentFrontCull),
                          .noCull = forwardRasterPipelineHandle(
                              p, ForwardRasterPipelineId::TransparentNoCull)},
            .pipelineLayout = layout,
            .pushConstants = bindlessPushConstants(p),
            .debugOverlay = &debugOverlay});
}

void recordBimSurface(VkCommandBuffer cmd, const FrameRecordParams& p,
                      BimSurfacePassKind kind, bool passReady,
                      VkPipeline singleSided, VkPipeline windingFlipped,
                      VkPipeline doubleSided, VkPipelineLayout layout,
                      const DebugOverlayRenderer& debugOverlay) {
  if (!passReady) {
    return;
  }

  const bool transparentPass =
      kind == BimSurfacePassKind::TransparentLighting;
  const std::array<VkDescriptorSet, 2> opaqueDescriptorSets =
      forwardOpaqueDescriptorSets(p, ForwardRasterDescriptorSetId::BimScene);
  const std::array<VkDescriptorSet, 4> transparentDescriptorSets =
      forwardDescriptorSets(p, ForwardRasterDescriptorSetId::BimScene);
  const std::span<const VkDescriptorSet> descriptorSets =
      transparentPass ? std::span<const VkDescriptorSet>(transparentDescriptorSets)
                      : std::span<const VkDescriptorSet>(opaqueDescriptorSets);
  container::gpu::BindlessPushConstants basePushConstants =
      bindlessPushConstants(p);
  basePushConstants.semanticColorMode = p.bim.semanticColorMode;
  if (!transparentPass) {
    bindForwardOpaqueLightingSets(cmd, layout, p);
  }
  const BimSurfacePassPlan plan = buildBimSurfaceFramePassPlan(
      {.kind = kind,
       .passReady = true,
       .draws = bimSurfaceDrawSources(p.bim),
       .geometry = {.descriptorSets = descriptorSets,
                    .vertexSlice = p.bim.scene.vertexSlice,
                    .indexSlice = p.bim.scene.indexSlice,
                    .indexType = p.bim.scene.indexType},
       .pipelines = {.singleSided = singleSided,
                     .windingFlipped = windingFlipped,
                     .doubleSided = doubleSided},
       .pushConstants = &basePushConstants,
       .semanticColorMode = p.bim.semanticColorMode});
  (void)recordBimSurfacePassCommands(
      cmd, {.plan = &plan,
            .geometry = {.descriptorSets = descriptorSets,
                         .vertexSlice = p.bim.scene.vertexSlice,
                         .indexSlice = p.bim.scene.indexSlice,
                         .indexType = p.bim.scene.indexType},
            .singleSidedPipeline = singleSided,
            .windingFlippedPipeline = windingFlipped,
            .doubleSidedPipeline = doubleSided,
            .pipelineLayout = layout,
            .pushConstants = bimSurfaceRasterPassPushConstants(
                basePushConstants, plan),
            .debugOverlay = &debugOverlay,
            .bimManager = p.services.bimManager});
}

}  // namespace

bool hasForwardRasterNativePrimitiveDraws(const FrameRecordParams &p) {
  return hasNativePoints(p) || hasNativeCurves(p);
}

RenderPassReadiness
checkForwardRasterLightingPassReadiness(const FrameRecordParams& p) {
  if (!hasAnyDraws(p)) {
    return notNeeded();
  }

  const bool sceneOpaqueDraws = hasSceneOpaqueDraws(p);
  const bool sceneTransparentDraws = hasSceneTransparentDraws(p);
  const bool bimOpaqueDraws = hasBimOpaqueDraws(p.bim);
  const bool bimTransparentDraws = hasBimTransparentDraws(p.bim);
  if (hasForwardRasterNativePrimitiveDraws(p)) {
    if (p.pushConstants.wireframe == nullptr || !hasBimGeometry(p))
      return missing(RenderResourceId::BimGeometry);
    if (!forwardRasterPipelineLayoutReady(
            p, ForwardRasterPipelineLayoutId::Wireframe) ||
        (hasNativePoints(p) &&
         !forwardRasterPipelineReady(
             p, p.bim.primitivePasses.pointCloud.depthTest
                    ? ForwardRasterPipelineId::BimPointCloudDepth
                    : ForwardRasterPipelineId::BimPointCloudNoDepth)) ||
        (hasNativeCurves(p) &&
         !forwardRasterPipelineReady(
             p, p.bim.primitivePasses.curves.depthTest
                    ? ForwardRasterPipelineId::BimCurveDepth
                    : ForwardRasterPipelineId::BimCurveNoDepth)))
      return missing(RenderResourceId::SceneColor);
  }
  if (!hasLightingFramebuffer(p, ForwardRasterFramebufferId::Lighting) ||
      !hasLightingFramebuffer(p,
                              ForwardRasterFramebufferId::TransparentLighting) ||
      !forwardRasterPipelineLayoutReady(
          p, ForwardRasterPipelineLayoutId::Transparent)) {
    return missing(RenderResourceId::SceneColor);
  }

  if ((sceneOpaqueDraws || bimOpaqueDraws) && !hasForwardOpaquePipeline(p)) {
    return missing(RenderResourceId::SceneColor);
  }
  if ((sceneOpaqueDraws || sceneTransparentDraws) && !hasSceneGeometry(p)) {
    return missing(RenderResourceId::SceneGeometry);
  }
  if ((bimOpaqueDraws || bimTransparentDraws) && !hasBimGeometry(p)) {
    return missing(RenderResourceId::BimGeometry);
  }
  if ((sceneOpaqueDraws || bimOpaqueDraws) && !hasForwardLightSets(p)) {
    return missing(RenderResourceId::SceneGeometry);
  }
  if ((sceneTransparentDraws || bimTransparentDraws) &&
      (!hasForwardTransparentSets(p) || !hasForwardTransparentPipelines(p))) {
    return missing(RenderResourceId::SceneColor);
  }

  return ready();
}

bool recordForwardRasterLightingPassCommands(VkCommandBuffer commandBuffer,
                                             const FrameRecordParams& p) {
  if (commandBuffer == VK_NULL_HANDLE ||
      !checkForwardRasterLightingPassReadiness(p).ready) {
    return false;
  }

  const FrameFramebufferBinding* framebuffer =
      forwardRasterFramebufferBinding(p, ForwardRasterFramebufferId::Lighting);
  const FrameFramebufferBinding* transparentFramebuffer =
      forwardRasterFramebufferBinding(
          p, ForwardRasterFramebufferId::TransparentLighting);
  if (framebuffer == nullptr || transparentFramebuffer == nullptr) {
    return false;
  }

  const std::array<VkClearValue, 2> clearValues = lightingClearValues();
  if (!recordRenderPassBeginCommands(
          commandBuffer,
          {.renderPass = framebuffer->renderPass,
           .framebuffer = framebuffer->framebuffer,
           .renderArea = {.offset = {0, 0}, .extent = framebuffer->extent},
           .clearValues = clearValues})) {
    return false;
  }

  recordSceneViewportAndScissor(commandBuffer, framebuffer->extent);
  const DebugOverlayRenderer debugOverlay{};
  const VkPipelineLayout layout = forwardRasterPipelineLayout(
      p, ForwardRasterPipelineLayoutId::Transparent);

  recordSceneOpaque(commandBuffer, p, layout, debugOverlay);

  const VkPipeline opaquePipeline =
      forwardRasterPipelineHandle(p, ForwardRasterPipelineId::ForwardOpaque);
  recordBimSurface(commandBuffer, p, BimSurfacePassKind::OpaqueLighting,
                   hasBimOpaqueDraws(p.bim), opaquePipeline,
                   forwardRasterPipelineHandle(
                       p, ForwardRasterPipelineId::ForwardOpaqueFrontCull),
                   forwardRasterPipelineHandle(
                       p, ForwardRasterPipelineId::ForwardOpaqueNoCull),
                   layout, debugOverlay);

  (void)recordRenderPassEndCommands(commandBuffer);

  // OIT uses single-sample storage. Load the resolved opaque color and depth
  // in a separate scope, also preparing depth for post-processing on opaque-only
  // frames.
  if (!recordRenderPassBeginCommands(
          commandBuffer,
          {.renderPass = transparentFramebuffer->renderPass,
           .framebuffer = transparentFramebuffer->framebuffer,
           .renderArea = {.offset = {0, 0},
                          .extent = transparentFramebuffer->extent},
           .clearValues = clearValues})) {
    return false;
  }
  recordSceneViewportAndScissor(commandBuffer, transparentFramebuffer->extent);

  recordSceneTransparent(commandBuffer, p, layout, debugOverlay);
  recordBimSurface(
      commandBuffer, p, BimSurfacePassKind::TransparentLighting,
      hasBimTransparentDraws(p.bim),
      forwardRasterPipelineHandle(p, ForwardRasterPipelineId::Transparent),
      forwardRasterPipelineHandle(p,
                                  ForwardRasterPipelineId::TransparentFrontCull),
      forwardRasterPipelineHandle(p, ForwardRasterPipelineId::TransparentNoCull),
      layout, debugOverlay);

  recordNativePrimitives(commandBuffer, p, debugOverlay,
                         transparentFramebuffer->extent);

  (void)recordRenderPassEndCommands(commandBuffer);
  return true;
}

}  // namespace container::renderer
