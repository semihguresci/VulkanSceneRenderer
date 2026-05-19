#include "Container/renderer/bim/BimDrawCompactionPlanner.h"

#include "Container/renderer/bim/BimManager.h"

#include <algorithm>

namespace container::renderer {

namespace {

[[nodiscard]] bool hasDrawCommands(const std::vector<DrawCommand> *commands) {
  return commands != nullptr && !commands->empty();
}

void appendSource(std::vector<BimDrawCompactionPlanSource> &plan,
                  BimDrawCompactionSlot slot,
                  const std::vector<DrawCommand> *commands) {
  if (hasDrawCommands(commands)) {
    plan.push_back({.slot = slot, .commands = commands});
  }
}

[[nodiscard]] const std::vector<DrawCommand> *
transparentSingleSidedSource(const BimDrawCompactionPlanInputs &inputs) {
  return hasDrawCommands(inputs.transparentSingleSided)
             ? inputs.transparentSingleSided
             : inputs.transparentAggregate;
}

} // namespace

BimDrawCompactionPlanner::BimDrawCompactionPlanner(
    BimDrawCompactionPlanInputs inputs)
    : inputs_(inputs) {}

std::vector<BimDrawCompactionPlanSource>
BimDrawCompactionPlanner::build() const {
  std::vector<BimDrawCompactionPlanSource> plan;
  plan.reserve(kBimDrawCompactionSlotCount);

  if (inputs_.meshCompactionEnabled) {
    appendSource(plan, BimDrawCompactionSlot::OpaqueSingleSided,
                 inputs_.opaqueSingleSided);
    appendSource(plan, BimDrawCompactionSlot::OpaqueWindingFlipped,
                 inputs_.opaqueWindingFlipped);
    appendSource(plan, BimDrawCompactionSlot::OpaqueDoubleSided,
                 inputs_.opaqueDoubleSided);
    appendSource(plan, BimDrawCompactionSlot::TransparentSingleSided,
                 transparentSingleSidedSource(inputs_));
    appendSource(plan, BimDrawCompactionSlot::TransparentWindingFlipped,
                 inputs_.transparentWindingFlipped);
    appendSource(plan, BimDrawCompactionSlot::TransparentDoubleSided,
                 inputs_.transparentDoubleSided);
  }
  appendSource(plan, BimDrawCompactionSlot::NativePointOpaque,
               inputs_.nativePointOpaque);
  appendSource(plan, BimDrawCompactionSlot::NativePointTransparent,
               inputs_.nativePointTransparent);
  appendSource(plan, BimDrawCompactionSlot::NativeCurveOpaque,
               inputs_.nativeCurveOpaque);
  appendSource(plan, BimDrawCompactionSlot::NativeCurveTransparent,
               inputs_.nativeCurveTransparent);

  return plan;
}

BimDrawCompactionPlanInputs
makeBimDrawCompactionPlanInputs(const BimManager &bimManager) {
  return {
      .opaqueSingleSided = &bimManager.opaqueSingleSidedDrawCommands(),
      .opaqueWindingFlipped = &bimManager.opaqueWindingFlippedDrawCommands(),
      .opaqueDoubleSided = &bimManager.opaqueDoubleSidedDrawCommands(),
      .transparentAggregate = &bimManager.transparentDrawCommands(),
      .transparentSingleSided =
          &bimManager.transparentSingleSidedDrawCommands(),
      .transparentWindingFlipped =
          &bimManager.transparentWindingFlippedDrawCommands(),
      .transparentDoubleSided =
          &bimManager.transparentDoubleSidedDrawCommands(),
      .nativePointOpaque =
          &bimManager.nativePointDrawLists().opaqueDrawCommands,
      .nativePointTransparent =
          &bimManager.nativePointDrawLists().transparentDrawCommands,
      .nativeCurveOpaque =
          &bimManager.nativeCurveDrawLists().opaqueDrawCommands,
      .nativeCurveTransparent =
          &bimManager.nativeCurveDrawLists().transparentDrawCommands,
  };
}

size_t
bimDrawCompactionOutputCapacity(const std::vector<DrawCommand> &commands) {
  size_t outputCapacity = 0u;
  for (const DrawCommand &command : commands) {
    outputCapacity += std::max(command.instanceCount, 1u);
  }
  return outputCapacity;
}

std::vector<BimDrawCompactionPlanSource>
buildBimDrawCompactionPlan(const BimDrawCompactionPlanInputs &inputs) {
  return BimDrawCompactionPlanner(inputs).build();
}

} // namespace container::renderer
