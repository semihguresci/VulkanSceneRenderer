#include "Container/renderer/bim/BimLightingOverlayRecorder.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <vector>

namespace {

using container::renderer::BimLightingOverlayDrawPlan;
using container::renderer::BimLightingOverlayFrameRecordInputs;
using container::renderer::BimLightingOverlayPipeline;
using container::renderer::BimLightingOverlayRecordInputs;
using container::renderer::BimLightingOverlayStylePlan;
using container::renderer::BimLightingSelectionOutlinePlan;
using container::renderer::buildBimLightingOverlayFramePlan;
using container::renderer::buildBimLightingOverlayFramePlanInputs;
using container::renderer::DebugOverlayRenderer;
using container::renderer::DrawCommand;
using container::renderer::recordBimLightingOverlayCommands;
using container::renderer::recordBimLightingOverlayFrameCommands;
using container::renderer::WireframePushConstants;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

std::vector<DrawCommand> oneDrawCommand() {
  return {DrawCommand{.objectIndex = 7u, .firstIndex = 0u, .indexCount = 3u}};
}

BimLightingOverlayRecordInputs
requiredInputs(const container::renderer::BimLightingOverlayPlan &plan,
               const WireframePushConstants &pushConstants,
               const DebugOverlayRenderer &debugOverlay) {
  return {.plan = &plan,
          .wireframeLayout = fakeHandle<VkPipelineLayout>(0x1),
          .wireframePushConstants = &pushConstants,
          .debugOverlay = &debugOverlay};
}

} // namespace

TEST(BimLightingOverlayRecorderTests, NullOrInactivePlanReturnsFalse) {
  EXPECT_FALSE(recordBimLightingOverlayCommands(VK_NULL_HANDLE, {}));

  container::renderer::BimLightingOverlayPlan plan{};
  WireframePushConstants pushConstants{};
  DebugOverlayRenderer debugOverlay{};
  const BimLightingOverlayRecordInputs inputs =
      requiredInputs(plan, pushConstants, debugOverlay);

  EXPECT_FALSE(recordBimLightingOverlayCommands(VK_NULL_HANDLE, inputs));
}

TEST(BimLightingOverlayRecorderTests, StrokeKernelFillsTheEntireDiskForThinFeatures) {
  using namespace container::renderer;
  std::set<std::pair<int, int>> samples;
  for (const auto offset : kSelectionStrokeOffsets)
    EXPECT_TRUE(samples.emplace(offset.x, offset.y).second);
  // An isolated selected pixel must have every neighbouring pixel within the
  // stroke radius filled. Boundary-only translations leave holes here.
  for (int y = -6; y <= 6; ++y)
    for (int x = -6; x <= 6; ++x)
      EXPECT_EQ(samples.contains({x, y}), (x || y) && x*x + y*y <= 25)
          << "offset " << x << ", " << y;
  EXPECT_EQ(samples.size(), kSelectionStrokeSampleCount);
}

TEST(BimLightingOverlayRecorderTests, StrokeInstancesKeepNonzeroBaseAndWholeObjectGroups) {
  using namespace container::renderer;
  const auto batch = PlanSelectionStrokeInstanceBatch(19, 3);
  EXPECT_EQ(batch.objectIndex, 19u);
  EXPECT_EQ(batch.objectCount, 3u);
  EXPECT_EQ(batch.instanceCount, 240u);
  // Last sample of one object and first sample of the next must never read
  // the unrelated object rows between expanded instance ordinals.
  EXPECT_EQ(batch.objectIndex + 79 / kSelectionStrokeSampleCount, 19u);
  EXPECT_EQ(batch.objectIndex + 80 / kSelectionStrokeSampleCount, 20u);
  EXPECT_EQ(batch.objectIndex + 239 / kSelectionStrokeSampleCount, 21u);
  EXPECT_EQ(PlanSelectionStrokeInstanceBatch(19, 0).instanceCount, 80u);
}

TEST(BimLightingOverlayRecorderTests, StrokeInstanceOverflowSplitsOnlyAtObjectBoundaries) {
  using namespace container::renderer;
  constexpr auto maximum = std::numeric_limits<uint32_t>::max();
  const auto batch = PlanSelectionStrokeInstanceBatch(0, maximum);
  EXPECT_EQ(batch.objectCount, maximum / kSelectionStrokeSampleCount);
  EXPECT_EQ(batch.instanceCount % kSelectionStrokeSampleCount, 0u);
  EXPECT_LE(uint64_t(batch.instanceCount), uint64_t(maximum));
  EXPECT_LT(uint64_t(maximum), uint64_t(batch.instanceCount) + kSelectionStrokeSampleCount);
  const auto next = PlanSelectionStrokeInstanceBatch(batch.objectCount,
      maximum - batch.objectCount);
  EXPECT_EQ(next.objectIndex, batch.objectCount);
  EXPECT_EQ(PlanSelectionStrokeInstanceBatch(maximum, 1).objectIndex, maximum);
  EXPECT_THROW(PlanSelectionStrokeInstanceBatch(maximum, 2), std::invalid_argument);
  EXPECT_THROW(PlanSelectionStrokeInstanceBatch(10, maximum), std::invalid_argument);
}

TEST(BimLightingOverlayRecorderTests,
     MissingWireframeLayoutPushConstantsOrDebugOverlayReturnsFalse) {
  container::renderer::BimLightingOverlayPlan plan{};
  plan.floorPlan.active = true;

  WireframePushConstants pushConstants{};
  DebugOverlayRenderer debugOverlay{};

  BimLightingOverlayRecordInputs inputs =
      requiredInputs(plan, pushConstants, debugOverlay);
  inputs.wireframeLayout = VK_NULL_HANDLE;
  EXPECT_FALSE(recordBimLightingOverlayCommands(VK_NULL_HANDLE, inputs));

  inputs = requiredInputs(plan, pushConstants, debugOverlay);
  inputs.wireframePushConstants = nullptr;
  EXPECT_FALSE(recordBimLightingOverlayCommands(VK_NULL_HANDLE, inputs));

  inputs = requiredInputs(plan, pushConstants, debugOverlay);
  inputs.debugOverlay = nullptr;
  EXPECT_FALSE(recordBimLightingOverlayCommands(VK_NULL_HANDLE, inputs));
}

TEST(BimLightingOverlayRecorderTests,
     ActiveStylePlanWithMissingBimGeometryReturnsFalse) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  container::renderer::BimLightingOverlayPlan plan{};
  BimLightingOverlayStylePlan &style = plan.pointStyle;
  style.active = true;
  style.routeCount = 1u;
  style.routes[0].commands = &commands;

  WireframePushConstants pushConstants{};
  DebugOverlayRenderer debugOverlay{};
  const BimLightingOverlayRecordInputs inputs =
      requiredInputs(plan, pushConstants, debugOverlay);

  EXPECT_FALSE(recordBimLightingOverlayCommands(VK_NULL_HANDLE, inputs));
}

TEST(BimLightingOverlayRecorderTests,
     ActiveSceneOverlayWithMissingSceneGeometryReturnsFalse) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  container::renderer::BimLightingOverlayPlan plan{};
  BimLightingOverlayDrawPlan &sceneHover = plan.sceneHover;
  sceneHover.active = true;
  sceneHover.pipeline = BimLightingOverlayPipeline::WireframeDepth;
  sceneHover.commands = &commands;

  WireframePushConstants pushConstants{};
  DebugOverlayRenderer debugOverlay{};
  BimLightingOverlayRecordInputs inputs =
      requiredInputs(plan, pushConstants, debugOverlay);
  inputs.pipelines.wireframeDepth = fakeHandle<VkPipeline>(0x2);

  EXPECT_FALSE(recordBimLightingOverlayCommands(VK_NULL_HANDLE, inputs));
}

TEST(BimLightingOverlayRecorderTests,
     SelectionOutlineRequiresMaskAndOutlinePipelinesBeforeRecording) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  container::renderer::BimLightingOverlayPlan plan{};
  BimLightingSelectionOutlinePlan &selection = plan.sceneSelectionOutline;
  selection.active = true;
  selection.commands = &commands;

  WireframePushConstants pushConstants{};
  DebugOverlayRenderer debugOverlay{};
  BimLightingOverlayRecordInputs inputs =
      requiredInputs(plan, pushConstants, debugOverlay);
  EXPECT_FALSE(recordBimLightingOverlayCommands(VK_NULL_HANDLE, inputs));

  inputs.pipelines.selectionMask = fakeHandle<VkPipeline>(0x3);
  EXPECT_FALSE(recordBimLightingOverlayCommands(VK_NULL_HANDLE, inputs));
}

TEST(BimLightingOverlayRecorderTests,
     FrameOverlayInputsBuildPlanAndPreserveRecordingHandles) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  const std::vector<DrawCommand> cutPlaneCommands = oneDrawCommand();
  WireframePushConstants pushConstants{};
  DebugOverlayRenderer debugOverlay{};
  const BimLightingOverlayFrameRecordInputs inputs{
      .bimGeometryReady = true,
      .floorPlan = {.enabled = true, .depthTest = true, .lineWidth = 2.0f},
      .sectionPlaneVisual =
          {.enabled = true, .depthTest = true, .lineWidth = 3.0f},
      .draws = {.floorPlan = &commands,
                .sectionPlaneVisual = &cutPlaneCommands},
      .sectionPlaneVisualGeometryReady = true,
      .pipelines = {.wireframeDepth = fakeHandle<VkPipeline>(0x6),
                    .bimFloorPlanDepth = fakeHandle<VkPipeline>(0x4)},
      .wireframeLayout = fakeHandle<VkPipelineLayout>(0x5),
      .wireframePushConstants = &pushConstants,
      .debugOverlay = &debugOverlay};

  const auto planInputs = buildBimLightingOverlayFramePlanInputs(inputs);
  const auto plan = buildBimLightingOverlayFramePlan(inputs);

  EXPECT_TRUE(planInputs.wireframeLayoutReady);
  EXPECT_TRUE(planInputs.wireframePushConstantsReady);
  EXPECT_TRUE(planInputs.pipelines.wireframeDepth);
  EXPECT_TRUE(planInputs.pipelines.bimFloorPlanDepth);
  EXPECT_TRUE(plan.floorPlan.active);
  EXPECT_EQ(plan.floorPlan.commands, &commands);
  ASSERT_TRUE(plan.sectionPlaneVisual.active);
  EXPECT_EQ(plan.sectionPlaneVisual.commands, &cutPlaneCommands);
  EXPECT_EQ(inputs.pipelines.bimFloorPlanDepth, fakeHandle<VkPipeline>(0x4));
  EXPECT_EQ(inputs.wireframePushConstants, &pushConstants);
  EXPECT_EQ(inputs.debugOverlay, &debugOverlay);
}

TEST(BimLightingOverlayRecorderTests,
     FrameOverlayRejectsNullCommandBufferBeforeRecording) {
  const BimLightingOverlayFrameRecordInputs inputs{
      .wireframeLayout = fakeHandle<VkPipelineLayout>(0x1)};

  EXPECT_FALSE(recordBimLightingOverlayFrameCommands(VK_NULL_HANDLE, inputs));
}
