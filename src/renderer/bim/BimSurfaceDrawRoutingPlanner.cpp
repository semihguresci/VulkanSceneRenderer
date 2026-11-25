#include "Container/renderer/bim/BimSurfaceDrawRoutingPlanner.h"

#include "Container/renderer/bim/BimManager.h"

namespace container::renderer {

namespace {

[[nodiscard]] bool hasDrawCommands(const std::vector<DrawCommand> *commands) {
  return commands != nullptr && !commands->empty();
}

[[nodiscard]] bool
hasSplitOpaqueDrawCommands(const BimSurfaceDrawLists &draws) {
  return hasDrawCommands(draws.opaqueSingleSidedDrawCommands) ||
         hasDrawCommands(draws.opaqueWindingFlippedDrawCommands) ||
         hasDrawCommands(draws.opaqueDoubleSidedDrawCommands);
}

[[nodiscard]] bool
hasSplitTransparentDrawCommands(const BimSurfaceDrawLists &draws) {
  return hasDrawCommands(draws.transparentSingleSidedDrawCommands) ||
         hasDrawCommands(draws.transparentWindingFlippedDrawCommands) ||
         hasDrawCommands(draws.transparentDoubleSidedDrawCommands);
}

[[nodiscard]] const std::vector<DrawCommand> *
primaryOpaqueDrawCommands(const BimSurfaceDrawLists &draws) {
  return hasSplitOpaqueDrawCommands(draws) ? draws.opaqueSingleSidedDrawCommands
                                           : draws.opaqueDrawCommands;
}

[[nodiscard]] const std::vector<DrawCommand> *
primaryTransparentDrawCommands(const BimSurfaceDrawLists &draws) {
  return hasSplitTransparentDrawCommands(draws)
             ? draws.transparentSingleSidedDrawCommands
             : draws.transparentDrawCommands;
}

[[nodiscard]] BimDrawCompactionSlot singleSidedSlot(BimSurfaceDrawKind kind) {
  return kind == BimSurfaceDrawKind::Transparent
             ? BimDrawCompactionSlot::TransparentSingleSided
             : BimDrawCompactionSlot::OpaqueSingleSided;
}

[[nodiscard]] BimDrawCompactionSlot
windingFlippedSlot(BimSurfaceDrawKind kind) {
  return kind == BimSurfaceDrawKind::Transparent
             ? BimDrawCompactionSlot::TransparentWindingFlipped
             : BimDrawCompactionSlot::OpaqueWindingFlipped;
}

[[nodiscard]] BimDrawCompactionSlot doubleSidedSlot(BimSurfaceDrawKind kind) {
  return kind == BimSurfaceDrawKind::Transparent
             ? BimDrawCompactionSlot::TransparentDoubleSided
             : BimDrawCompactionSlot::OpaqueDoubleSided;
}

[[nodiscard]] const std::vector<DrawCommand> *
windingFlippedCommands(BimSurfaceDrawKind kind,
                       const BimSurfaceDrawLists &draws) {
  return kind == BimSurfaceDrawKind::Transparent
             ? draws.transparentWindingFlippedDrawCommands
             : draws.opaqueWindingFlippedDrawCommands;
}

[[nodiscard]] const std::vector<DrawCommand> *
doubleSidedCommands(BimSurfaceDrawKind kind, const BimSurfaceDrawLists &draws) {
  return kind == BimSurfaceDrawKind::Transparent
             ? draws.transparentDoubleSidedDrawCommands
             : draws.opaqueDoubleSidedDrawCommands;
}

[[nodiscard]] const std::vector<DrawCommand> *
singleSidedCommands(BimSurfaceDrawKind kind, const BimSurfaceDrawLists &draws) {
  return kind == BimSurfaceDrawKind::Transparent
             ? primaryTransparentDrawCommands(draws)
             : primaryOpaqueDrawCommands(draws);
}

} // namespace

std::optional<BimDrawCompactionSlot> bimViewportCompactionSlot(BimDrawCompactionSlot slot) {
  switch (slot) {
  case BimDrawCompactionSlot::OpaqueSingleSided:
    return BimDrawCompactionSlot::ViewportOpaqueSingleSided;
  case BimDrawCompactionSlot::OpaqueWindingFlipped:
    return BimDrawCompactionSlot::ViewportOpaqueWindingFlipped;
  case BimDrawCompactionSlot::OpaqueDoubleSided:
    return BimDrawCompactionSlot::ViewportOpaqueDoubleSided;
  default:
    return std::nullopt;
  }
}

bool hasBimSurfaceOpaqueDrawCommands(const BimSurfaceDrawLists &draws) {
  return hasDrawCommands(draws.opaqueDrawCommands) ||
         hasSplitOpaqueDrawCommands(draws);
}

bool hasBimSurfaceTransparentDrawCommands(const BimSurfaceDrawLists &draws) {
  return hasDrawCommands(draws.transparentDrawCommands) ||
         hasSplitTransparentDrawCommands(draws);
}

BimSurfaceDrawRoutingPlanner::BimSurfaceDrawRoutingPlanner(
    BimSurfaceDrawRoutingInputs inputs)
    : inputs_(inputs) {}

BimSurfaceDrawRoutingPlan BimSurfaceDrawRoutingPlanner::build() const {
  const bool cpuFallbackAllowed = !inputs_.gpuVisibilityOwnsCpuFallback;
  const std::vector<DrawCommand> *singleSided =
      singleSidedCommands(inputs_.kind, inputs_.draws);
  const std::vector<DrawCommand> *windingFlipped =
      windingFlippedCommands(inputs_.kind, inputs_.draws);
  const std::vector<DrawCommand> *doubleSided =
      doubleSidedCommands(inputs_.kind, inputs_.draws);
  const auto gpuAllowed = [&](const std::vector<DrawCommand> *commands) {
    return inputs_.gpuCompactionEligible && hasDrawCommands(commands);
  };
  BimSurfaceDrawRoutingPlan plan{};
  plan.routeCount = static_cast<uint32_t>(plan.routes.size());
  plan.routes = {{
      {.kind = BimSurfaceDrawRouteKind::SingleSided,
       .gpuSlot = singleSidedSlot(inputs_.kind),
       .cpuCommands = singleSided,
       .gpuCompactionAllowed = gpuAllowed(singleSided),
       .cpuFallbackAllowed = cpuFallbackAllowed},
      {.kind = BimSurfaceDrawRouteKind::WindingFlipped,
       .gpuSlot = windingFlippedSlot(inputs_.kind),
       .cpuCommands = windingFlipped,
       .gpuCompactionAllowed = gpuAllowed(windingFlipped),
       .cpuFallbackAllowed = cpuFallbackAllowed},
      {.kind = BimSurfaceDrawRouteKind::DoubleSided,
       .gpuSlot = doubleSidedSlot(inputs_.kind),
       .cpuCommands = doubleSided,
       .gpuCompactionAllowed = gpuAllowed(doubleSided),
       .cpuFallbackAllowed = cpuFallbackAllowed},
  }};
  return plan;
}

BimSurfaceDrawRoutingPlan
buildBimSurfaceDrawRoutingPlan(const BimSurfaceDrawRoutingInputs &inputs) {
  return BimSurfaceDrawRoutingPlanner(inputs).build();
}

} // namespace container::renderer
