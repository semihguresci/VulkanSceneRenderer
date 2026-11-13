#include "Container/renderer/bim/BimLightingOverlayRecorder.h"

#include "Container/renderer/bim/BimPrimitivePassRecorder.h"
#include "Container/renderer/bim/BimSectionClipCapPassRecorder.h"
#include "Container/renderer/core/FrameRecorder.h"

#include <algorithm>

namespace container::renderer {

namespace {

[[nodiscard]] bool hasDrawCommands(const std::vector<DrawCommand> *commands) {
  return commands != nullptr && !commands->empty();
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

  recorded |= drawSelectionOutline(cmd, inputs.scene,
                                   plan.sceneSelectionOutline, inputs);
  recorded |=
      drawSelectionOutline(cmd, inputs.bim, plan.bimSelectionOutline, inputs);
  recorded |= drawOverlayPlan(cmd, inputs.bim, plan.nativePointSelection, false,
                              inputs, pushConstants);
  recorded |= drawOverlayPlan(cmd, inputs.bim, plan.nativeCurveSelection, false,
                              inputs, pushConstants);
  return recorded;
}

bool recordBimLightingOverlayFrameCommands(
    VkCommandBuffer cmd, const BimLightingOverlayFrameRecordInputs &inputs) {
  if (cmd == VK_NULL_HANDLE) {
    return false;
  }

  const BimLightingOverlayPlan plan = buildBimLightingOverlayFramePlan(inputs);
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

void recordBimRasterOverlays(VkCommandBuffer cmd, const FrameRecordParams &p,
                             const BimRasterOverlayRecordInputs &inputs) {
  const auto &bim = p.bim;
  const bool display = !p.screenshot.sceneLinear;
  const bool lineListWideLinesSupported = p.debug.lineListWideLinesSupported;
  if (display) {
    const auto &cap = bim.sectionClipCapGeometry.scene;
    (void)recordBimSectionClipCapFramePassCommands(
        cmd, {.style = sectionClipCapStyle(bim, lineListWideLinesSupported),
              .geometry = {.sceneDescriptorSet = inputs.bimDescriptorSet,
                           .vertexSlice = cap.vertexSlice,
                           .indexSlice = cap.indexSlice,
                           .indexType = cap.indexType},
              .fillPipeline = inputs.capFill,
              .hatchPipeline = inputs.capHatch,
              .wireframeLayout = inputs.wireframeLayout,
              .pushConstants = inputs.wireframePushConstants,
              .debugOverlay = inputs.debugOverlay});
  }

  const BimPrimitivePassGeometryBinding primitiveGeometry{
      .sceneDescriptorSet = inputs.bimDescriptorSet,
      .vertexSlice = bim.scene.vertexSlice,
      .indexSlice = bim.scene.indexSlice,
      .indexType = bim.scene.indexType};
  const auto &points = bim.primitivePasses.pointCloud;
  (void)recordBimPrimitiveFramePassCommands(
      cmd, {.style = {.kind = BimPrimitivePassKind::Points,
                      .enabled = points.enabled,
                      .depthTest = points.depthTest,
                      .placeholderRangePreviewEnabled =
                          display && points.placeholderRangePreviewEnabled,
                      .nativeDrawsUseGpuVisibility = bim.nativePointDrawsUseGpuVisibility,
                      .opacity = points.opacity,
                      .primitiveSize = points.pointSize,
                      .color = points.color},
            .placeholderDraws = primitiveDrawLists(bim.pointDraws),
            .nativeDraws = primitiveDrawLists(bim.nativePointDraws),
            .geometry = primitiveGeometry,
            .pipelines = {.depth = inputs.pipelines.bimPointCloudDepth,
                          .noDepth = inputs.pointCloudNoDepth},
            .wireframeLayout = inputs.wireframeLayout,
            .pushConstants = inputs.wireframePushConstants,
            .debugOverlay = inputs.debugOverlay,
            .bimManager = p.services.bimManager});

  const auto &curves = bim.primitivePasses.curves;
  (void)recordBimPrimitiveFramePassCommands(
      cmd, {.style = {.kind = BimPrimitivePassKind::Curves,
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
            .nativeDraws = primitiveDrawLists(bim.nativeCurveDraws),
            .geometry = primitiveGeometry,
            .pipelines = {.depth = inputs.pipelines.bimCurveDepth,
                          .noDepth = inputs.curveNoDepth},
            .wireframeLayout = inputs.wireframeLayout,
            .pushConstants = inputs.wireframePushConstants,
            .debugOverlay = inputs.debugOverlay,
            .bimManager = p.services.bimManager});

  if (!display) {
    return;
  }

  const auto &pointStyle = bim.pointCurveStyle.points;
  const auto &curveStyle = bim.pointCurveStyle.curves;
  const auto &floorPlan = bim.floorPlan;
  const auto &markers = bim.coordinationMarkers;
  const auto &plane = bim.sectionPlaneVisual;
  const auto markerGeometry = overlayGeometry(inputs.bimDescriptorSet,
                                              bim.coordinationMarkerScene);
  const auto planeGeometry = overlayGeometry(inputs.bimDescriptorSet,
                                             bim.sectionPlaneVisualScene);
  const VkClearAttachment selectionClear{
      .aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
      .clearValue = {.depthStencil = {0.0f, 0u}}};
  const VkClearRect selectionRect{
      .rect = {.offset = {0, 0}, .extent = inputs.extent},
      .baseArrayLayer = 0u,
      .layerCount = 1u};
  // Path tracing composites the floor plan in its presentation pass.
  const bool floorPlanEnabled = floorPlan.enabled &&
      p.runtime.activeTechnique != RenderTechniqueId::PathTracing;
  (void)recordBimLightingOverlayFrameCommands(
      cmd, {.bimGeometryReady = hasBimPrimitiveFramePassGeometry(primitiveGeometry),
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
            .floorPlan = {.enabled = floorPlanEnabled,
                          .depthTest = floorPlan.depthTest,
                          .color = floorPlan.color,
                          .opacity = floorPlan.opacity,
                          .lineWidth = floorPlan.lineWidth},
            .coordinationIssueMarkers = {.enabled = markers.issueMarkersEnabled,
                                         .depthTest = markers.depthTest,
                                         .color = markers.issueColor,
                                         .opacity = markers.issueOpacity,
                                         .lineWidth = markers.issueLineWidth},
            .coordinationClashMarkers = {.enabled = markers.clashMarkersEnabled,
                                         .depthTest = markers.depthTest,
                                         .color = markers.clashColor,
                                         .opacity = markers.clashOpacity,
                                         .lineWidth = markers.clashLineWidth},
            .sectionPlaneVisual = {.enabled = plane.enabled,
                                   .depthTest = plane.depthTest,
                                   .color = plane.color,
                                   .opacity = plane.opacity,
                                   .lineWidth = plane.lineWidth},
            .draws = {.points = overlayDrawLists(bim.pointDraws),
                      .curves = overlayDrawLists(bim.curveDraws),
                      .floorPlan = bim.floorPlanDrawCommands,
                      .sceneHover = p.draws.hoveredDrawCommands,
                      .bimHover = bim.draws.hoveredDrawCommands,
                      .sceneSelection = p.draws.selectedDrawCommands,
                      .bimSelection = bim.draws.selectedDrawCommands,
                      .nativePointHover = bim.nativePointDraws.hoveredDrawCommands,
                      .nativeCurveHover = bim.nativeCurveDraws.hoveredDrawCommands,
                      .nativePointSelection = bim.nativePointDraws.selectedDrawCommands,
                      .nativeCurveSelection = bim.nativeCurveDraws.selectedDrawCommands,
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
            .bim = overlayGeometry(inputs.bimDescriptorSet, bim.scene),
            .coordinationMarkers = markerGeometry,
            .sectionPlaneVisualGeometry = planeGeometry,
            .selectionStencilClearAttachment = selectionClear,
            .selectionStencilClearRect = selectionRect,
            .wireframePushConstants = inputs.wireframePushConstants,
            .debugOverlay = inputs.debugOverlay,
            .wireframeRasterModeSupported = p.debug.wireframeRasterModeSupported,
            .wireframeWideLinesSupported = p.debug.wireframeWideLinesSupported,
            .lineListWideLinesSupported = lineListWideLinesSupported});
}

} // namespace container::renderer
