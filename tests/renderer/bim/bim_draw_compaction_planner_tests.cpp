#include "Container/renderer/bim/BimManager.h"
#include "Container/renderer/bim/BimDrawCompactionPlanner.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

using container::renderer::BimDrawCompactionPlanInputs;
using container::renderer::BimDrawCompactionSlot;
using container::renderer::bimDrawCompactionOutputCapacity;
using container::renderer::buildBimDrawCompactionPlan;
using container::renderer::DrawCommand;

[[nodiscard]] std::vector<DrawCommand> drawCommands(uint32_t firstIndex) {
  return {DrawCommand{.objectIndex = firstIndex,
                      .firstIndex = firstIndex,
                      .indexCount = 3u,
                      .instanceCount = 1u}};
}

TEST(BimDrawCompactionPlannerTests, SkipsEmptyAndMissingSources) {
  const std::vector<DrawCommand> empty;

  const auto plan = buildBimDrawCompactionPlan(
      {.opaqueSingleSided = &empty, .opaqueWindingFlipped = nullptr});

  EXPECT_TRUE(plan.empty());
}

TEST(BimDrawCompactionPlannerTests, MapsSurfaceSlotsInStableOrder) {
  const auto opaqueSingleSided = drawCommands(1u);
  const auto opaqueWindingFlipped = drawCommands(2u);
  const auto opaqueDoubleSided = drawCommands(3u);
  const auto transparentSingleSided = drawCommands(4u);
  const auto transparentWindingFlipped = drawCommands(5u);
  const auto transparentDoubleSided = drawCommands(6u);

  const auto plan = buildBimDrawCompactionPlan(
      {.opaqueSingleSided = &opaqueSingleSided,
       .opaqueWindingFlipped = &opaqueWindingFlipped,
       .opaqueDoubleSided = &opaqueDoubleSided,
       .transparentSingleSided = &transparentSingleSided,
       .transparentWindingFlipped = &transparentWindingFlipped,
       .transparentDoubleSided = &transparentDoubleSided});

  ASSERT_EQ(plan.size(), 6u);
  EXPECT_EQ(plan[0].slot, BimDrawCompactionSlot::OpaqueSingleSided);
  EXPECT_EQ(plan[0].commands, &opaqueSingleSided);
  EXPECT_EQ(plan[1].slot, BimDrawCompactionSlot::OpaqueWindingFlipped);
  EXPECT_EQ(plan[1].commands, &opaqueWindingFlipped);
  EXPECT_EQ(plan[2].slot, BimDrawCompactionSlot::OpaqueDoubleSided);
  EXPECT_EQ(plan[2].commands, &opaqueDoubleSided);
  EXPECT_EQ(plan[3].slot, BimDrawCompactionSlot::TransparentSingleSided);
  EXPECT_EQ(plan[3].commands, &transparentSingleSided);
  EXPECT_EQ(plan[4].slot, BimDrawCompactionSlot::TransparentWindingFlipped);
  EXPECT_EQ(plan[4].commands, &transparentWindingFlipped);
  EXPECT_EQ(plan[5].slot, BimDrawCompactionSlot::TransparentDoubleSided);
  EXPECT_EQ(plan[5].commands, &transparentDoubleSided);
}

TEST(BimDrawCompactionPlannerTests,
     TransparentSingleSidedFallsBackToAggregateTransparent) {
  const std::vector<DrawCommand> transparentSingleSided;
  const auto transparentAggregate = drawCommands(10u);

  const auto plan = buildBimDrawCompactionPlan(
      {.transparentAggregate = &transparentAggregate,
       .transparentSingleSided = &transparentSingleSided});

  ASSERT_EQ(plan.size(), 1u);
  EXPECT_EQ(plan[0].slot, BimDrawCompactionSlot::TransparentSingleSided);
  EXPECT_EQ(plan[0].commands, &transparentAggregate);
}

TEST(BimDrawCompactionPlannerTests,
     TransparentSingleSidedPrefersSplitOverAggregateTransparent) {
  const auto transparentSingleSided = drawCommands(11u);
  const auto transparentAggregate = drawCommands(12u);

  const auto plan = buildBimDrawCompactionPlan(
      {.transparentAggregate = &transparentAggregate,
       .transparentSingleSided = &transparentSingleSided});

  ASSERT_EQ(plan.size(), 1u);
  EXPECT_EQ(plan[0].slot, BimDrawCompactionSlot::TransparentSingleSided);
  EXPECT_EQ(plan[0].commands, &transparentSingleSided);
}

TEST(BimDrawCompactionPlannerTests, IncludesNativePointAndCurveSlots) {
  const auto nativePointOpaque = drawCommands(20u);
  const auto nativePointTransparent = drawCommands(21u);
  const auto nativeCurveOpaque = drawCommands(22u);
  const auto nativeCurveTransparent = drawCommands(23u);

  const auto plan = buildBimDrawCompactionPlan(
      {.nativePointOpaque = &nativePointOpaque,
       .nativePointTransparent = &nativePointTransparent,
       .nativeCurveOpaque = &nativeCurveOpaque,
       .nativeCurveTransparent = &nativeCurveTransparent});

  ASSERT_EQ(plan.size(), 4u);
  EXPECT_EQ(plan[0].slot, BimDrawCompactionSlot::NativePointOpaque);
  EXPECT_EQ(plan[0].commands, &nativePointOpaque);
  EXPECT_EQ(plan[1].slot, BimDrawCompactionSlot::NativePointTransparent);
  EXPECT_EQ(plan[1].commands, &nativePointTransparent);
  EXPECT_EQ(plan[2].slot, BimDrawCompactionSlot::NativeCurveOpaque);
  EXPECT_EQ(plan[2].commands, &nativeCurveOpaque);
  EXPECT_EQ(plan[3].slot, BimDrawCompactionSlot::NativeCurveTransparent);
  EXPECT_EQ(plan[3].commands, &nativeCurveTransparent);
}

TEST(BimDrawCompactionPlannerTests,
     MeshCompactionCanBeDisabledWithoutDroppingNativePrimitiveSlots) {
  const auto opaqueSingleSided = drawCommands(30u);
  const auto transparentSingleSided = drawCommands(31u);
  const auto nativePointOpaque = drawCommands(32u);
  const auto nativeCurveTransparent = drawCommands(33u);

  const auto plan = buildBimDrawCompactionPlan(
      {.meshCompactionEnabled = false,
       .opaqueSingleSided = &opaqueSingleSided,
       .transparentSingleSided = &transparentSingleSided,
       .nativePointOpaque = &nativePointOpaque,
       .nativeCurveTransparent = &nativeCurveTransparent});

  ASSERT_EQ(plan.size(), 2u);
  EXPECT_EQ(plan[0].slot, BimDrawCompactionSlot::NativePointOpaque);
  EXPECT_EQ(plan[0].commands, &nativePointOpaque);
  EXPECT_EQ(plan[1].slot, BimDrawCompactionSlot::NativeCurveTransparent);
  EXPECT_EQ(plan[1].commands, &nativeCurveTransparent);
}

TEST(BimDrawCompactionPlannerTests,
     OutputCapacityCountsEveryDrawInstanceWithoutObjectCountClamp) {
  const std::vector<DrawCommand> commands = {
      DrawCommand{.objectIndex = 0u,
                  .firstIndex = 0u,
                  .indexCount = 3u,
                  .instanceCount = 3u},
      DrawCommand{.objectIndex = 3u,
                  .firstIndex = 3u,
                  .indexCount = 3u,
                  .instanceCount = 2u},
      DrawCommand{.objectIndex = 5u,
                  .firstIndex = 6u,
                  .indexCount = 3u,
                  .instanceCount = 0u},
  };

  EXPECT_EQ(bimDrawCompactionOutputCapacity(commands), 6u);
}

} // namespace
