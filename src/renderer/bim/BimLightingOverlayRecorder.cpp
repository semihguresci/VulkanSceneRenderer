#include "Container/renderer/bim/BimLightingOverlayRecorder.h"

#include "Container/renderer/bim/BimFloorPlanDisplay.h"
#include "Container/renderer/bim/BimPrimitivePassRecorder.h"
#include "Container/renderer/bim/BimSectionClipCapPassRecorder.h"
#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/deferred/DeferredRasterDebugOverlayRecorder.h"
#include "Container/renderer/deferred/DeferredRasterLighting.h"
#include "Container/renderer/forward/ForwardRasterPipelineBridge.h"
#include "Container/renderer/forward/ForwardRasterResourceBridge.h"
#include "Container/renderer/scene/SceneViewport.h"

#include <glm/gtc/color_space.hpp>

#include <algorithm>

namespace container::renderer {

namespace {

[[nodiscard]] bool hasDrawCommands(const std::vector<DrawCommand> *commands) {
  return commands != nullptr && !commands->empty();
}

[[nodiscard]] bool presentsBimXrayClash(const FrameRecordParams &p) {
  const auto technique = p.runtime.activeTechnique;
  const auto mode = static_cast<ui::GBufferViewMode>(p.debug.displayMode);
  const auto &markers = p.bim.coordinationMarkers;
  return (technique == RenderTechniqueId::ForwardRaster || technique == RenderTechniqueId::DeferredRaster) &&
         (mode == ui::GBufferViewMode::Lit || mode == ui::GBufferViewMode::Overview) &&
         !p.screenshot.sceneLinear && markers.clashMarkersEnabled && !markers.clashDepthTest &&
         hasDrawCommands(p.bim.coordinationClashMarkerDrawCommands);
}

[[nodiscard]] bool presentsBimMeshSelection(const FrameRecordParams &p) {
  const auto technique = p.runtime.activeTechnique;
  const auto mode = static_cast<ui::GBufferViewMode>(p.debug.displayMode);
  return (technique == RenderTechniqueId::ForwardRaster || technique == RenderTechniqueId::DeferredRaster) &&
         (mode == ui::GBufferViewMode::Lit || mode == ui::GBufferViewMode::Overview) &&
         !p.screenshot.sceneLinear && hasDrawCommands(p.bim.draws.selectedDrawCommands);
}

BimLightingOverlayGeometryBinding overlayGeometry(
    VkDescriptorSet descriptorSet, const FrameSceneGeometry &scene) {
  return {.descriptorSet = descriptorSet,
          .vertexSlice = scene.vertexSlice,
          .indexSlice = scene.indexSlice,
          .indexType = scene.indexType};
}

BimPrimitivePassDrawLists primitiveDrawLists(const FrameDrawLists &draws) {
  return {.opaqueDrawCommands = draws.opaqueDrawCommands,
          .opaqueSingleSidedDrawCommands = draws.opaqueSingleSidedDrawCommands,
          .opaqueWindingFlippedDrawCommands = draws.opaqueWindingFlippedDrawCommands,
          .opaqueDoubleSidedDrawCommands = draws.opaqueDoubleSidedDrawCommands,
          .transparentDrawCommands = draws.transparentDrawCommands,
          .transparentSingleSidedDrawCommands = draws.transparentSingleSidedDrawCommands,
          .transparentWindingFlippedDrawCommands = draws.transparentWindingFlippedDrawCommands,
          .transparentDoubleSidedDrawCommands = draws.transparentDoubleSidedDrawCommands};
}

[[nodiscard]] bool hasDepthTestedNativePrimitive(
    bool enabled, bool depthTest, const BimPrimitivePassDrawLists &draws) {
  return enabled && depthTest && hasBimPrimitivePassDrawCommands(draws);
}

BimLightingOverlayDrawLists overlayDrawLists(const FrameDrawLists &draws) {
  return {.opaqueSingleSidedDrawCommands = draws.opaqueSingleSidedDrawCommands,
          .transparentSingleSidedDrawCommands = draws.transparentSingleSidedDrawCommands,
          .opaqueWindingFlippedDrawCommands = draws.opaqueWindingFlippedDrawCommands,
          .transparentWindingFlippedDrawCommands = draws.transparentWindingFlippedDrawCommands,
          .opaqueDoubleSidedDrawCommands = draws.opaqueDoubleSidedDrawCommands,
          .transparentDoubleSidedDrawCommands = draws.transparentDoubleSidedDrawCommands};
}

BimSectionClipCapFramePassStyle sectionClipCapStyle(
    const FrameBimResources &bim, bool wideLinesSupported) {
  const auto &style = bim.sectionClipCaps;
  const auto &elevation = bim.technicalElevation;
  const auto &geometry = bim.sectionClipCapGeometry;
  const bool technicalCaps = elevation.enabled && elevation.sectionCapsEnabled;
  return {.enabled = style.enabled || technicalCaps,
          .fillEnabled = technicalCaps ? elevation.capFillEnabled : style.fillEnabled,
          .hatchEnabled = technicalCaps ? elevation.capHatchingEnabled : style.hatchEnabled,
          .wideLinesSupported = wideLinesSupported,
          .fillColor = style.fillColor,
          .hatchColor = style.hatchColor,
          .hatchLineWidth = technicalCaps
                                ? std::max(style.hatchLineWidth, elevation.lineWidth)
                                : style.hatchLineWidth,
          .fillDrawCommands = geometry.fillDrawCommands,
          .hatchDrawCommands = geometry.hatchDrawCommands,
          .fillDrawStyles = geometry.fillDrawStyles,
          .hatchDrawStyles = geometry.hatchDrawStyles,
          .sectionMarkerLines = geometry.sectionMarkerLines};
}

[[nodiscard]] bool
hasRequiredInputs(const BimLightingOverlayRecordInputs &inputs) {
  return inputs.plan != nullptr && inputs.wireframeLayout != VK_NULL_HANDLE &&
         inputs.wireframePushConstants != nullptr &&
         inputs.debugOverlay != nullptr;
}

[[nodiscard]] VkPipeline choosePipeline(VkPipeline preferred,
                                        VkPipeline fallback) {
  return preferred != VK_NULL_HANDLE ? preferred : fallback;
}

[[nodiscard]] uint32_t nonzeroExtent(uint32_t value) {
  return value == 0u ? 1u : value;
}

[[nodiscard]] BimLightingOverlayPipelineReadiness
bimLightingOverlayPipelineReadiness(
    const BimLightingOverlayPipelineHandles &pipelines) {
  return {.wireframeDepth = pipelines.wireframeDepth != VK_NULL_HANDLE,
          .wireframeNoDepth = pipelines.wireframeNoDepth != VK_NULL_HANDLE,
          .bimFloorPlanDepth = pipelines.bimFloorPlanDepth != VK_NULL_HANDLE,
          .bimFloorPlanNoDepth =
              pipelines.bimFloorPlanNoDepth != VK_NULL_HANDLE,
          .bimPointCloudDepth = pipelines.bimPointCloudDepth != VK_NULL_HANDLE,
          .bimCurveDepth = pipelines.bimCurveDepth != VK_NULL_HANDLE,
          .bimCurveStrokeDepth = pipelines.bimCurveStrokeDepth != VK_NULL_HANDLE,
          .selectionMask = pipelines.selectionMask != VK_NULL_HANDLE,
          .selectionOutline = pipelines.selectionOutline != VK_NULL_HANDLE};
}

[[nodiscard]] BimLightingOverlayStyleInputs
bimLightingOverlayStyleInputs(const BimLightingOverlayFrameStyleState &style,
                              BimLightingOverlayDrawLists draws) {
  return {.enabled = style.enabled,
          .depthTest = style.depthTest,
          .color = style.color,
          .opacity = style.opacity,
          .lineWidth = style.lineWidth,
          .draws = draws};
}

[[nodiscard]] BimLightingFloorPlanOverlayInputs
bimLightingFloorPlanOverlayInputs(
    const BimLightingOverlayFrameStyleState &style,
    const std::vector<DrawCommand> *commands) {
  return {.enabled = style.enabled,
          .depthTest = style.depthTest,
          .color = style.color,
          .opacity = style.opacity,
          .lineWidth = style.lineWidth,
          .commands = commands};
}

[[nodiscard]] VkPipeline pipelineForBimLightingOverlay(
    BimLightingOverlayPipeline pipeline,
    const BimLightingOverlayPipelineHandles &pipelines) {
  switch (pipeline) {
  case BimLightingOverlayPipeline::WireframeDepth:
    return pipelines.wireframeDepth;
  case BimLightingOverlayPipeline::WireframeNoDepth:
    return pipelines.wireframeNoDepth;
  case BimLightingOverlayPipeline::WireframeDepthFrontCull:
    return choosePipeline(pipelines.wireframeDepthFrontCull,
                          pipelines.wireframeDepth);
  case BimLightingOverlayPipeline::WireframeNoDepthFrontCull:
    return choosePipeline(pipelines.wireframeNoDepthFrontCull,
                          pipelines.wireframeNoDepth);
  case BimLightingOverlayPipeline::BimFloorPlanDepth:
    return pipelines.bimFloorPlanDepth;
  case BimLightingOverlayPipeline::BimFloorPlanNoDepth:
    return pipelines.bimFloorPlanNoDepth;
  case BimLightingOverlayPipeline::BimPointCloudDepth:
    return pipelines.bimPointCloudDepth;
  case BimLightingOverlayPipeline::BimCurveDepth:
    return pipelines.bimCurveDepth;
  case BimLightingOverlayPipeline::BimCurveStrokeDepth:
    return pipelines.bimCurveStrokeDepth;
  }
  return VK_NULL_HANDLE;
}

void bindGeometry(VkCommandBuffer cmd,
                  const BimLightingOverlayGeometryBinding &geometry,
                  VkPipelineLayout layout) {
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1,
                          &geometry.descriptorSet, 0, nullptr);
  const VkDeviceSize offsets[] = {geometry.vertexSlice.offset};
  vkCmdBindVertexBuffers(cmd, 0, 1, &geometry.vertexSlice.buffer, offsets);
  vkCmdBindIndexBuffer(cmd, geometry.indexSlice.buffer,
                       geometry.indexSlice.offset, geometry.indexType);
}

[[nodiscard]] bool
bindGeometryIfReady(VkCommandBuffer cmd,
                    const BimLightingOverlayGeometryBinding &geometry,
                    VkPipelineLayout layout) {
  if (geometry.descriptorSet == VK_NULL_HANDLE ||
      geometry.vertexSlice.buffer == VK_NULL_HANDLE ||
      geometry.indexSlice.buffer == VK_NULL_HANDLE) {
    return false;
  }
  bindGeometry(cmd, geometry, layout);
  return true;
}

void bindWireframePipeline(VkCommandBuffer cmd, VkPipeline pipeline,
                           float lineWidth,
                           const BimLightingOverlayRecordInputs &inputs) {
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  if (inputs.wireframeRasterModeSupported) {
    vkCmdSetLineWidth(cmd,
                      inputs.wireframeWideLinesSupported ? lineWidth : 1.0f);
  }
}

bool drawStyleOverlayRoutes(VkCommandBuffer cmd,
                            const BimLightingOverlayStylePlan &plan,
                            const BimLightingOverlayRecordInputs &inputs,
                            WireframePushConstants &pushConstants) {
  if (!plan.active) {
    return false;
  }

  bool recorded = false;
  for (uint32_t routeIndex = 0u; routeIndex < plan.routeCount; ++routeIndex) {
    const BimLightingOverlayDrawRoute &route = plan.routes[routeIndex];
    const VkPipeline pipeline =
        pipelineForBimLightingOverlay(route.pipeline, inputs.pipelines);
    if (!hasDrawCommands(route.commands) || pipeline == VK_NULL_HANDLE) {
      continue;
    }
    bindWireframePipeline(cmd, pipeline, route.rasterLineWidth, inputs);
    inputs.debugOverlay->drawWireframe(
        cmd, inputs.wireframeLayout, *route.commands, plan.color, plan.opacity,
        plan.drawLineWidth, pushConstants);
    recorded = true;
  }
  return recorded;
}

bool drawOverlayPlan(VkCommandBuffer cmd,
                     const BimLightingOverlayGeometryBinding &geometry,
                     const BimLightingOverlayDrawPlan &plan,
                     bool useWireframePipelineBinding,
                     const BimLightingOverlayRecordInputs &inputs,
                     WireframePushConstants &pushConstants) {
  if (!plan.active || !hasDrawCommands(plan.commands)) {
    return false;
  }
  const VkPipeline pipeline =
      pipelineForBimLightingOverlay(plan.pipeline, inputs.pipelines);
  if (pipeline == VK_NULL_HANDLE ||
      !bindGeometryIfReady(cmd, geometry, inputs.wireframeLayout)) {
    return false;
  }

  if (useWireframePipelineBinding) {
    bindWireframePipeline(cmd, pipeline, plan.rasterLineWidth, inputs);
  } else {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    if (plan.rasterLineWidthApplies) {
      const float lineWidth = inputs.lineListWideLinesSupported
                                  ? plan.drawLineWidth
                                  : 1.0f;
      vkCmdSetLineWidth(cmd, lineWidth);
    }
  }
  WireframePushConstants drawPushConstants = pushConstants;
  if (plan.kind == BimLightingOverlayKind::SectionPlaneVisual) {
    drawPushConstants.sectionPlaneEnabled = 0u;
  }
  inputs.debugOverlay->drawWireframe(cmd, inputs.wireframeLayout,
                                     *plan.commands, plan.color, plan.opacity,
                                     plan.drawLineWidth, drawPushConstants);
  return true;
}

bool drawSelectionOutline(VkCommandBuffer cmd,
                          const BimLightingOverlayGeometryBinding &geometry,
                          const BimLightingSelectionOutlinePlan &plan,
                          const BimLightingOverlayRecordInputs &inputs) {
  if (!plan.active || !hasDrawCommands(plan.commands) ||
      inputs.pipelines.selectionMask == VK_NULL_HANDLE ||
      inputs.pipelines.selectionOutline == VK_NULL_HANDLE ||
      !bindGeometryIfReady(cmd, geometry, inputs.wireframeLayout)) {
    return false;
  }

  WireframePushConstants pushConstants = *inputs.wireframePushConstants;
  pushConstants.padding7 = static_cast<float>(plan.framebufferWidth);
  pushConstants.padding8 = static_cast<float>(plan.framebufferHeight);

  vkCmdClearAttachments(cmd, 1, &inputs.selectionStencilClearAttachment, 1,
                        &inputs.selectionStencilClearRect);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    inputs.pipelines.selectionMask);
  inputs.debugOverlay->drawWireframe(cmd, inputs.wireframeLayout,
                                     *plan.commands, plan.color, 1.0f,
                                     plan.maskLineWidth, pushConstants);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    inputs.pipelines.selectionOutline);
  // Fill the complete pixel disk, including offsets inside its boundary, so
  // thin features and corners join into one continuous opaque stroke.
  inputs.debugOverlay->drawSelectionStroke(cmd, inputs.wireframeLayout,
                                           *plan.commands, plan.color,
                                           pushConstants);
  return true;
}

} // namespace

BimLightingOverlayInputs buildBimLightingOverlayFramePlanInputs(
    const BimLightingOverlayFrameRecordInputs &inputs) {
  return {
      .bimGeometryReady = inputs.bimGeometryReady,
      .wireframeLayoutReady = inputs.wireframeLayout != VK_NULL_HANDLE,
      .wireframePushConstantsReady = inputs.wireframePushConstants != nullptr,
      .wideLinesSupported = inputs.wireframeWideLinesSupported,
      .framebufferWidth = nonzeroExtent(inputs.framebufferExtent.width),
      .framebufferHeight = nonzeroExtent(inputs.framebufferExtent.height),
      .pipelines = bimLightingOverlayPipelineReadiness(inputs.pipelines),
      .points =
          bimLightingOverlayStyleInputs(inputs.points, inputs.draws.points),
      .curves =
          bimLightingOverlayStyleInputs(inputs.curves, inputs.draws.curves),
      .floorPlan = bimLightingFloorPlanOverlayInputs(inputs.floorPlan,
                                                     inputs.draws.floorPlan),
      .sceneHoverCommands = inputs.draws.sceneHover,
      .bimHoverCommands = inputs.draws.bimHover,
      .sceneSelectionCommands = inputs.draws.sceneSelection,
      .bimSelectionCommands = inputs.draws.bimSelection,
      .nativePointHoverCommands = inputs.draws.nativePointHover,
      .nativeCurveHoverCommands = inputs.draws.nativeCurveHover,
      .nativePointSelectionCommands = inputs.draws.nativePointSelection,
      .nativeCurveSelectionCommands = inputs.draws.nativeCurveSelection,
      .nativeCurveStrokeHoverCommands = inputs.draws.nativeCurveStrokeHover,
      .nativeCurveStrokeSelectionCommands = inputs.draws.nativeCurveStrokeSelection,
      .coordinationIssueMarkerCommands =
          inputs.draws.coordinationIssueMarkers,
      .coordinationClashMarkerCommands =
          inputs.draws.coordinationClashMarkers,
      .sectionPlaneVisualCommands = inputs.draws.sectionPlaneVisual,
      .nativePointSize = inputs.nativePointSize,
      .nativeCurveLineWidth = inputs.nativeCurveLineWidth,
      .coordinationMarkerGeometryReady =
          inputs.coordinationMarkerGeometryReady,
      .sectionPlaneVisualGeometryReady =
          inputs.sectionPlaneVisualGeometryReady,
      .coordinationIssueMarkers = bimLightingOverlayStyleInputs(
          inputs.coordinationIssueMarkers, {}),
      .coordinationClashMarkers = bimLightingOverlayStyleInputs(
          inputs.coordinationClashMarkers, {}),
      .sectionPlaneVisual = bimLightingOverlayStyleInputs(
          inputs.sectionPlaneVisual, {})};
}

BimLightingOverlayPlan buildBimLightingOverlayFramePlan(
    const BimLightingOverlayFrameRecordInputs &inputs) {
  return buildBimLightingOverlayPlan(
      buildBimLightingOverlayFramePlanInputs(inputs));
}

bool recordBimLightingOverlayCommands(
    VkCommandBuffer cmd, const BimLightingOverlayRecordInputs &inputs) {
  if (!hasRequiredInputs(inputs)) {
    return false;
  }

  const BimLightingOverlayPlan &plan = *inputs.plan;
  bool recorded = false;
  WireframePushConstants pushConstants = *inputs.wireframePushConstants;

  if ((plan.pointStyle.active || plan.curveStyle.active) &&
      bindGeometryIfReady(cmd, inputs.bim, inputs.wireframeLayout)) {
    recorded |=
        drawStyleOverlayRoutes(cmd, plan.pointStyle, inputs, pushConstants);
    recorded |=
        drawStyleOverlayRoutes(cmd, plan.curveStyle, inputs, pushConstants);
  }

  recorded |= drawOverlayPlan(cmd, inputs.bim, plan.floorPlan, false, inputs,
                              pushConstants);
  recorded |= drawOverlayPlan(cmd, inputs.coordinationMarkers,
                              plan.coordinationIssueMarkers, true, inputs,
                              pushConstants);
  recorded |= drawOverlayPlan(cmd, inputs.coordinationMarkers,
                              plan.coordinationClashMarkers, true, inputs,
                              pushConstants);
  recorded |= drawOverlayPlan(cmd, inputs.sectionPlaneVisual,
                              plan.sectionPlaneVisual, true, inputs,
                              pushConstants);
  recorded |= drawOverlayPlan(cmd, inputs.scene, plan.sceneHover, true, inputs,
                              pushConstants);
  recorded |= drawOverlayPlan(cmd, inputs.bim, plan.bimHover, true, inputs,
                              pushConstants);

  // Native point/curve hover and selection remain CPU-filtered until their
  // primitive visibility moves to GPU-owned compaction.
  recorded |= drawOverlayPlan(cmd, inputs.bim, plan.nativePointHover, false,
                              inputs, pushConstants);
  recorded |= drawOverlayPlan(cmd, inputs.bim, plan.nativeCurveHover, false,
                              inputs, pushConstants);
  recorded |= drawOverlayPlan(cmd, inputs.bim, plan.nativeCurveStrokeHover,
                              false, inputs, pushConstants);

  recorded |= drawSelectionOutline(cmd, inputs.scene,
                                   plan.sceneSelectionOutline, inputs);
  recorded |=
      drawSelectionOutline(cmd, inputs.bim, plan.bimSelectionOutline, inputs);
  recorded |= drawOverlayPlan(cmd, inputs.bim, plan.nativePointSelection, false,
                              inputs, pushConstants);
  recorded |= drawOverlayPlan(cmd, inputs.bim, plan.nativeCurveSelection, false,
                              inputs, pushConstants);
  recorded |= drawOverlayPlan(cmd, inputs.bim, plan.nativeCurveStrokeSelection,
                              false, inputs, pushConstants);
  return recorded;
}

namespace {

bool recordBimLightingOverlayFramePlanCommands(
    VkCommandBuffer cmd, const BimLightingOverlayFrameRecordInputs &inputs,
    const BimLightingOverlayPlan &plan) {
  if (cmd == VK_NULL_HANDLE) {
    return false;
  }

  return recordBimLightingOverlayCommands(
      cmd, {.plan = &plan,
            .pipelines = inputs.pipelines,
            .wireframeLayout = inputs.wireframeLayout,
            .scene = inputs.scene,
            .bim = inputs.bim,
            .coordinationMarkers = inputs.coordinationMarkers,
            .sectionPlaneVisual = inputs.sectionPlaneVisualGeometry,
            .selectionStencilClearAttachment =
                inputs.selectionStencilClearAttachment,
            .selectionStencilClearRect = inputs.selectionStencilClearRect,
            .wireframePushConstants = inputs.wireframePushConstants,
            .debugOverlay = inputs.debugOverlay,
            .wireframeRasterModeSupported = inputs.wireframeRasterModeSupported,
            .wireframeWideLinesSupported = inputs.wireframeWideLinesSupported,
            .lineListWideLinesSupported = inputs.lineListWideLinesSupported});
}

} // namespace

bool recordBimLightingOverlayFrameCommands(
    VkCommandBuffer cmd, const BimLightingOverlayFrameRecordInputs &inputs) {
  return recordBimLightingOverlayFramePlanCommands(
      cmd, inputs, buildBimLightingOverlayFramePlan(inputs));
}

bool hasBimRasterDisplayOverlays(const FrameRecordParams &p) {
  if (p.screenshot.sceneLinear || !p.pushConstants.wireframe)
    return false;

  const auto &bim = p.bim;
  const auto cap = sectionClipCapStyle(bim, p.debug.lineListWideLinesSupported);
  if (cap.enabled && bim.sectionClipCapGeometry.valid())
    return true;

  const auto displayMode = static_cast<ui::GBufferViewMode>(p.debug.displayMode);
  if ((bim.floorPlan.enabled && hasDrawCommands(bim.floorPlanDrawCommands) &&
       !bimFloorPlanUsesPresentationOverlay(p.runtime.activeTechnique, displayMode, bim.floorPlan.depthTest)) ||
      (bim.coordinationMarkers.issueMarkersEnabled &&
       hasDrawCommands(bim.coordinationIssueMarkerDrawCommands)) ||
      (bim.coordinationMarkers.clashMarkersEnabled &&
       !presentsBimXrayClash(p) &&
       hasDrawCommands(bim.coordinationClashMarkerDrawCommands)) ||
      (bim.sectionPlaneVisual.enabled && hasDrawCommands(bim.sectionPlaneVisualDrawCommands)))
    return true;

  const auto pointDraws = primitiveDrawLists(bim.pointDraws);
  const auto curveDraws = primitiveDrawLists(bim.curveDraws);
  const auto &points = bim.primitivePasses.pointCloud;
  const auto &curves = bim.primitivePasses.curves;
  const bool previewPoints = points.enabled && points.placeholderRangePreviewEnabled &&
      !hasBimPrimitivePassDrawCommands(primitiveDrawLists(bim.nativePointDraws));
  const bool previewCurves = curves.enabled && curves.placeholderRangePreviewEnabled &&
      !hasBimPrimitivePassDrawCommands(primitiveDrawLists(bim.nativeCurveDraws));
  if (((bim.pointCurveStyle.points.enabled || previewPoints) &&
       hasBimPrimitivePassDrawCommands(pointDraws)) ||
      ((bim.pointCurveStyle.curves.enabled || previewCurves) &&
       hasBimPrimitivePassDrawCommands(curveDraws)))
    return true;

  const std::array drawLists{&p.draws, &bim.draws, &bim.nativePointDraws,
                             &bim.nativeCurveDraws, &bim.nativeCurveStrokeDraws};
  return std::ranges::any_of(drawLists, [](const auto *draws) {
    return hasDrawCommands(draws->hoveredDrawCommands) ||
           hasDrawCommands(draws->selectedDrawCommands);
  });
}

bool hasBimNativeOitDrawCommands(const FrameRecordParams &p) {
  const auto &bim = p.bim;
  const auto &points = bim.primitivePasses.pointCloud;
  const auto &curves = bim.primitivePasses.curves;
  return hasDepthTestedNativePrimitive(points.enabled, points.depthTest,
                                      primitiveDrawLists(bim.nativePointDraws)) ||
         hasDepthTestedNativePrimitive(curves.enabled, curves.depthTest,
                                      primitiveDrawLists(bim.nativeCurveDraws)) ||
         hasDepthTestedNativePrimitive(curves.enabled, curves.depthTest,
                                      primitiveDrawLists(bim.nativeCurveStrokeDraws));
}

void recordBimRasterOverlays(VkCommandBuffer cmd, const FrameRecordParams &p,
                             const BimRasterOverlayRecordInputs &inputs,
                             BimRasterOverlayPhase phase) {
  const auto &bim = p.bim;
  const bool display = !p.screenshot.sceneLinear && phase != BimRasterOverlayPhase::Native;
  const bool native = phase != BimRasterOverlayPhase::Display;
  if (!display && !native)
    return;
  const auto &floorPlan = bim.floorPlan;
  const auto &markers = bim.coordinationMarkers;
  const auto displayMode = static_cast<ui::GBufferViewMode>(p.debug.displayMode);
  const bool lineListWideLinesSupported = p.debug.lineListWideLinesSupported;
  const VkPipelineLayout oitLayout =
      inputs.nativeOitSet != VK_NULL_HANDLE
          ? p.pipelineLayout(RenderTechniqueId::ForwardRaster, kBimNativeOitLayout)
          : VK_NULL_HANDLE;
  const bool reflectionDiagnostic = p.runtime.activeTechnique == RenderTechniqueId::DeferredRaster &&
      screenReflectionDebugView(p.debug.screenReflections) != 0u;
  const bool sortDisplayOverlays = oitLayout != VK_NULL_HANDLE && !p.screenshot.sceneLinear &&
      (displayMode == ui::GBufferViewMode::Lit || displayMode == ui::GBufferViewMode::Overview) &&
      !reflectionDiagnostic && inputs.wireframePushConstants && inputs.debugOverlay;

  const auto capStyle = sectionClipCapStyle(bim, lineListWideLinesSupported);
  if (capStyle.enabled) {
    const VkPipeline fillOit = sortDisplayOverlays
        ? p.pipelineHandle(RenderTechniqueId::ForwardRaster, kBimSectionCapFillOitPipeline)
        : VK_NULL_HANDLE;
    const VkPipeline hatchOit = sortDisplayOverlays
        ? p.pipelineHandle(RenderTechniqueId::ForwardRaster, kBimSectionCapHatchOitPipeline)
        : VK_NULL_HANDLE;
    const bool sortCap = fillOit != VK_NULL_HANDLE && hatchOit != VK_NULL_HANDLE;
    if (sortCap ? native : display) {
      const auto &cap = bim.sectionClipCapGeometry.scene;
      (void)recordBimSectionClipCapFramePassCommands(
          cmd, {.style = capStyle,
                .geometry = {.sceneDescriptorSet = inputs.bimDescriptorSet,
                             .vertexSlice = cap.vertexSlice,
                             .indexSlice = cap.indexSlice,
                             .indexType = cap.indexType},
                .fillPipeline = sortCap ? fillOit : inputs.capFill,
                .hatchPipeline = sortCap ? hatchOit : inputs.capHatch,
                .wireframeLayout = sortCap ? oitLayout : inputs.wireframeLayout,
                .pushConstants = inputs.wireframePushConstants,
                .debugOverlay = inputs.debugOverlay,
                .oitDescriptorSet = sortCap ? inputs.nativeOitSet : VK_NULL_HANDLE});
    }
  }

  const BimPrimitivePassGeometryBinding primitiveGeometry{
      .sceneDescriptorSet = inputs.bimDescriptorSet,
      .vertexSlice = bim.scene.vertexSlice,
      .indexSlice = bim.scene.indexSlice,
      .indexType = bim.scene.indexType};
  const auto recordPrimitive = [&](BimPrimitiveFramePassRecordInputs primitive,
                                   const char *oitPipelineName) {
    primitive.geometry = primitiveGeometry;
    primitive.wireframeLayout = inputs.wireframeLayout;
    primitive.pushConstants = inputs.wireframePushConstants;
    primitive.debugOverlay = inputs.debugOverlay;
    primitive.bimManager = p.services.bimManager;
    if (native && oitLayout != VK_NULL_HANDLE &&
        hasDepthTestedNativePrimitive(primitive.style.enabled,
                                      primitive.style.depthTest, primitive.nativeDraws)) {
      const VkPipeline oitPipeline =
          p.pipelineHandle(RenderTechniqueId::ForwardRaster, oitPipelineName);
      if (oitPipeline != VK_NULL_HANDLE) {
        primitive.pipelines.depth = oitPipeline;
        primitive.wireframeLayout = oitLayout;
        primitive.oitDescriptorSet = inputs.nativeOitSet;
      }
    }
    (void)recordBimPrimitiveFramePassCommands(cmd, primitive);
  };

  const auto bimGeometry = overlayGeometry(inputs.bimDescriptorSet, bim.scene);
  const auto markerGeometry = overlayGeometry(inputs.bimDescriptorSet, bim.coordinationMarkerScene);
  const auto sortedOverlayPipeline = [&](const BimLightingOverlayFrameStyleState &style,
                                         const BimLightingOverlayGeometryBinding &geometry,
                                         const std::vector<DrawCommand> *commands,
                                         const char *pipelineName) {
    return sortDisplayOverlays && style.enabled && style.depthTest && hasDrawCommands(commands) &&
        geometry.descriptorSet != VK_NULL_HANDLE && geometry.vertexSlice.buffer != VK_NULL_HANDLE &&
        geometry.indexSlice.buffer != VK_NULL_HANDLE
        ? p.pipelineHandle(RenderTechniqueId::ForwardRaster, pipelineName) : VK_NULL_HANDLE;
  };
  const auto recordSortedOverlay = [&](VkPipeline pipeline,
                                       const BimLightingOverlayFrameStyleState &style,
                                       const BimLightingOverlayGeometryBinding &geometry,
                                       const std::vector<DrawCommand> *commands,
                                       bool useRasterLineWidth,
                                       const WireframePushConstants *pushConstants = nullptr) {
    if (!native || pipeline == VK_NULL_HANDLE)
      return;
    // Insert display fragments at their own depth. In Deferred, the same
    // eligibility check also suppresses drawing them after reflections.
    const BimPrimitivePassPlan plan{
        .active = true,
        .opacity = sanitizeBimLightingOverlayOpacity(style.opacity),
        .primitiveSize = sanitizeBimLightingOverlayLineWidth(style.lineWidth),
        .cpuDrawSources = {commands}};
    (void)recordBimPrimitivePassCommands(cmd, {
        .plan = &plan,
        .geometryReady = true,
        .depthPipeline = pipeline,
        .wireframeLayout = oitLayout,
        .sceneDescriptorSet = geometry.descriptorSet,
        .vertexSlice = geometry.vertexSlice,
        .indexSlice = geometry.indexSlice,
        .indexType = geometry.indexType,
        .pushConstants = pushConstants ? pushConstants : inputs.wireframePushConstants,
        .debugOverlay = inputs.debugOverlay,
        .color = style.color,
        .recordLineWidth = useRasterLineWidth,
        .wideLinesSupported = lineListWideLinesSupported,
        .oitDescriptorSet = inputs.nativeOitSet});
  };

  BimLightingOverlayFrameStyleState floorPlanStyle{
      .enabled = floorPlan.enabled, .depthTest = floorPlan.depthTest, .color = floorPlan.color,
      .opacity = floorPlan.opacity, .lineWidth = floorPlan.lineWidth};
  const auto floorPlanOit = sortedOverlayPipeline(
      floorPlanStyle, bimGeometry, bim.floorPlanDrawCommands, kBimFloorPlanOitPipeline);
  recordSortedOverlay(floorPlanOit, floorPlanStyle, bimGeometry, bim.floorPlanDrawCommands, true);

  BimLightingOverlayFrameStyleState issueMarkerStyle{
      .enabled = markers.issueMarkersEnabled, .depthTest = markers.depthTest, .color = markers.issueColor,
      .opacity = markers.issueOpacity, .lineWidth = markers.issueLineWidth};
  const auto issueMarkerOit = sortedOverlayPipeline(
      issueMarkerStyle, markerGeometry, bim.coordinationIssueMarkerDrawCommands, kBimIssueMarkerOitPipeline);
  recordSortedOverlay(issueMarkerOit, issueMarkerStyle, markerGeometry,
                      bim.coordinationIssueMarkerDrawCommands, false);

  const auto *bimHoverDraws = bim.draws.hoveredDrawCommands;
  const BimLightingOverlayFrameStyleState bimHoverStyle{
      .enabled = true, .color = kHoverColor, .opacity = kMeshHoverOpacity,
      .lineWidth = rasterBimLightingOverlayLineWidth(kMeshHoverLineWidth,
                                                     p.debug.wireframeWideLinesSupported)};
  const auto bimHoverOit = sortedOverlayPipeline(
      bimHoverStyle, bimGeometry, bimHoverDraws, kBimIssueMarkerOitPipeline);
  recordSortedOverlay(bimHoverOit, bimHoverStyle, bimGeometry, bimHoverDraws, false);

  const auto &points = bim.primitivePasses.pointCloud;
  const auto nativePointDraws = primitiveDrawLists(bim.nativePointDraws);
  // The all-in-one route already gives native draws precedence over placeholders.
  if (native || !hasBimPrimitivePassDrawCommands(nativePointDraws)) {
    recordPrimitive(
        {.style = {.kind = BimPrimitivePassKind::Points,
                   .enabled = points.enabled,
                   .depthTest = points.depthTest,
                   .placeholderRangePreviewEnabled =
                       display && points.placeholderRangePreviewEnabled,
                   .nativeDrawsUseGpuVisibility = bim.nativePointDrawsUseGpuVisibility,
                   .opacity = points.opacity,
                   .primitiveSize = points.pointSize,
                   .color = points.color},
         .placeholderDraws = primitiveDrawLists(bim.pointDraws),
         .nativeDraws = nativePointDraws,
         .pipelines = {.depth = inputs.pipelines.bimPointCloudDepth,
                       .noDepth = inputs.pointCloudNoDepth}},
        kBimNativePointOitPipeline);
  }

  const auto &curves = bim.primitivePasses.curves;
  const auto nativeCurveDraws = primitiveDrawLists(bim.nativeCurveDraws);
  if (native || !hasBimPrimitivePassDrawCommands(nativeCurveDraws)) {
    recordPrimitive(
        {.style = {.kind = BimPrimitivePassKind::Curves,
                   .enabled = curves.enabled,
                   .depthTest = curves.depthTest,
                   .placeholderRangePreviewEnabled =
                       display && curves.placeholderRangePreviewEnabled,
                   .nativeDrawsUseGpuVisibility = bim.nativeCurveDrawsUseGpuVisibility,
                   .opacity = curves.opacity,
                   .primitiveSize = curves.lineWidth,
                   .color = curves.color,
                   .recordLineWidth = true,
                   .wideLinesSupported = lineListWideLinesSupported},
         .placeholderDraws = primitiveDrawLists(bim.curveDraws),
         .nativeDraws = nativeCurveDraws,
         .pipelines = {.depth = inputs.pipelines.bimCurveDepth,
                       .noDepth = inputs.curveNoDepth}},
        kBimNativeCurveOitPipeline);
  }

  if (native) {
    recordPrimitive(
        {.style = {.kind = BimPrimitivePassKind::CurveStrokes,
                   .enabled = curves.enabled,
                   .depthTest = curves.depthTest,
                   .nativeDrawsUseGpuVisibility = bim.nativeCurveStrokeDrawsUseGpuVisibility,
                   .opacity = curves.opacity,
                   .color = curves.color},
         .nativeDraws = primitiveDrawLists(bim.nativeCurveStrokeDraws),
         .pipelines = {.depth = inputs.pipelines.bimCurveStrokeDepth,
                       .noDepth = inputs.curveStrokeNoDepth}},
        kBimNativeCurveStrokeOitPipeline);
  }

  if (!display && !sortDisplayOverlays) {
    return;
  }

  const auto &pointStyle = bim.pointCurveStyle.points;
  const auto &curveStyle = bim.pointCurveStyle.curves;
  const auto &plane = bim.sectionPlaneVisual;
  const auto planeGeometry = overlayGeometry(inputs.bimDescriptorSet,
                                             bim.sectionPlaneVisualScene);
  BimLightingOverlayFrameStyleState planeStyle{
      .enabled = plane.enabled, .depthTest = plane.depthTest, .color = plane.color,
      .opacity = plane.opacity, .lineWidth = plane.lineWidth};
  const VkPipeline planeOit = sortedOverlayPipeline(
      planeStyle, planeGeometry, bim.sectionPlaneVisualDrawCommands, kBimDepthWireframeOitPipeline);
  if (planeOit != VK_NULL_HANDLE) {
    WireframePushConstants planePush = *inputs.wireframePushConstants;
    planePush.sectionPlaneEnabled = 0u;
    recordSortedOverlay(planeOit, planeStyle, planeGeometry, bim.sectionPlaneVisualDrawCommands,
                        p.debug.wireframeRasterModeSupported, &planePush);
    planeStyle.enabled = false;
  }
  const VkClearAttachment selectionClear{
      .aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
      .clearValue = {.depthStencil = {0.0f, 0u}}};
  const VkClearRect selectionRect{
      .rect = {.offset = {0, 0}, .extent = inputs.extent},
      .baseArrayLayer = 0u,
      .layerCount = 1u};
  floorPlanStyle.enabled = floorPlanStyle.enabled && floorPlanOit == VK_NULL_HANDLE &&
      !bimFloorPlanUsesPresentationOverlay(p.runtime.activeTechnique, displayMode, floorPlan.depthTest);
  issueMarkerStyle.enabled = issueMarkerStyle.enabled && issueMarkerOit == VK_NULL_HANDLE;
  const BimLightingOverlayFrameRecordInputs overlay{
    .bimGeometryReady = hasBimPrimitiveFramePassGeometry(primitiveGeometry),
    .framebufferExtent = inputs.extent,
    .points = {.enabled = pointStyle.enabled,
               .depthTest = pointStyle.depthTest,
               .color = pointStyle.color,
               .opacity = pointStyle.opacity,
               .lineWidth = pointStyle.pointSize},
    .curves = {.enabled = curveStyle.enabled,
               .depthTest = curveStyle.depthTest,
               .color = curveStyle.color,
               .opacity = curveStyle.opacity,
               .lineWidth = curveStyle.lineWidth},
    .floorPlan = floorPlanStyle,
    .coordinationIssueMarkers = issueMarkerStyle,
    .coordinationClashMarkers = {.enabled = markers.clashMarkersEnabled && !presentsBimXrayClash(p),
                                 .depthTest = markers.depthTest && markers.clashDepthTest,
                                 .color = markers.clashColor,
                                 .opacity = markers.clashOpacity,
                                 .lineWidth = markers.clashLineWidth},
    .sectionPlaneVisual = planeStyle,
    .draws = {.points = overlayDrawLists(bim.pointDraws),
              .curves = overlayDrawLists(bim.curveDraws),
              .floorPlan = bim.floorPlanDrawCommands,
              .sceneHover = p.draws.hoveredDrawCommands,
              .bimHover = bimHoverOit == VK_NULL_HANDLE ? bimHoverDraws : nullptr,
              .sceneSelection = p.draws.selectedDrawCommands,
              .bimSelection = presentsBimMeshSelection(p) ? nullptr : bim.draws.selectedDrawCommands,
              .nativePointHover = bim.nativePointDraws.hoveredDrawCommands,
              .nativeCurveHover = bim.nativeCurveDraws.hoveredDrawCommands,
              .nativePointSelection = bim.nativePointDraws.selectedDrawCommands,
              .nativeCurveSelection = bim.nativeCurveDraws.selectedDrawCommands,
              .nativeCurveStrokeHover =
                  bim.nativeCurveStrokeDraws.hoveredDrawCommands,
              .nativeCurveStrokeSelection =
                  bim.nativeCurveStrokeDraws.selectedDrawCommands,
              .coordinationIssueMarkers = bim.coordinationIssueMarkerDrawCommands,
              .coordinationClashMarkers = bim.coordinationClashMarkerDrawCommands,
              .sectionPlaneVisual = bim.sectionPlaneVisualDrawCommands},
    .nativePointSize = points.pointSize,
    .nativeCurveLineWidth = curves.lineWidth,
    .coordinationMarkerGeometryReady =
        markerGeometry.descriptorSet != VK_NULL_HANDLE &&
        markerGeometry.vertexSlice.buffer != VK_NULL_HANDLE &&
        markerGeometry.indexSlice.buffer != VK_NULL_HANDLE,
    .sectionPlaneVisualGeometryReady =
        planeGeometry.descriptorSet != VK_NULL_HANDLE &&
        planeGeometry.vertexSlice.buffer != VK_NULL_HANDLE &&
        planeGeometry.indexSlice.buffer != VK_NULL_HANDLE,
    .pipelines = inputs.pipelines,
    .wireframeLayout = inputs.wireframeLayout,
    .scene = overlayGeometry(inputs.sceneDescriptorSet, p.scene),
    .bim = bimGeometry,
    .coordinationMarkers = markerGeometry,
    .sectionPlaneVisualGeometry = planeGeometry,
    .selectionStencilClearAttachment = selectionClear,
    .selectionStencilClearRect = selectionRect,
    .wireframePushConstants = inputs.wireframePushConstants,
    .debugOverlay = inputs.debugOverlay,
    .wireframeRasterModeSupported = p.debug.wireframeRasterModeSupported,
    .wireframeWideLinesSupported = p.debug.wireframeWideLinesSupported,
    .lineListWideLinesSupported = lineListWideLinesSupported};
  auto plan = buildBimLightingOverlayFramePlan(overlay);
  if (sortDisplayOverlays) {
    const auto sortHighlight = [&](BimLightingOverlayDrawPlan &highlight,
                                   const char *pipelineName) {
      const BimLightingOverlayFrameStyleState style{
          .enabled = highlight.active, .color = highlight.color,
          .opacity = highlight.opacity, .lineWidth = highlight.drawLineWidth};
      const auto pipeline = sortedOverlayPipeline(style, bimGeometry,
                                                  highlight.commands, pipelineName);
      recordSortedOverlay(pipeline, style, bimGeometry, highlight.commands,
                          highlight.rasterLineWidthApplies);
      if (pipeline != VK_NULL_HANDLE)
        highlight.active = false;
    };
    sortHighlight(plan.nativePointHover, kBimPointHighlightOitPipeline);
    sortHighlight(plan.nativeCurveHover, kBimFloorPlanOitPipeline);
    sortHighlight(plan.nativeCurveStrokeHover, kBimCurveStrokeHighlightOitPipeline);
    sortHighlight(plan.nativePointSelection, kBimPointSelectionOitPipeline);
    sortHighlight(plan.nativeCurveSelection, kBimCurveSelectionOitPipeline);
    sortHighlight(plan.nativeCurveStrokeSelection, kBimCurveStrokeSelectionOitPipeline);
  }
  if (display)
    (void)recordBimLightingOverlayFramePlanCommands(cmd, overlay, plan);
}

void recordBimPresentationFloorPlan(VkCommandBuffer cmd, const FrameRecordParams &p,
                                    VkExtent2D extent, const DebugOverlayRenderer *debugOverlay,
                                    bool resolvedDepthAvailable) {
  const auto &style = p.bim.floorPlan;
  const auto technique = p.runtime.activeTechnique;
  const auto mode = static_cast<ui::GBufferViewMode>(p.debug.displayMode);
  if (p.screenshot.sceneLinear || !style.enabled || !hasDrawCommands(p.bim.floorPlanDrawCommands) ||
      !bimFloorPlanUsesPresentationOverlay(technique, mode, style.depthTest) ||
      !p.pushConstants.wireframe || !debugOverlay)
    return;

  const bool reference = isReferenceRayTechnique(technique);
  const auto layout = forwardRasterPipelineLayout(p, ForwardRasterPipelineLayoutId::BimFloorPlanDisplay);
  const auto bimSet = p.descriptorSet(reference ? RenderTechniqueId::ForwardRaster : technique,
                                      "bim-scene-descriptor-set");
  const auto geometry = overlayGeometry(bimSet, p.bim.scene);
  if (layout == VK_NULL_HANDLE || bimSet == VK_NULL_HANDLE ||
      geometry.vertexSlice.buffer == VK_NULL_HANDLE || geometry.indexSlice.buffer == VK_NULL_HANDLE)
    return;

  WireframePushConstants pushConstants = *p.pushConstants.wireframe;
  if (style.depthTest) {
    const auto frameLighting = forwardRasterDescriptorSet(p, ForwardRasterDescriptorSetId::FrameLighting);
    const auto *depth = forwardRasterImageBinding(p, ForwardRasterImageId::DepthSamplingView);
    if (!resolvedDepthAvailable || frameLighting == VK_NULL_HANDLE || depth == nullptr ||
        depth->view == VK_NULL_HANDLE || depth->extent.width != extent.width || depth->extent.height != extent.height)
      return;

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &frameLighting, 0, nullptr);
    if (depth->format == VK_FORMAT_D24_UNORM_S8_UINT)
      pushConstants.floorPlanDepthTolerance = 2.0f / 16777215.0f;
  }
  const bool overview = !reference && mode == ui::GBufferViewMode::Overview;
  if (overview && !recordOverviewLitTileViewport(cmd, extent))
    return;
  if (!reference)
    pushConstants.useUnjitteredCamera = 1u;
  if (p.pushConstants.bindless) {
    pushConstants.sectionPlaneEnabled = p.pushConstants.bindless->sectionPlaneEnabled;
    pushConstants.sectionPlane = p.pushConstants.bindless->sectionPlane;
  }

  const glm::vec3 color = p.postProcess.encodeSrgbOutput
      ? glm::convertLinearToSRGB(style.color, 2.4f) : style.color;
  (void)recordBimLightingOverlayFrameCommands(cmd, {
      .bimGeometryReady = true,
      .framebufferExtent = extent,
      .floorPlan = {.enabled = true,
                    .depthTest = style.depthTest,
                    .color = color,
                    .opacity = style.opacity,
                    .lineWidth = overview ? overviewLitTileLineWidth(style.lineWidth) : style.lineWidth},
      .draws = {.floorPlan = p.bim.floorPlanDrawCommands},
      .pipelines = {.bimFloorPlanDepth = forwardRasterPipelineHandle(p, ForwardRasterPipelineId::BimFloorPlanDisplayDepth),
                    .bimFloorPlanNoDepth = forwardRasterPipelineHandle(p, ForwardRasterPipelineId::BimFloorPlanDisplayNoDepth)},
      .wireframeLayout = layout,
      .bim = geometry,
      .wireframePushConstants = &pushConstants,
      .debugOverlay = debugOverlay,
      .lineListWideLinesSupported = p.debug.lineListWideLinesSupported});
  if (overview)
    recordSceneViewportAndScissor(cmd, extent);
}

void recordBimXrayClashOverlay(VkCommandBuffer cmd, const FrameRecordParams &p,
                               VkExtent2D extent, const DebugOverlayRenderer *debugOverlay) {
  if (!presentsBimXrayClash(p) || !p.pushConstants.wireframe || !debugOverlay)
    return;

  const auto technique = p.runtime.activeTechnique;
  const auto layout = p.pipelineLayout(technique, "wireframe");
  const auto pipeline = p.pipelineHandle(RenderTechniqueId::ForwardRaster, kBimDisplayWireframePipeline);
  const auto descriptor = p.descriptorSet(technique, "bim-scene-descriptor-set");
  const auto geometry = overlayGeometry(descriptor, p.bim.coordinationMarkerScene);
  if (layout == VK_NULL_HANDLE || pipeline == VK_NULL_HANDLE || descriptor == VK_NULL_HANDLE ||
      geometry.vertexSlice.buffer == VK_NULL_HANDLE || geometry.indexSlice.buffer == VK_NULL_HANDLE)
    return;

  const bool overview = p.debug.displayMode == static_cast<uint32_t>(ui::GBufferViewMode::Overview);
  if (overview && !recordOverviewLitTileViewport(cmd, extent))
    return;

  const auto &markers = p.bim.coordinationMarkers;
  WireframePushConstants pushConstants = *p.pushConstants.wireframe;
  pushConstants.useUnjitteredCamera = 1u;
  if (p.pushConstants.bindless) {
    pushConstants.sectionPlaneEnabled = p.pushConstants.bindless->sectionPlaneEnabled;
    pushConstants.sectionPlane = p.pushConstants.bindless->sectionPlane;
  }
  const glm::vec3 color = p.postProcess.encodeSrgbOutput
      ? glm::convertLinearToSRGB(markers.clashColor) : markers.clashColor;
  (void)recordBimLightingOverlayFrameCommands(cmd, {
      .bimGeometryReady = true,
      .framebufferExtent = extent,
      .coordinationClashMarkers = {.enabled = true,
                                   .depthTest = false,
                                   .color = color,
                                   .opacity = markers.clashOpacity,
                                   .lineWidth = overview ? overviewLitTileLineWidth(markers.clashLineWidth)
                                                         : markers.clashLineWidth},
      .draws = {.coordinationClashMarkers = p.bim.coordinationClashMarkerDrawCommands},
      .coordinationMarkerGeometryReady = true,
      .pipelines = {.wireframeNoDepth = pipeline},
      .wireframeLayout = layout,
      .coordinationMarkers = geometry,
      .wireframePushConstants = &pushConstants,
      .debugOverlay = debugOverlay,
      .wireframeRasterModeSupported = p.debug.wireframeRasterModeSupported,
      .wireframeWideLinesSupported = p.debug.wireframeWideLinesSupported});
  if (overview)
    recordSceneViewportAndScissor(cmd, extent);
}

bool presentsBimTechnicalLines(const FrameRecordParams &p) {
  const auto technique = p.runtime.activeTechnique;
  const auto mode = static_cast<ui::GBufferViewMode>(p.debug.displayMode);
  const auto &style = p.bim.technicalElevation;
  return (technique == RenderTechniqueId::ForwardRaster || technique == RenderTechniqueId::DeferredRaster) &&
         (mode == ui::GBufferViewMode::Lit || mode == ui::GBufferViewMode::Overview) &&
         !p.screenshot.sceneLinear && style.enabled && !style.hiddenLineOverlay && !style.depthTestLines &&
         p.pipelineHandle(RenderTechniqueId::ForwardRaster, kBimDisplayWireframePipeline) != VK_NULL_HANDLE;
}

bool recordBimTechnicalOitLines(VkCommandBuffer cmd, const FrameRecordParams &p,
                               const DeferredDebugOverlayRecordInputs &inputs,
                               VkDescriptorSet oitSet) {
  const auto &style = p.bim.technicalElevation;
  const auto mode = static_cast<ui::GBufferViewMode>(p.debug.displayMode);
  if (oitSet == VK_NULL_HANDLE || p.screenshot.sceneLinear ||
      !style.enabled || style.hiddenLineOverlay || !style.depthTestLines ||
      (mode != ui::GBufferViewMode::Lit && mode != ui::GBufferViewMode::Overview) ||
      (p.runtime.activeTechnique == RenderTechniqueId::DeferredRaster &&
       screenReflectionDebugView(p.debug.screenReflections) != 0u)) {
    return false;
  }

  auto sorted = inputs;
  sorted.wireframeLayout = p.pipelineLayout(RenderTechniqueId::ForwardRaster, kBimNativeOitLayout);
  sorted.pipelines.wireframeDepth = p.pipelineHandle(RenderTechniqueId::ForwardRaster,
                                                     kBimDepthWireframeOitPipeline);
  sorted.pipelines.wireframeDepthFrontCull = p.pipelineHandle(RenderTechniqueId::ForwardRaster,
                                                              kBimDepthWireframeOitFrontCullPipeline);
  if (sorted.wireframeLayout == VK_NULL_HANDLE || sorted.pipelines.wireframeDepth == VK_NULL_HANDLE ||
      sorted.pipelines.wireframeDepthFrontCull == VK_NULL_HANDLE) {
    return false;
  }
  return recordDeferredDebugOverlayWireframeOverlayCommands(cmd, sorted, oitSet);
}

void recordBimTechnicalPresentationLines(VkCommandBuffer cmd, const FrameRecordParams &p,
                                        VkExtent2D extent, const DebugOverlayRenderer *debugOverlay) {
  if (!presentsBimTechnicalLines(p) || !debugOverlay || !p.pushConstants.wireframe)
    return;

  const auto technique = p.runtime.activeTechnique;
  const auto layout = p.pipelineLayout(technique, "wireframe");
  if (!layout)
    return;
  const DeferredDebugOverlayPipelineHandles pipelines{
      .wireframeNoDepth = p.pipelineHandle(RenderTechniqueId::ForwardRaster, kBimDisplayWireframePipeline),
      .wireframeNoDepthFrontCull = p.pipelineHandle(RenderTechniqueId::ForwardRaster, kBimDisplayWireframeFrontCullPipeline)};
  const auto &style = p.bim.technicalElevation;
  const auto lighting = buildDeferredLightingFrameState({
      .wireframeSupported = true,
      .bimTechnicalElevation = {.enabled = true, .depthTestLines = false, .lineColor = style.lineColor,
                                .lineWidth = style.lineWidth, .overlayIntensity = style.overlayIntensity},
      .pipelines = {.wireframeNoDepth = true}});
  const auto sceneSet = p.descriptorSet(technique, "scene-descriptor-set");
  const auto bimSet = p.descriptorSet(technique, "bim-scene-descriptor-set");
  const bool sceneReady = sceneSet && p.scene.vertexSlice.buffer && p.scene.indexSlice.buffer;
  const bool bimReady = bimSet && p.bim.scene.vertexSlice.buffer && p.bim.scene.indexSlice.buffer;
  const bool overview = p.debug.displayMode == static_cast<uint32_t>(ui::GBufferViewMode::Overview);
  const float width = overview ? overviewLitTileLineWidth(lighting.wireframeSettings.lineWidth)
                               : lighting.wireframeSettings.lineWidth;
  const auto plan = buildDeferredDebugOverlayPlan({
      .frameState = {.wireframeOverlayMode = true, .wireframeDepthTest = false, .wireframeLineWidth = width},
      .pipelines = {.wireframeNoDepth = true, .wireframeNoDepthFrontCull = pipelines.wireframeNoDepthFrontCull != VK_NULL_HANDLE},
      .wireframePushConstantsReady = true,
      .sources = {{{.source = DeferredDebugOverlaySource::Scene, .geometryReady = sceneReady,
                     .draws = debugOverlayDrawLists(p.draws)},
                    {.source = DeferredDebugOverlaySource::BimMesh, .geometryReady = bimReady,
                     .draws = debugOverlayDrawLists(p.bim.draws)},
                    {.source = DeferredDebugOverlaySource::BimPointPlaceholders, .geometryReady = bimReady,
                     .draws = debugOverlayDrawLists(p.bim.pointDraws)},
                    {.source = DeferredDebugOverlaySource::BimCurvePlaceholders, .geometryReady = bimReady,
                     .draws = debugOverlayDrawLists(p.bim.curveDraws)}}},
      .sourceCount = kDeferredDebugOverlaySourceCapacity});
  if (plan.wireframeOverlaySourceCount == 0 || (overview && !recordOverviewLitTileViewport(cmd, extent)))
    return;

  WireframePushConstants pushConstants = *p.pushConstants.wireframe;
  pushConstants.useUnjitteredCamera = 1u;
  if (p.pushConstants.bindless) {
    pushConstants.sectionPlaneEnabled = p.pushConstants.bindless->sectionPlaneEnabled;
    pushConstants.sectionPlane = p.pushConstants.bindless->sectionPlane;
  }
  const auto &color = lighting.wireframeSettings.color;
  (void)recordDeferredDebugOverlayWireframeOverlayCommands(cmd, {
      .plan = &plan, .pipelines = pipelines, .wireframeLayout = layout,
      .scene = {.descriptorSet = sceneSet, .vertexSlice = p.scene.vertexSlice,
                .indexSlice = p.scene.indexSlice, .indexType = p.scene.indexType},
      .bim = {.descriptorSet = bimSet, .vertexSlice = p.bim.scene.vertexSlice,
              .indexSlice = p.bim.scene.indexSlice, .indexType = p.bim.scene.indexType},
      .wireframePushConstants = &pushConstants,
      .wireframeColor = p.postProcess.encodeSrgbOutput ? glm::convertLinearToSRGB(color) : color,
      .wireframeIntensity = lighting.wireframeIntensity, .wireframeLineWidth = width,
      .wireframeRasterModeSupported = p.debug.wireframeRasterModeSupported,
      .wireframeWideLinesSupported = p.debug.wireframeWideLinesSupported, .debugOverlay = debugOverlay});
  if (overview)
    recordSceneViewportAndScissor(cmd, extent);
}

bool recordBimSelectionStrokeMask(VkCommandBuffer cmd, const FrameRecordParams &p,
                                  VkExtent2D extent, const DebugOverlayRenderer *debugOverlay) {
  if (!presentsBimMeshSelection(p) || !p.pushConstants.wireframe || !debugOverlay)
    return false;

  const auto technique = p.runtime.activeTechnique;
  const auto layout = p.pipelineLayout(technique, "wireframe");
  const auto clearSilhouette = p.pipelineHandle(RenderTechniqueId::ForwardRaster, kBimSelectionClearSilhouettePipeline);
  const auto expandSilhouette = p.pipelineHandle(RenderTechniqueId::ForwardRaster, kBimSelectionExpandSilhouettePipeline);
  const auto *mask = p.imageBinding(RenderTechniqueId::ForwardRaster, kBimSelectionStrokeImage);
  const auto geometry = overlayGeometry(p.descriptorSet(technique, "bim-scene-descriptor-set"), p.bim.scene);
  if (!mask || !mask->image || !mask->view || !layout || !clearSilhouette || !expandSilhouette ||
      mask->extent.width != extent.width || mask->extent.height != extent.height ||
      !bindGeometryIfReady(cmd, geometry, layout))
    return false;

  const bool overview = p.debug.displayMode == static_cast<uint32_t>(ui::GBufferViewMode::Overview);
  if (overview) {
    if (!recordOverviewLitTileViewport(cmd, extent))
      return false;
  } else {
    recordSceneViewportAndScissor(cmd, extent);
  }

  // The previous use of this swapchain image's mask is retired with the frame.
  VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
      .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = mask->image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
  const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
  vkCmdPipelineBarrier2(cmd, &dependency);

  const VkRenderingAttachmentInfo attachment{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = mask->view, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
  const VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea = {{0, 0}, extent}, .layerCount = 1,
      .colorAttachmentCount = 1, .pColorAttachments = &attachment};
  vkCmdBeginRendering(cmd, &rendering);
  WireframePushConstants pushConstants = *p.pushConstants.wireframe;
  pushConstants.useUnjitteredCamera = 1u;
  pushConstants.useObjectColor = 0u;
  if (p.pushConstants.bindless) {
    pushConstants.sectionPlaneEnabled = p.pushConstants.bindless->sectionPlaneEnabled;
    pushConstants.sectionPlane = p.pushConstants.bindless->sectionPlane;
  }
  // Use the full display extent so Overview scales the stroke with the scene tile.
  pushConstants.padding7 = static_cast<float>(extent.width);
  pushConstants.padding8 = static_cast<float>(extent.height);
  const auto &commands = *p.bim.draws.selectedDrawCommands;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, expandSilhouette);
  debugOverlay->drawSelectionStroke(cmd, layout, commands, glm::vec3(1.0f), pushConstants);
  // Erase the entire original union after all expanded triangles are drawn.
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, clearSilhouette);
  debugOverlay->drawWireframe(cmd, layout, commands, glm::vec3(0.0f), 1.0f, 1.0f, pushConstants);
  vkCmdEndRendering(cmd);

  barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  vkCmdPipelineBarrier2(cmd, &dependency);
  return true;
}

void recordBimSelectionDisplay(VkCommandBuffer cmd, const FrameRecordParams &p,
                               VkDescriptorSet postProcessSet) {
  const auto pipeline = p.pipelineHandle(RenderTechniqueId::ForwardRaster, kBimSelectionDisplayPipeline);
  const auto layout = p.pipelineLayout(p.runtime.activeTechnique, "post-process");
  if (!pipeline || !layout || !postProcessSet)
    return;
  const glm::vec4 color(p.postProcess.encodeSrgbOutput ? glm::convertLinearToSRGB(kSelectionColor) : kSelectionColor, 1.0f);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &postProcessSet, 0, nullptr);
  vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(color), &color);
  vkCmdDraw(cmd, 3, 1, 0, 0);
}

} // namespace container::renderer
