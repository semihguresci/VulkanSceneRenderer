#include "Container/renderer/bim/BimLightingOverlayPlanner.h"

#include "Container/renderer/scene/DrawCommand.h"

#include <algorithm>

namespace container::renderer {

namespace {

[[nodiscard]] bool hasDrawCommands(const std::vector<DrawCommand> *commands) {
  return commands != nullptr && !commands->empty();
}

[[nodiscard]] bool commonWireframeReady(
    const BimLightingOverlayInputs &inputs) {
  return inputs.wireframeLayoutReady && inputs.wireframePushConstantsReady;
}

[[nodiscard]] bool selectedWireframePipelineReady(
    const BimLightingOverlayPipelineReadiness &pipelines, bool depthTest) {
  return depthTest ? pipelines.wireframeDepth : pipelines.wireframeNoDepth;
}

[[nodiscard]] BimLightingOverlayPipeline wireframePipeline(bool depthTest) {
  return depthTest ? BimLightingOverlayPipeline::WireframeDepth
                   : BimLightingOverlayPipeline::WireframeNoDepth;
}

[[nodiscard]] BimLightingOverlayPipeline wireframeFrontCullPipeline(
    bool depthTest) {
  return depthTest ? BimLightingOverlayPipeline::WireframeDepthFrontCull
                   : BimLightingOverlayPipeline::WireframeNoDepthFrontCull;
}

void appendRoute(BimLightingOverlayStylePlan &plan,
                 const std::vector<DrawCommand> *commands,
                 BimLightingOverlayPipeline pipeline, float rasterLineWidth) {
  if (!hasDrawCommands(commands) || plan.routeCount >= plan.routes.size()) {
    return;
  }

  plan.routes[plan.routeCount] = {
      .pipeline = pipeline,
      .commands = commands,
      .rasterLineWidth = rasterLineWidth,
  };
  ++plan.routeCount;
}

[[nodiscard]] BimLightingOverlayStylePlan buildStylePlan(
    BimLightingOverlayKind kind, const BimLightingOverlayStyleInputs &style,
    const BimLightingOverlayInputs &inputs) {
  BimLightingOverlayStylePlan plan{};
  plan.kind = kind;
  plan.color = style.color;
  plan.opacity = sanitizeBimLightingOverlayOpacity(style.opacity);
  plan.drawLineWidth = sanitizeBimLightingOverlayLineWidth(style.lineWidth);
  const float rasterLineWidth =
      rasterBimLightingOverlayLineWidth(plan.drawLineWidth,
                                        inputs.wideLinesSupported);

  if (!style.enabled || !inputs.bimGeometryReady ||
      !commonWireframeReady(inputs) ||
      !selectedWireframePipelineReady(inputs.pipelines, style.depthTest)) {
    return plan;
  }

  const BimLightingOverlayPipeline mainPipeline =
      wireframePipeline(style.depthTest);
  const BimLightingOverlayPipeline frontCullPipeline =
      wireframeFrontCullPipeline(style.depthTest);
  appendRoute(plan, style.draws.opaqueSingleSidedDrawCommands, mainPipeline,
              rasterLineWidth);
  appendRoute(plan, style.draws.transparentSingleSidedDrawCommands,
              mainPipeline, rasterLineWidth);
  appendRoute(plan, style.draws.opaqueWindingFlippedDrawCommands,
              frontCullPipeline, rasterLineWidth);
  appendRoute(plan, style.draws.transparentWindingFlippedDrawCommands,
              frontCullPipeline, rasterLineWidth);
  appendRoute(plan, style.draws.opaqueDoubleSidedDrawCommands, mainPipeline,
              rasterLineWidth);
  appendRoute(plan, style.draws.transparentDoubleSidedDrawCommands,
              mainPipeline, rasterLineWidth);
  plan.active = plan.routeCount > 0u;
  return plan;
}

[[nodiscard]] bool selectedFloorPlanPipelineReady(
    const BimLightingOverlayPipelineReadiness &pipelines, bool depthTest) {
  return depthTest ? pipelines.bimFloorPlanDepth
                   : pipelines.bimFloorPlanNoDepth;
}

[[nodiscard]] BimLightingOverlayPipeline floorPlanPipeline(bool depthTest) {
  return depthTest ? BimLightingOverlayPipeline::BimFloorPlanDepth
                   : BimLightingOverlayPipeline::BimFloorPlanNoDepth;
}

[[nodiscard]] BimLightingOverlayDrawPlan buildFloorPlan(
    const BimLightingOverlayInputs &inputs) {
  BimLightingOverlayDrawPlan plan{};
  plan.kind = BimLightingOverlayKind::FloorPlan;
  plan.pipeline = floorPlanPipeline(inputs.floorPlan.depthTest);
  plan.commands = inputs.floorPlan.commands;
  plan.color = inputs.floorPlan.color;
  plan.opacity = sanitizeBimLightingOverlayOpacity(inputs.floorPlan.opacity);
  plan.drawLineWidth =
      sanitizeBimLightingOverlayLineWidth(inputs.floorPlan.lineWidth);
  plan.rasterLineWidth = rasterBimLightingOverlayLineWidth(
      plan.drawLineWidth, inputs.wideLinesSupported);
  plan.rasterLineWidthApplies = true;
  plan.active = inputs.floorPlan.enabled && inputs.bimGeometryReady &&
                commonWireframeReady(inputs) &&
                selectedFloorPlanPipelineReady(inputs.pipelines,
                                               inputs.floorPlan.depthTest) &&
                hasDrawCommands(plan.commands);
  return plan;
}

[[nodiscard]] BimLightingOverlayDrawPlan buildHover(
    BimLightingOverlayKind kind, const std::vector<DrawCommand> *commands,
    bool geometryReady, const BimLightingOverlayInputs &inputs) {
  BimLightingOverlayDrawPlan plan{};
  plan.kind = kind;
  plan.pipeline = BimLightingOverlayPipeline::WireframeDepth;
  plan.commands = commands;
  plan.color = kHoverColor;
  plan.opacity = kMeshHoverOpacity;
  plan.drawLineWidth = rasterBimLightingOverlayLineWidth(kMeshHoverLineWidth,
                                                        inputs.wideLinesSupported);
  plan.rasterLineWidth = plan.drawLineWidth;
  plan.rasterLineWidthApplies = true;
  plan.active = geometryReady && commonWireframeReady(inputs) &&
                inputs.pipelines.wireframeDepth && hasDrawCommands(commands);
  return plan;
}

[[nodiscard]] float nativePointHoverWidth(float pointSize) {
  return std::max(pointSize + 2.0f, 5.0f);
}

[[nodiscard]] float nativeCurveHoverWidth(float lineWidth) {
  return std::max(lineWidth + 1.0f, 3.0f);
}

[[nodiscard]] float nativePointSelectionWidth(float pointSize) {
  return std::max(pointSize + 3.0f, 6.0f);
}

[[nodiscard]] float nativeCurveSelectionWidth(float lineWidth) {
  return std::max(lineWidth + 2.0f, 4.0f);
}

[[nodiscard]] BimLightingOverlayDrawPlan buildNativePrimitiveHighlight(
    BimLightingOverlayKind kind, BimLightingOverlayPipeline pipeline,
    bool pipelineReady, const std::vector<DrawCommand> *commands,
    const glm::vec3 &color, float opacity, float requestedWidth,
    bool isLinePrimitive, const BimLightingOverlayInputs &inputs) {
  BimLightingOverlayDrawPlan plan{};
  plan.kind = kind;
  plan.pipeline = pipeline;
  plan.commands = commands;
  plan.color = color;
  plan.opacity = sanitizeBimLightingOverlayOpacity(opacity);
  plan.drawLineWidth = sanitizeBimLightingOverlayLineWidth(requestedWidth);
  plan.rasterLineWidth = rasterBimLightingOverlayLineWidth(
      plan.drawLineWidth, inputs.wideLinesSupported);
  plan.rasterLineWidthApplies = isLinePrimitive;
  plan.active = inputs.bimGeometryReady && commonWireframeReady(inputs) &&
                pipelineReady && hasDrawCommands(commands);
  return plan;
}

[[nodiscard]] BimLightingOverlayDrawPlan buildCoordinationMarker(
    BimLightingOverlayKind kind, const BimLightingOverlayStyleInputs &style,
    const std::vector<DrawCommand> *commands,
    const BimLightingOverlayInputs &inputs) {
  BimLightingOverlayDrawPlan plan{};
  plan.kind = kind;
  plan.pipeline = wireframePipeline(style.depthTest);
  plan.commands = commands;
  plan.color = style.color;
  plan.opacity = sanitizeBimLightingOverlayOpacity(style.opacity);
  plan.drawLineWidth = sanitizeBimLightingOverlayLineWidth(style.lineWidth);
  plan.rasterLineWidth = rasterBimLightingOverlayLineWidth(
      plan.drawLineWidth, inputs.wideLinesSupported);
  plan.rasterLineWidthApplies = true;
  plan.active = style.enabled && inputs.coordinationMarkerGeometryReady &&
                commonWireframeReady(inputs) &&
                selectedWireframePipelineReady(inputs.pipelines,
                                               style.depthTest) &&
                hasDrawCommands(commands);
  return plan;
}

[[nodiscard]] BimLightingOverlayDrawPlan buildSectionPlaneVisual(
    const BimLightingOverlayInputs &inputs) {
  BimLightingOverlayDrawPlan plan{};
  plan.kind = BimLightingOverlayKind::SectionPlaneVisual;
  plan.pipeline = wireframePipeline(inputs.sectionPlaneVisual.depthTest);
  plan.commands = inputs.sectionPlaneVisualCommands;
  plan.color = inputs.sectionPlaneVisual.color;
  plan.opacity =
      sanitizeBimLightingOverlayOpacity(inputs.sectionPlaneVisual.opacity);
  plan.drawLineWidth =
      sanitizeBimLightingOverlayLineWidth(inputs.sectionPlaneVisual.lineWidth);
  plan.rasterLineWidth = rasterBimLightingOverlayLineWidth(
      plan.drawLineWidth, inputs.wideLinesSupported);
  plan.rasterLineWidthApplies = true;
  plan.active = inputs.sectionPlaneVisual.enabled &&
                inputs.sectionPlaneVisualGeometryReady &&
                commonWireframeReady(inputs) &&
                selectedWireframePipelineReady(
                    inputs.pipelines, inputs.sectionPlaneVisual.depthTest) &&
                hasDrawCommands(plan.commands);
  return plan;
}

[[nodiscard]] BimLightingSelectionOutlinePlan buildSelectionOutline(
    const std::vector<DrawCommand> *commands, bool geometryReady,
    const BimLightingOverlayInputs &inputs) {
  BimLightingSelectionOutlinePlan plan{};
  plan.commands = commands;
  plan.color = kSelectionColor;
  plan.framebufferWidth = std::max(inputs.framebufferWidth, 1u);
  plan.framebufferHeight = std::max(inputs.framebufferHeight, 1u);
  plan.active = geometryReady && commonWireframeReady(inputs) &&
                inputs.pipelines.selectionMask &&
                inputs.pipelines.selectionOutline && hasDrawCommands(commands);
  return plan;
}

} // namespace

float sanitizeBimLightingOverlayOpacity(float opacity) {
  return std::clamp(opacity, 0.0f, 1.0f);
}

float sanitizeBimLightingOverlayLineWidth(float lineWidth) {
  return std::max(lineWidth, 1.0f);
}

float rasterBimLightingOverlayLineWidth(float lineWidth,
                                        bool wideLinesSupported) {
  return wideLinesSupported ? sanitizeBimLightingOverlayLineWidth(lineWidth)
                            : 1.0f;
}

BimLightingOverlayPlanner::BimLightingOverlayPlanner(
    BimLightingOverlayInputs inputs)
    : inputs_(inputs) {}

BimLightingOverlayPlan BimLightingOverlayPlanner::build() const {
  BimLightingOverlayPlan plan{};
  plan.pointStyle =
      buildStylePlan(BimLightingOverlayKind::PointStyle, inputs_.points,
                     inputs_);
  plan.curveStyle =
      buildStylePlan(BimLightingOverlayKind::CurveStyle, inputs_.curves,
                     inputs_);
  plan.floorPlan = buildFloorPlan(inputs_);
  plan.sceneHover = buildHover(BimLightingOverlayKind::SceneHover,
                               inputs_.sceneHoverCommands, true, inputs_);
  plan.bimHover = buildHover(BimLightingOverlayKind::BimHover,
                             inputs_.bimHoverCommands, inputs_.bimGeometryReady,
                             inputs_);
  plan.nativePointHover = buildNativePrimitiveHighlight(
      BimLightingOverlayKind::NativePointHover,
      BimLightingOverlayPipeline::BimPointCloudDepth,
      inputs_.pipelines.bimPointCloudDepth, inputs_.nativePointHoverCommands,
      kHoverColor, 0.85f, nativePointHoverWidth(inputs_.nativePointSize), false,
      inputs_);
  plan.nativeCurveHover = buildNativePrimitiveHighlight(
      BimLightingOverlayKind::NativeCurveHover,
      BimLightingOverlayPipeline::BimCurveDepth,
      inputs_.pipelines.bimCurveDepth, inputs_.nativeCurveHoverCommands,
      kHoverColor, 0.85f, nativeCurveHoverWidth(inputs_.nativeCurveLineWidth),
      true, inputs_);
  plan.sceneSelectionOutline =
      buildSelectionOutline(inputs_.sceneSelectionCommands, true, inputs_);
  plan.bimSelectionOutline = buildSelectionOutline(
      inputs_.bimSelectionCommands, inputs_.bimGeometryReady, inputs_);
  plan.nativePointSelection = buildNativePrimitiveHighlight(
      BimLightingOverlayKind::NativePointSelection,
      BimLightingOverlayPipeline::BimPointCloudDepth,
      inputs_.pipelines.bimPointCloudDepth,
      inputs_.nativePointSelectionCommands, kSelectionColor, 1.0f,
      nativePointSelectionWidth(inputs_.nativePointSize), false, inputs_);
  plan.nativeCurveSelection = buildNativePrimitiveHighlight(
      BimLightingOverlayKind::NativeCurveSelection,
      BimLightingOverlayPipeline::BimCurveDepth,
      inputs_.pipelines.bimCurveDepth, inputs_.nativeCurveSelectionCommands,
      kSelectionColor, 1.0f,
      nativeCurveSelectionWidth(inputs_.nativeCurveLineWidth), true, inputs_);
  plan.nativeCurveStrokeHover = buildNativePrimitiveHighlight(
      BimLightingOverlayKind::NativeCurveStrokeHover,
      BimLightingOverlayPipeline::BimCurveStrokeDepth,
      inputs_.pipelines.bimCurveStrokeDepth,
      inputs_.nativeCurveStrokeHoverCommands, kHoverColor, 0.85f, 1.0f,
      false, inputs_);
  plan.nativeCurveStrokeSelection = buildNativePrimitiveHighlight(
      BimLightingOverlayKind::NativeCurveStrokeSelection,
      BimLightingOverlayPipeline::BimCurveStrokeDepth,
      inputs_.pipelines.bimCurveStrokeDepth,
      inputs_.nativeCurveStrokeSelectionCommands, kSelectionColor, 1.0f,
      1.0f, false, inputs_);
  plan.coordinationIssueMarkers = buildCoordinationMarker(
      BimLightingOverlayKind::CoordinationIssueMarker,
      inputs_.coordinationIssueMarkers,
      inputs_.coordinationIssueMarkerCommands, inputs_);
  plan.coordinationClashMarkers = buildCoordinationMarker(
      BimLightingOverlayKind::CoordinationClashMarker,
      inputs_.coordinationClashMarkers,
      inputs_.coordinationClashMarkerCommands, inputs_);
  plan.sectionPlaneVisual = buildSectionPlaneVisual(inputs_);
  return plan;
}

BimLightingOverlayPlan
buildBimLightingOverlayPlan(const BimLightingOverlayInputs &inputs) {
  return BimLightingOverlayPlanner(inputs).build();
}

} // namespace container::renderer
