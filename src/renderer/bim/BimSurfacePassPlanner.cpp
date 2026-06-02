#include "Container/renderer/bim/BimSurfacePassPlanner.h"

#include <algorithm>

namespace container::renderer {

namespace {

[[nodiscard]] bool commonBimSurfacePassReady(
    const BimSurfacePassInputs &inputs) {
  return inputs.passReady && inputs.geometryReady && inputs.descriptorSetReady &&
         inputs.bindlessPushConstantsReady && inputs.basePipelineReady;
}

[[nodiscard]] bool isMeshSource(BimSurfacePassSourceKind source) {
  return source == BimSurfacePassSourceKind::Mesh;
}

[[nodiscard]] BimSurfaceDrawKind
drawKindForSurfacePass(BimSurfacePassKind kind) {
  switch (kind) {
  case BimSurfacePassKind::DepthPrepass:
  case BimSurfacePassKind::GBuffer:
  case BimSurfacePassKind::OpaqueLighting:
    return BimSurfaceDrawKind::Opaque;
  case BimSurfacePassKind::TransparentPick:
  case BimSurfacePassKind::TransparentLighting:
    return BimSurfaceDrawKind::Transparent;
  }
  return BimSurfaceDrawKind::Opaque;
}

[[nodiscard]] bool surfaceSourceHasDrawCommands(
    BimSurfaceDrawKind drawKind, const BimSurfaceDrawLists &draws) {
  return drawKind == BimSurfaceDrawKind::Transparent
             ? hasBimSurfaceTransparentDrawCommands(draws)
             : hasBimSurfaceOpaqueDrawCommands(draws);
}

[[nodiscard]] bool surfacePassWritesSemanticColor(BimSurfacePassKind kind) {
  return kind == BimSurfacePassKind::GBuffer ||
         kind == BimSurfacePassKind::OpaqueLighting ||
         kind == BimSurfacePassKind::TransparentLighting;
}

[[nodiscard]] BimSurfaceDrawRoutingInputs
routingInputsForSurfaceSource(BimSurfaceDrawKind drawKind,
                              const BimSurfacePassSource &source) {
  return {.kind = drawKind,
          .draws = source.draws,
          .gpuCompactionEligible =
              isMeshSource(source.source) && source.gpuCompactionEligible,
          .gpuVisibilityOwnsCpuFallback =
              isMeshSource(source.source) &&
              source.gpuVisibilityOwnsCpuFallback};
}

void appendSourcePlan(BimSurfacePassPlan &plan,
                      BimSurfaceDrawKind drawKind,
                      const BimSurfacePassSource &source) {
  if (!surfaceSourceHasDrawCommands(drawKind, source.draws) ||
      plan.sourceCount >= plan.sources.size()) {
    return;
  }

  const BimSurfaceDrawRoutingPlan routingPlan =
      buildBimSurfaceDrawRoutingPlan(
          routingInputsForSurfaceSource(drawKind, source));
  plan.sources[plan.sourceCount] = {.source = source.source,
                                    .routes = routingPlan.routes,
                                    .routeCount = routingPlan.routeCount};
  ++plan.sourceCount;
}

} // namespace

BimSurfacePassPlanner::BimSurfacePassPlanner(BimSurfacePassInputs inputs)
    : inputs_(inputs) {}

BimSurfacePassPlan BimSurfacePassPlanner::build() const {
  BimSurfacePassPlan plan{};
  if (!commonBimSurfacePassReady(inputs_)) {
    return plan;
  }

  const BimSurfaceDrawKind drawKind = drawKindForSurfacePass(inputs_.kind);
  plan.writesSemanticColorMode = surfacePassWritesSemanticColor(inputs_.kind);
  plan.semanticColorMode =
      plan.writesSemanticColorMode ? inputs_.semanticColorMode : 0u;

  const uint32_t sourceCount = std::min(
      inputs_.sourceCount, static_cast<uint32_t>(inputs_.sources.size()));
  for (uint32_t sourceIndex = 0u; sourceIndex < sourceCount; ++sourceIndex) {
    appendSourcePlan(plan, drawKind, inputs_.sources[sourceIndex]);
  }

  plan.active = plan.sourceCount > 0u;
  return plan;
}

BimSurfacePassPlan
buildBimSurfacePassPlan(const BimSurfacePassInputs &inputs) {
  return BimSurfacePassPlanner(inputs).build();
}

} // namespace container::renderer
