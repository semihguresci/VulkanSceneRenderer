#include "Container/renderer/shadow/ShadowCascadeDepthPlanner.h"

#include <gtest/gtest.h>

namespace {

using container::renderer::ShadowCascadeDepthPlanInputs;
using container::renderer::buildShadowCascadeDepthPlan;
using container::renderer::expandShadowCascadeRadiusForFilterGuard;

TEST(ShadowCascadeDepthPlannerTests,
     FiniteCasterBoundsTightenReceiverDepthExtension) {
  ShadowCascadeDepthPlanInputs inputs{};
  inputs.receiverMinBounds = {-12.0f, -12.0f, -20.0f};
  inputs.receiverMaxBounds = {12.0f, 12.0f, -10.0f};
  inputs.casterMinBounds = {-12.0f, -12.0f, -18.0f};
  inputs.casterMaxBounds = {12.0f, 12.0f, -2.0f};
  inputs.hasFiniteCasterBounds = true;
  inputs.texelSize = 0.5f;
  inputs.lightDistance = 24.0f;
  inputs.fallbackCasterDepth = 40.0f;

  const auto plan = buildShadowCascadeDepthPlan(inputs);

  EXPECT_FLOAT_EQ(plan.lightDistance, 24.0f);
  EXPECT_FLOAT_EQ(plan.receiverMinBounds.z, -20.0f);
  EXPECT_FLOAT_EQ(plan.receiverMaxBounds.z, -10.0f);
  EXPECT_FLOAT_EQ(plan.casterMinBounds.z, -20.0f);
  EXPECT_FLOAT_EQ(plan.casterMaxBounds.z, -1.0f);
  EXPECT_FLOAT_EQ(plan.nearPlane, 1.0f);
  EXPECT_FLOAT_EQ(plan.farPlane, 20.0f);
}

TEST(ShadowCascadeDepthPlannerTests,
     CastersInFrontOfLightNearPlaneMoveLightBack) {
  ShadowCascadeDepthPlanInputs inputs{};
  inputs.receiverMinBounds = {-8.0f, -8.0f, -10.0f};
  inputs.receiverMaxBounds = {8.0f, 8.0f, -5.0f};
  inputs.casterMinBounds = {-8.0f, -8.0f, -8.0f};
  inputs.casterMaxBounds = {8.0f, 8.0f, 3.0f};
  inputs.hasFiniteCasterBounds = true;
  inputs.texelSize = 0.25f;
  inputs.lightDistance = 10.0f;
  inputs.fallbackCasterDepth = 20.0f;

  const auto plan = buildShadowCascadeDepthPlan(inputs);

  EXPECT_FLOAT_EQ(plan.lightDistanceIncrease, 3.51f);
  EXPECT_FLOAT_EQ(plan.lightDistance, 13.51f);
  EXPECT_NEAR(plan.casterMaxBounds.z, -0.01f, 1.0e-5f);
  EXPECT_FLOAT_EQ(plan.receiverMinBounds.z, -13.51f);
  EXPECT_FLOAT_EQ(plan.receiverMaxBounds.z, -8.51f);
  EXPECT_FLOAT_EQ(plan.nearPlane, 0.01f);
  EXPECT_FLOAT_EQ(plan.farPlane, 13.51f);
}

TEST(ShadowCascadeDepthPlannerTests,
     MissingFiniteCasterBoundsUsesFallbackCasterDepth) {
  ShadowCascadeDepthPlanInputs inputs{};
  inputs.receiverMinBounds = {-8.0f, -8.0f, -40.0f};
  inputs.receiverMaxBounds = {8.0f, 8.0f, -30.0f};
  inputs.hasFiniteCasterBounds = false;
  inputs.texelSize = 0.25f;
  inputs.lightDistance = 60.0f;
  inputs.fallbackCasterDepth = 20.0f;

  const auto plan = buildShadowCascadeDepthPlan(inputs);

  EXPECT_FLOAT_EQ(plan.lightDistanceIncrease, 0.0f);
  EXPECT_FLOAT_EQ(plan.lightDistance, 60.0f);
  EXPECT_FLOAT_EQ(plan.receiverMinBounds.z, -40.0f);
  EXPECT_FLOAT_EQ(plan.receiverMaxBounds.z, -30.0f);
  EXPECT_FLOAT_EQ(plan.casterMinBounds.z, -40.0f);
  EXPECT_FLOAT_EQ(plan.casterMaxBounds.z, -9.5f);
  EXPECT_FLOAT_EQ(plan.nearPlane, 9.5f);
  EXPECT_FLOAT_EQ(plan.farPlane, 40.0f);
}

TEST(ShadowCascadeDepthPlannerTests,
     FilterGuardExpandsCascadeRadiusByAtLeastTexelMargin) {
  const float receiverRadius = 100.0f;
  const uint32_t resolution = 1000u;
  const float guardTexels = 10.0f;

  const float guardedRadius = expandShadowCascadeRadiusForFilterGuard(
      receiverRadius, resolution, guardTexels);
  const float guardedTexelSize =
      (guardedRadius * 2.0f) / static_cast<float>(resolution);

  EXPECT_GT(guardedRadius, receiverRadius);
  const float expandedTexels =
      (guardedRadius - receiverRadius) / guardedTexelSize;
  EXPECT_GE(expandedTexels, guardTexels);
  EXPECT_LT(expandedTexels, guardTexels + 8.0f);
}

TEST(ShadowCascadeDepthPlannerTests,
     FilterGuardStabilizesSubTexelRadiusNoise) {
  const uint32_t resolution = 4096u;
  const float guardTexels = 8.0f;

  const float baseRadius =
      expandShadowCascadeRadiusForFilterGuard(100.0f, resolution, guardTexels);
  const float jitteredRadius = expandShadowCascadeRadiusForFilterGuard(
      100.02f, resolution, guardTexels);

  EXPECT_FLOAT_EQ(jitteredRadius, baseRadius);
}

TEST(ShadowCascadeDepthPlannerTests,
     FilterGuardStillTracksRealRadiusGrowth) {
  const uint32_t resolution = 4096u;
  const float guardTexels = 8.0f;

  const float baseRadius =
      expandShadowCascadeRadiusForFilterGuard(100.0f, resolution, guardTexels);
  const float largerRadius =
      expandShadowCascadeRadiusForFilterGuard(101.0f, resolution, guardTexels);

  EXPECT_GT(largerRadius, baseRadius);
}

TEST(ShadowCascadeDepthPlannerTests,
     FilterGuardLeavesRadiusUnchangedWhenGuardIsDisabled) {
  EXPECT_FLOAT_EQ(expandShadowCascadeRadiusForFilterGuard(42.0f, 4096u, 0.0f),
                  42.0f);
}

}  // namespace
