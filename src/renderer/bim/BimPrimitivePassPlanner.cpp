#include "Container/renderer/bim/BimPrimitivePassPlanner.h"

#include "Container/renderer/bim/BimManager.h"

#include <algorithm>

namespace container::renderer {

namespace {

[[nodiscard]] bool hasDrawCommands(const std::vector<DrawCommand> *commands) {
  return commands != nullptr && !commands->empty();
}

[[nodiscard]] bool
hasOpaqueDrawCommands(const BimPrimitivePassDrawLists &draws) {
  return hasDrawCommands(draws.opaqueDrawCommands) ||
         hasDrawCommands(draws.opaqueSingleSidedDrawCommands) ||
         hasDrawCommands(draws.opaqueWindingFlippedDrawCommands) ||
         hasDrawCommands(draws.opaqueDoubleSidedDrawCommands);
}

[[nodiscard]] bool
hasTransparentDrawCommands(const BimPrimitivePassDrawLists &draws) {
  return hasDrawCommands(draws.transparentDrawCommands) ||
         hasDrawCommands(draws.transparentSingleSidedDrawCommands) ||
         hasDrawCommands(draws.transparentWindingFlippedDrawCommands) ||
         hasDrawCommands(draws.transparentDoubleSidedDrawCommands);
}

[[nodiscard]] std::array<BimDrawCompactionSlot, 2>
nativeGpuSlots(BimPrimitivePassKind kind) {
  if (kind == BimPrimitivePassKind::Curves) {
    return {BimDrawCompactionSlot::NativeCurveOpaque,
            BimDrawCompactionSlot::NativeCurveTransparent};
  }
  return {BimDrawCompactionSlot::NativePointOpaque,
          BimDrawCompactionSlot::NativePointTransparent};
}

void appendCpuSource(std::vector<const std::vector<DrawCommand> *> &sources,
                     const std::vector<DrawCommand> *commands) {
  if (hasDrawCommands(commands)) {
    sources.push_back(commands);
  }
}

void appendAggregateOrSplitCpuSources(
    std::vector<const std::vector<DrawCommand> *> &sources,
    const std::vector<DrawCommand> *aggregateCommands,
    const std::array<const std::vector<DrawCommand> *, 3> &splitCommands) {
  if (hasDrawCommands(aggregateCommands)) {
    sources.push_back(aggregateCommands);
    return;
  }

  for (const std::vector<DrawCommand> *commands : splitCommands) {
    appendCpuSource(sources, commands);
  }
}

[[nodiscard]] std::vector<const std::vector<DrawCommand> *>
cpuDrawSources(const BimPrimitivePassDrawLists &draws) {
  std::vector<const std::vector<DrawCommand> *> sources;
  sources.reserve(6u);
  appendAggregateOrSplitCpuSources(sources, draws.opaqueDrawCommands,
                                   {draws.opaqueSingleSidedDrawCommands,
                                    draws.opaqueWindingFlippedDrawCommands,
                                    draws.opaqueDoubleSidedDrawCommands});
  appendAggregateOrSplitCpuSources(sources, draws.transparentDrawCommands,
                                   {draws.transparentSingleSidedDrawCommands,
                                    draws.transparentWindingFlippedDrawCommands,
                                    draws.transparentDoubleSidedDrawCommands});
  return sources;
}

} // namespace

bool hasBimPrimitivePassDrawCommands(const BimPrimitivePassDrawLists &draws) {
  return hasOpaqueDrawCommands(draws) || hasTransparentDrawCommands(draws);
}

BimPrimitivePassPlanner::BimPrimitivePassPlanner(
    BimPrimitivePassPlanInputs inputs)
    : inputs_(inputs) {}

BimPrimitivePassPlan BimPrimitivePassPlanner::build() const {
  const bool hasNativeDraws =
      hasBimPrimitivePassDrawCommands(inputs_.nativeDraws);
  const BimPrimitivePassDrawLists &selectedDraws =
      hasNativeDraws ? inputs_.nativeDraws : inputs_.placeholderDraws;
  const bool placeholderAllowed =
      hasNativeDraws || inputs_.placeholderRangePreviewEnabled;
  const bool active = inputs_.enabled && placeholderAllowed &&
                      hasBimPrimitivePassDrawCommands(selectedDraws);
  const bool gpuCompaction =
      active && hasNativeDraws && inputs_.nativeDrawsUseGpuVisibility;

  BimPrimitivePassPlan plan{};
  plan.active = active;
  plan.depthTest = inputs_.depthTest;
  plan.nativeDrawsSelected = hasNativeDraws;
  plan.useObjectColor =
      hasNativeDraws && inputs_.kind == BimPrimitivePassKind::Curves;
  plan.gpuCompaction = gpuCompaction;
  plan.opacity = std::clamp(inputs_.opacity, 0.0f, 1.0f);
  plan.primitiveSize = std::max(inputs_.primitiveSize, 1.0f);
  plan.cpuDrawSources = gpuCompaction
                            ? std::vector<const std::vector<DrawCommand> *>{}
                            : cpuDrawSources(selectedDraws);
  plan.gpuSlots = nativeGpuSlots(inputs_.kind);
  plan.gpuSlotCount = gpuCompaction ? 2u : 0u;
  return plan;
}

BimPrimitivePassPlan
buildBimPrimitivePassPlan(const BimPrimitivePassPlanInputs &inputs) {
  return BimPrimitivePassPlanner(inputs).build();
}

} // namespace container::renderer
