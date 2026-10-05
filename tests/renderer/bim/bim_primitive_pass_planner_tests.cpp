#include "Container/renderer/bim/BimManager.h"
#include "Container/renderer/bim/BimPrimitivePassPlanner.h"
#include "Container/renderer/bim/BimPrimitivePassRecorder.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using container::renderer::BimDrawCompactionSlot;
using container::renderer::BimPrimitiveFramePassRecordInputs;
using container::renderer::BimPrimitivePassDrawLists;
using container::renderer::BimPrimitivePassGeometryBinding;
using container::renderer::BimPrimitivePassKind;
using container::renderer::BimPrimitivePassPlanInputs;
using container::renderer::buildBimPrimitiveFramePassPlanInputs;
using container::renderer::buildBimPrimitivePassPlan;
using container::renderer::DrawCommand;
using container::renderer::hasBimPrimitiveFramePassGeometry;
using container::renderer::hasBimPrimitivePassDrawCommands;
using container::renderer::recordBimPrimitiveFramePassCommands;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

[[nodiscard]] std::vector<DrawCommand> drawCommands(uint32_t firstIndex) {
  return {DrawCommand{.objectIndex = firstIndex,
                      .firstIndex = firstIndex,
                      .indexCount = 3u,
                      .instanceCount = 1u}};
}

[[nodiscard]] BimPrimitivePassDrawLists
opaqueAggregate(const std::vector<DrawCommand> &commands) {
  return {.opaqueDrawCommands = &commands};
}

[[nodiscard]] BimPrimitivePassDrawLists
transparentSplit(const std::vector<DrawCommand> &commands) {
  return {.transparentSingleSidedDrawCommands = &commands};
}

[[nodiscard]] BimPrimitivePassDrawLists
opaqueAggregateAndSplit(const std::vector<DrawCommand> &aggregate,
                        const std::vector<DrawCommand> &split) {
  return {.opaqueDrawCommands = &aggregate,
          .opaqueSingleSidedDrawCommands = &split};
}

[[nodiscard]] BimPrimitivePassDrawLists
opaqueSplits(const std::vector<DrawCommand> &singleSided,
             const std::vector<DrawCommand> &windingFlipped) {
  return {.opaqueSingleSidedDrawCommands = &singleSided,
          .opaqueWindingFlippedDrawCommands = &windingFlipped};
}

TEST(BimPrimitivePassPlannerTests, InactiveWhenDisabledOrMissingDraws) {
  const auto commands = drawCommands(1u);

  EXPECT_FALSE(
      buildBimPrimitivePassPlan({.enabled = false,
                                 .placeholderRangePreviewEnabled = true,
                                 .placeholderDraws = opaqueAggregate(commands)})
          .active);

  EXPECT_FALSE(buildBimPrimitivePassPlan(
                   {.enabled = true, .placeholderRangePreviewEnabled = true})
                   .active);
}

TEST(BimPrimitivePassPlannerTests, PlaceholderDrawsRequirePreviewFlag) {
  const auto commands = drawCommands(2u);

  const auto hidden = buildBimPrimitivePassPlan(
      {.enabled = true,
       .placeholderRangePreviewEnabled = false,
       .placeholderDraws = opaqueAggregate(commands)});
  const auto visible = buildBimPrimitivePassPlan(
      {.enabled = true,
       .placeholderRangePreviewEnabled = true,
       .placeholderDraws = opaqueAggregate(commands)});

  EXPECT_FALSE(hidden.active);
  EXPECT_TRUE(visible.active);
  EXPECT_FALSE(visible.nativeDrawsSelected);
  EXPECT_FALSE(visible.useObjectColor);
  ASSERT_EQ(visible.cpuDrawSources.size(), 1u);
  EXPECT_EQ(visible.cpuDrawSources[0], &commands);
}

TEST(BimPrimitivePassPlannerTests, NativeDrawsDoNotRequirePlaceholderPreview) {
  const auto placeholderCommands = drawCommands(3u);
  const auto nativeCommands = drawCommands(4u);

  const auto plan = buildBimPrimitivePassPlan(
      {.enabled = true,
       .placeholderRangePreviewEnabled = false,
       .placeholderDraws = opaqueAggregate(placeholderCommands),
       .nativeDraws = transparentSplit(nativeCommands)});

  EXPECT_TRUE(plan.active);
  EXPECT_TRUE(plan.nativeDrawsSelected);
  EXPECT_FALSE(plan.gpuCompaction);
  ASSERT_EQ(plan.cpuDrawSources.size(), 1u);
  EXPECT_EQ(plan.cpuDrawSources[0], &nativeCommands);
}

TEST(BimPrimitivePassPlannerTests, NativeGpuVisibilityMapsPointSlots) {
  const auto nativeCommands = drawCommands(5u);

  const auto plan = buildBimPrimitivePassPlan(
      {.kind = BimPrimitivePassKind::Points,
       .enabled = true,
       .nativeDrawsUseGpuVisibility = true,
       .nativeDraws = opaqueAggregate(nativeCommands)});

  ASSERT_TRUE(plan.gpuCompaction);
  EXPECT_FALSE(plan.useObjectColor);
  ASSERT_EQ(plan.gpuSlotCount, 2u);
  EXPECT_TRUE(plan.cpuDrawSources.empty());
  EXPECT_EQ(plan.gpuSlots[0], BimDrawCompactionSlot::NativePointOpaque);
  EXPECT_EQ(plan.gpuSlots[1], BimDrawCompactionSlot::NativePointTransparent);
}

TEST(BimPrimitivePassPlannerTests, CpuCurvesUseObjectColorOnlyForNativeDraws) {
  const auto commands = drawCommands(8u);
  const auto native =
      buildBimPrimitivePassPlan({.kind = BimPrimitivePassKind::Curves,
                                 .enabled = true,
                                 .nativeDraws = opaqueAggregate(commands)});
  EXPECT_TRUE(native.active);
  EXPECT_TRUE(native.useObjectColor);
  EXPECT_FALSE(native.gpuCompaction);
  const auto placeholder = buildBimPrimitivePassPlan(
      {.kind = BimPrimitivePassKind::Curves,
       .enabled = true,
       .placeholderRangePreviewEnabled = true,
       .placeholderDraws = opaqueAggregate(commands)});
  EXPECT_TRUE(placeholder.active);
  EXPECT_FALSE(placeholder.useObjectColor);
}

TEST(BimPrimitivePassPlannerTests, NativeGpuVisibilityMapsCurveSlots) {
  const auto nativeCommands = drawCommands(6u);

  const auto plan = buildBimPrimitivePassPlan(
      {.kind = BimPrimitivePassKind::Curves,
       .enabled = true,
       .nativeDrawsUseGpuVisibility = true,
       .nativeDraws = opaqueAggregate(nativeCommands)});

  ASSERT_TRUE(plan.gpuCompaction);
  EXPECT_TRUE(plan.useObjectColor);
  ASSERT_EQ(plan.gpuSlotCount, 2u);
  EXPECT_TRUE(plan.cpuDrawSources.empty());
  EXPECT_EQ(plan.gpuSlots[0], BimDrawCompactionSlot::NativeCurveOpaque);
  EXPECT_EQ(plan.gpuSlots[1], BimDrawCompactionSlot::NativeCurveTransparent);
}

TEST(BimPrimitivePassPlannerTests, SanitizesOpacityAndPrimitiveSize) {
  const auto commands = drawCommands(7u);

  const auto plan = buildBimPrimitivePassPlan(
      {.enabled = true,
       .placeholderRangePreviewEnabled = true,
       .opacity = 1.5f,
       .primitiveSize = 0.25f,
       .placeholderDraws = opaqueAggregate(commands)});

  EXPECT_FLOAT_EQ(plan.opacity, 1.0f);
  EXPECT_FLOAT_EQ(plan.primitiveSize, 1.0f);
}

TEST(BimPrimitivePassPlannerTests, CpuSourcesPreferAggregateOverSplitLists) {
  const auto aggregateCommands = drawCommands(8u);
  const auto splitCommands = drawCommands(9u);

  const auto plan =
      buildBimPrimitivePassPlan({.enabled = true,
                                 .placeholderRangePreviewEnabled = true,
                                 .placeholderDraws = opaqueAggregateAndSplit(
                                     aggregateCommands, splitCommands)});

  ASSERT_EQ(plan.cpuDrawSources.size(), 1u);
  EXPECT_EQ(plan.cpuDrawSources[0], &aggregateCommands);
}

TEST(BimPrimitivePassPlannerTests, CpuSourcesUseSplitsWhenAggregateIsEmpty) {
  const auto singleSidedCommands = drawCommands(10u);
  const auto windingFlippedCommands = drawCommands(11u);

  const auto plan = buildBimPrimitivePassPlan(
      {.enabled = true,
       .placeholderRangePreviewEnabled = true,
       .placeholderDraws =
           opaqueSplits(singleSidedCommands, windingFlippedCommands)});

  ASSERT_EQ(plan.cpuDrawSources.size(), 2u);
  EXPECT_EQ(plan.cpuDrawSources[0], &singleSidedCommands);
  EXPECT_EQ(plan.cpuDrawSources[1], &windingFlippedCommands);
}

TEST(BimPrimitivePassPlannerTests, DetectsAggregateAndSplitDrawCommands) {
  const auto aggregateCommands = drawCommands(12u);
  const auto splitCommands = drawCommands(13u);

  EXPECT_TRUE(
      hasBimPrimitivePassDrawCommands(opaqueAggregate(aggregateCommands)));
  EXPECT_TRUE(hasBimPrimitivePassDrawCommands(transparentSplit(splitCommands)));
  EXPECT_FALSE(hasBimPrimitivePassDrawCommands({}));
}

TEST(BimPrimitivePassPlannerTests,
     FramePrimitivePassInputsBuildPlannerInputsAndGeometryReadiness) {
  const auto placeholderCommands = drawCommands(14u);
  const auto nativeCommands = drawCommands(15u);
  const BimPrimitivePassGeometryBinding geometry{
      .sceneDescriptorSet = fakeHandle<VkDescriptorSet>(0x1),
      .vertexSlice = {.buffer = fakeHandle<VkBuffer>(0x2)},
      .indexSlice = {.buffer = fakeHandle<VkBuffer>(0x3)}};
  const BimPrimitiveFramePassRecordInputs inputs{
      .style = {.kind = BimPrimitivePassKind::Curves,
                .enabled = true,
                .depthTest = false,
                .placeholderRangePreviewEnabled = true,
                .nativeDrawsUseGpuVisibility = true,
                .opacity = 0.45f,
                .primitiveSize = 2.5f},
      .placeholderDraws = opaqueAggregate(placeholderCommands),
      .nativeDraws = transparentSplit(nativeCommands),
      .geometry = geometry};

  const BimPrimitivePassPlanInputs planInputs =
      buildBimPrimitiveFramePassPlanInputs(inputs);

  EXPECT_TRUE(hasBimPrimitiveFramePassGeometry(geometry));
  EXPECT_EQ(planInputs.kind, BimPrimitivePassKind::Curves);
  EXPECT_TRUE(planInputs.enabled);
  EXPECT_FALSE(planInputs.depthTest);
  EXPECT_TRUE(planInputs.placeholderRangePreviewEnabled);
  EXPECT_TRUE(planInputs.nativeDrawsUseGpuVisibility);
  EXPECT_FLOAT_EQ(planInputs.opacity, 0.45f);
  EXPECT_FLOAT_EQ(planInputs.primitiveSize, 2.5f);
  EXPECT_EQ(planInputs.placeholderDraws.opaqueDrawCommands,
            &placeholderCommands);
  EXPECT_EQ(planInputs.nativeDraws.transparentSingleSidedDrawCommands,
            &nativeCommands);
}

TEST(BimPrimitivePassPlannerTests,
     FramePrimitivePassRejectsNullCommandBufferBeforeRecording) {
  const auto commands = drawCommands(16u);
  const BimPrimitiveFramePassRecordInputs inputs{
      .style = {.enabled = true, .placeholderRangePreviewEnabled = true},
      .placeholderDraws = opaqueAggregate(commands)};

  EXPECT_FALSE(recordBimPrimitiveFramePassCommands(VK_NULL_HANDLE, inputs));
}

} // namespace
