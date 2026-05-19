#include "Container/renderer/scene/SceneOpaqueDrawPlanner.h"

#include "Container/renderer/scene/DrawCommand.h"

namespace container::renderer {

namespace {

[[nodiscard]] bool hasDrawCommands(const std::vector<DrawCommand> *commands) {
  return commands != nullptr && !commands->empty();
}

void appendCpuRoute(SceneOpaqueDrawPlan &plan, SceneOpaqueDrawRouteKind kind,
                    SceneOpaqueDrawPipeline pipeline,
                    const std::vector<DrawCommand> *commands) {
  if (!hasDrawCommands(commands) || plan.cpuRouteCount >= plan.cpuRoutes.size()) {
    return;
  }

  plan.cpuRoutes[plan.cpuRouteCount] = {
      .kind = kind,
      .pipeline = pipeline,
      .commands = commands,
  };
  ++plan.cpuRouteCount;
}

[[nodiscard]] const std::vector<DrawCommand> *
primaryCpuSingleSidedCommands(const SceneOpaqueDrawLists &draws) {
  return hasDrawCommands(draws.singleSided) ? draws.singleSided
                                            : draws.aggregate;
}

} // namespace

SceneOpaqueDrawPlanner::SceneOpaqueDrawPlanner(SceneOpaqueDrawInputs inputs)
    : inputs_(inputs) {}

SceneOpaqueDrawPlan SceneOpaqueDrawPlanner::build() const {
  SceneOpaqueDrawPlan plan{};
  const bool useOcclusionIndirect =
      inputs_.preferOccludedGpuIndirect &&
      inputs_.occludedGpuIndirectAvailable;
  const bool gpuIndirectAvailable =
      useOcclusionIndirect || inputs_.gpuIndirectAvailable;
  plan.useGpuIndirectSingleSided =
      gpuIndirectAvailable && hasDrawCommands(inputs_.draws.singleSided);
  plan.gpuIndirectRoute.indirectSource =
      useOcclusionIndirect ? SceneOpaqueIndirectSource::OcclusionCull
                           : SceneOpaqueIndirectSource::FrustumCull;
  plan.gpuIndirectRoute.commands =
      plan.useGpuIndirectSingleSided ? inputs_.draws.singleSided : nullptr;

  if (!plan.useGpuIndirectSingleSided) {
    appendCpuRoute(plan, SceneOpaqueDrawRouteKind::CpuSingleSided,
                   SceneOpaqueDrawPipeline::Primary,
                   primaryCpuSingleSidedCommands(inputs_.draws));
  }
  appendCpuRoute(plan, SceneOpaqueDrawRouteKind::CpuWindingFlipped,
                 SceneOpaqueDrawPipeline::FrontCull,
                 inputs_.draws.windingFlipped);
  appendCpuRoute(plan, SceneOpaqueDrawRouteKind::CpuDoubleSided,
                 SceneOpaqueDrawPipeline::NoCull, inputs_.draws.doubleSided);
  return plan;
}

SceneOpaqueDrawPlan
buildSceneOpaqueDrawPlan(const SceneOpaqueDrawInputs &inputs) {
  return SceneOpaqueDrawPlanner(inputs).build();
}

} // namespace container::renderer
