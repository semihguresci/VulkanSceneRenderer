#include "Container/renderer/shadow/ShadowCullPassPlanner.h"

#include <gtest/gtest.h>

namespace {

using container::renderer::RenderPassSkipReason;
using container::renderer::RenderResourceId;
using container::renderer::ShadowCullPassPlanInputs;
using container::renderer::buildShadowCullPassPlan;

ShadowCullPassPlanInputs readyInputs() {
  return {.shadowAtlasVisible = true,
          .gpuShadowCullEnabled = true,
          .shadowCullManagerReady = true,
          .sceneSingleSidedDrawsAvailable = true,
          .sourceDrawCommandsAllSingleInstance = true,
          .cameraBufferReady = true,
          .cascadeIndexInRange = true,
          .sourceDrawCount = 16u};
}

} // namespace

TEST(ShadowCullPassPlannerTests, HiddenAtlasReturnsNotNeeded) {
  auto inputs = readyInputs();
  inputs.shadowAtlasVisible = false;

  const auto plan = buildShadowCullPassPlan(inputs);

  EXPECT_FALSE(plan.active);
  EXPECT_FALSE(plan.readiness.ready);
  EXPECT_EQ(plan.readiness.skipReason, RenderPassSkipReason::NotNeeded);
}

TEST(ShadowCullPassPlannerTests, DisabledGpuCullReturnsNotNeeded) {
  auto inputs = readyInputs();
  inputs.gpuShadowCullEnabled = false;

  const auto plan = buildShadowCullPassPlan(inputs);

  EXPECT_FALSE(plan.active);
  EXPECT_FALSE(plan.readiness.ready);
  EXPECT_EQ(plan.readiness.skipReason, RenderPassSkipReason::NotNeeded);
}

TEST(ShadowCullPassPlannerTests, UnreadyManagerReturnsNotNeeded) {
  auto inputs = readyInputs();
  inputs.shadowCullManagerReady = false;

  const auto plan = buildShadowCullPassPlan(inputs);

  EXPECT_FALSE(plan.active);
  EXPECT_FALSE(plan.readiness.ready);
  EXPECT_EQ(plan.readiness.skipReason, RenderPassSkipReason::NotNeeded);
}

TEST(ShadowCullPassPlannerTests, MissingOrEmptyDrawsReturnNotNeeded) {
  auto inputs = readyInputs();
  inputs.sceneSingleSidedDrawsAvailable = false;
  EXPECT_EQ(buildShadowCullPassPlan(inputs).readiness.skipReason,
            RenderPassSkipReason::NotNeeded);

  inputs = readyInputs();
  inputs.sourceDrawCount = 0u;
  EXPECT_EQ(buildShadowCullPassPlan(inputs).readiness.skipReason,
            RenderPassSkipReason::NotNeeded);
}

TEST(ShadowCullPassPlannerTests, MultiInstanceSourceDrawsReturnNotNeeded) {
  auto inputs = readyInputs();
  inputs.sourceDrawCommandsAllSingleInstance = false;

  const auto plan = buildShadowCullPassPlan(inputs);

  EXPECT_FALSE(plan.active);
  EXPECT_FALSE(plan.readiness.ready);
  EXPECT_EQ(plan.readiness.skipReason, RenderPassSkipReason::NotNeeded);
}

TEST(ShadowCullPassPlannerTests, MissingCameraBufferReturnsMissingCamera) {
  auto inputs = readyInputs();
  inputs.cameraBufferReady = false;

  const auto plan = buildShadowCullPassPlan(inputs);

  EXPECT_FALSE(plan.active);
  EXPECT_FALSE(plan.readiness.ready);
  EXPECT_EQ(plan.readiness.skipReason, RenderPassSkipReason::MissingResource);
  EXPECT_EQ(plan.readiness.blockingResource, RenderResourceId::CameraBuffer);
}

TEST(ShadowCullPassPlannerTests, OutOfRangeCascadeReturnsNotNeeded) {
  auto inputs = readyInputs();
  inputs.cascadeIndexInRange = false;

  const auto plan = buildShadowCullPassPlan(inputs);

  EXPECT_FALSE(plan.active);
  EXPECT_FALSE(plan.readiness.ready);
  EXPECT_EQ(plan.readiness.skipReason, RenderPassSkipReason::NotNeeded);
}

TEST(ShadowCullPassPlannerTests, ReadyInputsProduceActivePlan) {
  const auto plan = buildShadowCullPassPlan(readyInputs());

  EXPECT_TRUE(plan.active);
  EXPECT_TRUE(plan.readiness.ready);
  EXPECT_EQ(plan.readiness.skipReason, RenderPassSkipReason::None);
  EXPECT_EQ(plan.drawCount, readyInputs().sourceDrawCount);
}
