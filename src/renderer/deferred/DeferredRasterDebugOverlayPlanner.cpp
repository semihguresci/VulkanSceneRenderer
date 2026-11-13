#include "Container/renderer/deferred/DeferredRasterDebugOverlayPlanner.h"

#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/scene/DrawCommand.h"

#include <algorithm>

namespace container::renderer {

DeferredDebugOverlayDrawLists
debugOverlayDrawLists(const FrameDrawLists &draws) {
  return {
      .opaqueDrawCommands = draws.opaqueDrawCommands,
      .transparentDrawCommands = draws.transparentDrawCommands,
      .opaqueSingleSidedDrawCommands = draws.opaqueSingleSidedDrawCommands,
      .transparentSingleSidedDrawCommands =
          draws.transparentSingleSidedDrawCommands,
      .opaqueWindingFlippedDrawCommands =
          draws.opaqueWindingFlippedDrawCommands,
      .transparentWindingFlippedDrawCommands =
          draws.transparentWindingFlippedDrawCommands,
      .opaqueDoubleSidedDrawCommands = draws.opaqueDoubleSidedDrawCommands,
      .transparentDoubleSidedDrawCommands =
          draws.transparentDoubleSidedDrawCommands,
  };
}

namespace {

constexpr uint32_t kNormalValidationInvertFaceClassification =
    kDeferredDebugOverlayNormalValidationInvertFaceClassification;
constexpr uint32_t kNormalValidationBothSidesValid =
    kDeferredDebugOverlayNormalValidationBothSidesValid;

[[nodiscard]] bool hasDrawCommands(const std::vector<DrawCommand> *commands) {
  return commands != nullptr && !commands->empty();
}

[[nodiscard]] bool hasPairDrawCommands(
    const std::vector<DrawCommand> *opaque,
    const std::vector<DrawCommand> *transparent) {
  return hasDrawCommands(opaque) || hasDrawCommands(transparent);
}

[[nodiscard]] bool selectedWireframePipelineReady(
    const DeferredDebugOverlayInputs &inputs) {
  return inputs.frameState.wireframeDepthTest
             ? inputs.pipelines.wireframeDepth
             : inputs.pipelines.wireframeNoDepth;
}

[[nodiscard]] DeferredDebugOverlayPipeline selectedWireframePipeline(
    const DeferredDebugOverlayFrameState &state) {
  return state.wireframeDepthTest ? DeferredDebugOverlayPipeline::WireframeDepth
                                  : DeferredDebugOverlayPipeline::WireframeNoDepth;
}

[[nodiscard]] DeferredDebugOverlayPipeline selectedWireframeFrontCullPipeline(
    const DeferredDebugOverlayInputs &inputs) {
  if (inputs.frameState.wireframeDepthTest) {
    return inputs.pipelines.wireframeDepthFrontCull
               ? DeferredDebugOverlayPipeline::WireframeDepthFrontCull
               : DeferredDebugOverlayPipeline::WireframeDepth;
  }
  return inputs.pipelines.wireframeNoDepthFrontCull
             ? DeferredDebugOverlayPipeline::WireframeNoDepthFrontCull
             : DeferredDebugOverlayPipeline::WireframeNoDepth;
}

[[nodiscard]] DeferredDebugOverlayPipeline objectNormalFrontCullPipeline(
    const DeferredDebugOverlayPipelineReadiness &pipelines) {
  return pipelines.objectNormalDebugFrontCull
             ? DeferredDebugOverlayPipeline::ObjectNormalDebugFrontCull
             : DeferredDebugOverlayPipeline::ObjectNormalDebug;
}

[[nodiscard]] DeferredDebugOverlayPipeline objectNormalNoCullPipeline(
    const DeferredDebugOverlayPipelineReadiness &pipelines) {
  return pipelines.objectNormalDebugNoCull
             ? DeferredDebugOverlayPipeline::ObjectNormalDebugNoCull
             : DeferredDebugOverlayPipeline::ObjectNormalDebug;
}

[[nodiscard]] DeferredDebugOverlayPipeline normalValidationFrontCullPipeline(
    const DeferredDebugOverlayPipelineReadiness &pipelines) {
  return pipelines.normalValidationFrontCull
             ? DeferredDebugOverlayPipeline::NormalValidationFrontCull
             : DeferredDebugOverlayPipeline::NormalValidation;
}

[[nodiscard]] DeferredDebugOverlayPipeline normalValidationNoCullPipeline(
    const DeferredDebugOverlayPipelineReadiness &pipelines) {
  return pipelines.normalValidationNoCull
             ? DeferredDebugOverlayPipeline::NormalValidationNoCull
             : DeferredDebugOverlayPipeline::NormalValidation;
}

void appendCommandRoute(DeferredDebugOverlaySourcePlan &plan,
                        DeferredDebugOverlayPipeline pipeline,
                        const std::vector<DrawCommand> *commands,
                        float lineWidth = 1.0f,
                        float rasterLineWidth = 1.0f) {
  if (!hasDrawCommands(commands) || plan.routeCount >= plan.routes.size()) {
    return;
  }

  plan.routes[plan.routeCount] = {
      .pipeline = pipeline,
      .commands = commands,
      .drawLineWidth = lineWidth,
      .rasterLineWidth = rasterLineWidth,
  };
  ++plan.routeCount;
}

void appendPairRoute(DeferredDebugOverlaySourcePlan &plan,
                     DeferredDebugOverlayPipeline pipeline,
                     const std::vector<DrawCommand> *opaque,
                     const std::vector<DrawCommand> *transparent,
                     uint32_t normalValidationFaceFlags = 0u,
                     float lineWidth = 1.0f,
                     float rasterLineWidth = 1.0f) {
  if (!hasPairDrawCommands(opaque, transparent) ||
      plan.routeCount >= plan.routes.size()) {
    return;
  }

  plan.routes[plan.routeCount] = {
      .pipeline = pipeline,
      .opaqueCommands = opaque,
      .transparentCommands = transparent,
      .normalValidationFaceFlags = normalValidationFaceFlags,
      .drawLineWidth = lineWidth,
      .rasterLineWidth = rasterLineWidth,
  };
  ++plan.routeCount;
}

[[nodiscard]] DeferredDebugOverlaySourcePlan makeSourcePlan(
    const DeferredDebugOverlaySourceInput &source) {
  DeferredDebugOverlaySourcePlan plan{};
  plan.source = source.source;
  plan.diagnosticCubeObjectIndex = source.diagnosticCubeObjectIndex;
  return plan;
}

void appendWireframeRoutes(DeferredDebugOverlaySourcePlan &plan,
                           const DeferredDebugOverlaySourceInput &source,
                           const DeferredDebugOverlayInputs &inputs) {
  const DeferredDebugOverlayPipeline mainPipeline =
      selectedWireframePipeline(inputs.frameState);
  const DeferredDebugOverlayPipeline frontCullPipeline =
      selectedWireframeFrontCullPipeline(inputs);
  const float lineWidth = inputs.frameState.wireframeLineWidth;

  appendCommandRoute(plan, mainPipeline,
                     source.draws.opaqueSingleSidedDrawCommands, lineWidth,
                     lineWidth);
  appendCommandRoute(plan, mainPipeline,
                     source.draws.transparentSingleSidedDrawCommands,
                     lineWidth, lineWidth);
  appendCommandRoute(plan, frontCullPipeline,
                     source.draws.opaqueWindingFlippedDrawCommands, lineWidth,
                     lineWidth);
  appendCommandRoute(plan, frontCullPipeline,
                     source.draws.transparentWindingFlippedDrawCommands,
                     lineWidth, lineWidth);
  appendCommandRoute(plan, mainPipeline,
                     source.draws.opaqueDoubleSidedDrawCommands, lineWidth,
                     lineWidth);
  appendCommandRoute(plan, mainPipeline,
                     source.draws.transparentDoubleSidedDrawCommands,
                     lineWidth, lineWidth);
}

void appendObjectNormalRoutes(DeferredDebugOverlaySourcePlan &plan,
                              const DeferredDebugOverlaySourceInput &source,
                              const DeferredDebugOverlayInputs &inputs) {
  appendCommandRoute(plan, DeferredDebugOverlayPipeline::ObjectNormalDebug,
                     source.draws.opaqueSingleSidedDrawCommands);
  appendCommandRoute(plan, DeferredDebugOverlayPipeline::ObjectNormalDebug,
                     source.draws.transparentSingleSidedDrawCommands);
  appendCommandRoute(plan, objectNormalFrontCullPipeline(inputs.pipelines),
                     source.draws.opaqueWindingFlippedDrawCommands);
  appendCommandRoute(plan, objectNormalFrontCullPipeline(inputs.pipelines),
                     source.draws.transparentWindingFlippedDrawCommands);
  appendCommandRoute(plan, objectNormalNoCullPipeline(inputs.pipelines),
                     source.draws.opaqueDoubleSidedDrawCommands);
  appendCommandRoute(plan, objectNormalNoCullPipeline(inputs.pipelines),
                     source.draws.transparentDoubleSidedDrawCommands);
}

[[nodiscard]] DeferredDebugOverlaySourcePlan buildWireframeFullSourcePlan(
    const DeferredDebugOverlaySourceInput &source,
    const DeferredDebugOverlayInputs &inputs) {
  DeferredDebugOverlaySourcePlan plan = makeSourcePlan(source);
  if (!source.geometryReady || !inputs.frameState.wireframeFullMode ||
      !inputs.wireframePushConstantsReady ||
      !selectedWireframePipelineReady(inputs)) {
    return plan;
  }

  appendWireframeRoutes(plan, source, inputs);
  plan.drawDiagnosticCube =
      source.drawDiagnosticCube && inputs.bindlessPushConstantsReady;
  plan.diagnosticCubePipeline = selectedWireframePipeline(inputs.frameState);
  return plan;
}

[[nodiscard]] DeferredDebugOverlaySourcePlan buildObjectNormalSourcePlan(
    const DeferredDebugOverlaySourceInput &source,
    const DeferredDebugOverlayInputs &inputs) {
  DeferredDebugOverlaySourcePlan plan = makeSourcePlan(source);
  if (!source.geometryReady || inputs.frameState.wireframeFullMode ||
      !inputs.frameState.objectSpaceNormalsEnabled ||
      !inputs.bindlessPushConstantsReady ||
      !inputs.pipelines.objectNormalDebug) {
    return plan;
  }

  appendObjectNormalRoutes(plan, source, inputs);
  plan.drawDiagnosticCube = source.drawDiagnosticCube;
  plan.diagnosticCubePipeline = DeferredDebugOverlayPipeline::ObjectNormalDebug;
  return plan;
}

[[nodiscard]] DeferredDebugOverlaySourcePlan buildGeometryOverlaySourcePlan(
    const DeferredDebugOverlaySourceInput &source,
    const DeferredDebugOverlayInputs &inputs) {
  DeferredDebugOverlaySourcePlan plan = makeSourcePlan(source);
  if (!source.geometryReady || !inputs.frameState.geometryOverlayEnabled ||
      !inputs.bindlessPushConstantsReady || !inputs.pipelines.geometryDebug) {
    return plan;
  }

  appendCommandRoute(plan, DeferredDebugOverlayPipeline::GeometryDebug,
                     source.draws.opaqueDrawCommands);
  appendCommandRoute(plan, DeferredDebugOverlayPipeline::GeometryDebug,
                     source.draws.transparentDrawCommands);
  return plan;
}

[[nodiscard]] DeferredDebugOverlaySourcePlan buildNormalValidationSourcePlan(
    const DeferredDebugOverlaySourceInput &source,
    const DeferredDebugOverlayInputs &inputs) {
  DeferredDebugOverlaySourcePlan plan = makeSourcePlan(source);
  if (!source.geometryReady || !inputs.frameState.normalValidationEnabled ||
      !inputs.normalValidationPushConstantsReady ||
      !inputs.pipelines.normalValidation) {
    return plan;
  }

  appendPairRoute(plan, DeferredDebugOverlayPipeline::NormalValidation,
                  source.draws.opaqueSingleSidedDrawCommands,
                  source.draws.transparentSingleSidedDrawCommands, 0u);
  appendPairRoute(plan, normalValidationFrontCullPipeline(inputs.pipelines),
                  source.draws.opaqueWindingFlippedDrawCommands,
                  source.draws.transparentWindingFlippedDrawCommands,
                  kNormalValidationInvertFaceClassification);
  appendPairRoute(plan, normalValidationNoCullPipeline(inputs.pipelines),
                  source.draws.opaqueDoubleSidedDrawCommands,
                  source.draws.transparentDoubleSidedDrawCommands,
                  kNormalValidationBothSidesValid);
  return plan;
}

[[nodiscard]] DeferredDebugOverlaySourcePlan buildSurfaceNormalSourcePlan(
    const DeferredDebugOverlaySourceInput &source,
    const DeferredDebugOverlayInputs &inputs) {
  DeferredDebugOverlaySourcePlan plan = makeSourcePlan(source);
  if (!source.geometryReady || !inputs.frameState.surfaceNormalLinesEnabled ||
      !inputs.surfaceNormalPushConstantsReady ||
      !inputs.pipelines.surfaceNormalLine) {
    return plan;
  }

  const float lineWidth = inputs.frameState.surfaceNormalLineWidth;
  appendPairRoute(plan, DeferredDebugOverlayPipeline::SurfaceNormalLine,
                  source.draws.opaqueDrawCommands,
                  source.draws.transparentDrawCommands, 0u, lineWidth,
                  lineWidth);
  return plan;
}

[[nodiscard]] DeferredDebugOverlaySourcePlan buildWireframeOverlaySourcePlan(
    const DeferredDebugOverlaySourceInput &source,
    const DeferredDebugOverlayInputs &inputs) {
  DeferredDebugOverlaySourcePlan plan = makeSourcePlan(source);
  if (!source.geometryReady || !inputs.frameState.wireframeOverlayMode ||
      !inputs.wireframePushConstantsReady ||
      !selectedWireframePipelineReady(inputs)) {
    return plan;
  }

  appendWireframeRoutes(plan, source, inputs);
  return plan;
}

[[nodiscard]] bool hasSourceWork(const DeferredDebugOverlaySourcePlan &plan) {
  return plan.routeCount > 0u || plan.drawDiagnosticCube;
}

template <size_t SourceCount>
void appendSourcePlan(
    std::array<DeferredDebugOverlaySourcePlan, SourceCount> &plans,
    uint32_t &planCount, const DeferredDebugOverlaySourcePlan &plan) {
  if (!hasSourceWork(plan) || planCount >= plans.size()) {
    return;
  }
  plans[planCount] = plan;
  ++planCount;
}

} // namespace

DeferredRasterDebugOverlayPlanner::DeferredRasterDebugOverlayPlanner(
    DeferredDebugOverlayInputs inputs)
    : inputs_(inputs) {}

DeferredDebugOverlayPlan DeferredRasterDebugOverlayPlanner::build() const {
  DeferredDebugOverlayPlan plan{};
  const uint32_t sourceCount =
      std::min(inputs_.sourceCount,
               static_cast<uint32_t>(inputs_.sources.size()));

  for (uint32_t sourceIndex = 0; sourceIndex < sourceCount; ++sourceIndex) {
    const DeferredDebugOverlaySourceInput &source = inputs_.sources[sourceIndex];
    appendSourcePlan(plan.wireframeFullSources, plan.wireframeFullSourceCount,
                     buildWireframeFullSourcePlan(source, inputs_));
    appendSourcePlan(plan.objectNormalSources, plan.objectNormalSourceCount,
                     buildObjectNormalSourcePlan(source, inputs_));
    appendSourcePlan(plan.geometryOverlaySources,
                     plan.geometryOverlaySourceCount,
                     buildGeometryOverlaySourcePlan(source, inputs_));
    appendSourcePlan(plan.normalValidationSources,
                     plan.normalValidationSourceCount,
                     buildNormalValidationSourcePlan(source, inputs_));
    appendSourcePlan(plan.surfaceNormalSources, plan.surfaceNormalSourceCount,
                     buildSurfaceNormalSourcePlan(source, inputs_));
    appendSourcePlan(plan.wireframeOverlaySources,
                     plan.wireframeOverlaySourceCount,
                     buildWireframeOverlaySourcePlan(source, inputs_));
  }

  return plan;
}

DeferredDebugOverlayPlan
buildDeferredDebugOverlayPlan(const DeferredDebugOverlayInputs &inputs) {
  return DeferredRasterDebugOverlayPlanner(inputs).build();
}

} // namespace container::renderer
