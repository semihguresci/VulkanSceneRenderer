#include "Container/renderer/deferred/DeferredPointLightingDrawPlanner.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

using container::renderer::DeferredPointLightingDrawInputs;
using container::renderer::DeferredPointLightingPath;
using container::renderer::DeferredPointLightingStencilPipeline;
using container::renderer::DeferredPointLightingState;
using container::renderer::buildDeferredPointLightingDrawPlan;

[[nodiscard]] container::gpu::PointLightData light(float seed) {
  return {.positionRadius = {seed, seed + 1.0f, seed + 2.0f, seed + 3.0f},
          .colorIntensity = {seed + 4.0f, seed + 5.0f, seed + 6.0f,
                             seed + 7.0f},
          .directionInnerCos = {seed + 8.0f, seed + 9.0f, seed + 10.0f,
                                seed + 11.0f},
          .coneOuterCosType = {seed + 12.0f, seed + 13.0f, seed + 14.0f,
                               seed + 15.0f}};
}

void expectLightEq(const container::gpu::PointLightData &actual,
                   const container::gpu::PointLightData &expected) {
  EXPECT_EQ(actual.positionRadius, expected.positionRadius);
  EXPECT_EQ(actual.colorIntensity, expected.colorIntensity);
  EXPECT_EQ(actual.directionInnerCos, expected.directionInnerCos);
  EXPECT_EQ(actual.coneOuterCosType, expected.coneOuterCosType);
}

TEST(DeferredPointLightingDrawPlannerTests,
     TiledPathBuildsPushConstantsAndClampsTileCounts) {
  const auto zeroPlan = buildDeferredPointLightingDrawPlan(
      {.state = {.path = DeferredPointLightingPath::Tiled},
       .framebufferWidth = 0u,
       .framebufferHeight = 0u,
       .cameraNear = 0.25f,
       .cameraFar = 250.0f,
       .contactVisibilityEnabled = 1u,
       .localShadowEnabled = 1u});
  EXPECT_EQ(zeroPlan.path, DeferredPointLightingPath::Tiled);
  EXPECT_EQ(zeroPlan.tiledPushConstants.tileCountX, 1u);
  EXPECT_EQ(zeroPlan.tiledPushConstants.tileCountY, 1u);
  EXPECT_EQ(zeroPlan.tiledPushConstants.depthSliceCount,
            container::gpu::kClusterDepthSlices);
  EXPECT_EQ(zeroPlan.tiledPushConstants.contactVisibilityEnabled, 1u);
  EXPECT_EQ(zeroPlan.tiledPushConstants.localShadowEnabled, 1u);
  EXPECT_FLOAT_EQ(zeroPlan.tiledPushConstants.cameraNear, 0.25f);
  EXPECT_FLOAT_EQ(zeroPlan.tiledPushConstants.cameraFar, 250.0f);

  const auto oneTilePlan = buildDeferredPointLightingDrawPlan(
      {.state = {.path = DeferredPointLightingPath::Tiled},
       .framebufferWidth = container::gpu::kTileSize,
       .framebufferHeight = container::gpu::kTileSize});
  EXPECT_EQ(oneTilePlan.tiledPushConstants.tileCountX, 1u);
  EXPECT_EQ(oneTilePlan.tiledPushConstants.tileCountY, 1u);

  const auto roundedPlan = buildDeferredPointLightingDrawPlan(
      {.state = {.path = DeferredPointLightingPath::Tiled},
       .framebufferWidth = container::gpu::kTileSize + 1u,
       .framebufferHeight = container::gpu::kTileSize * 2u + 1u});
  EXPECT_EQ(roundedPlan.tiledPushConstants.tileCountX, 2u);
  EXPECT_EQ(roundedPlan.tiledPushConstants.tileCountY, 3u);
}

TEST(DeferredPointLightingDrawPlannerTests,
     ContactVisibilityTogglePropagatesToLocalLightingPlans) {
  const auto tiledPlan = buildDeferredPointLightingDrawPlan(
      {.state = {.path = DeferredPointLightingPath::Tiled},
       .contactVisibilityEnabled = 1u,
       .localShadowEnabled = 1u});
  EXPECT_EQ(tiledPlan.contactVisibilityEnabled, 1u);
  EXPECT_EQ(tiledPlan.localShadowEnabled, 1u);
  EXPECT_EQ(tiledPlan.tiledPushConstants.contactVisibilityEnabled, 1u);
  EXPECT_EQ(tiledPlan.tiledPushConstants.localShadowEnabled, 1u);

  const std::vector<container::gpu::PointLightData> lights = {light(2.0f)};
  const auto stencilPlan = buildDeferredPointLightingDrawPlan(
      {.state = {.path = DeferredPointLightingPath::Stencil,
                 .stencilLightCount = 1u},
       .contactVisibilityEnabled = 1u,
       .localShadowEnabled = 1u,
       .pointLights = lights});
  EXPECT_EQ(stencilPlan.contactVisibilityEnabled, 1u);
  EXPECT_EQ(stencilPlan.localShadowEnabled, 1u);
  EXPECT_EQ(stencilPlan.stencilRouteCount, 1u);
}

TEST(DeferredPointLightingDrawPlannerTests, NonePathProducesNoRoutes) {
  const auto plan = buildDeferredPointLightingDrawPlan(
      {.state = {.path = DeferredPointLightingPath::None}});

  EXPECT_EQ(plan.path, DeferredPointLightingPath::None);
  EXPECT_EQ(plan.stencilRouteCount, 0u);
}

TEST(DeferredPointLightingDrawPlannerTests,
     StencilPathCopiesLightFieldsAndKeepsCount) {
  const std::vector<container::gpu::PointLightData> lights = {light(1.0f),
                                                             light(20.0f)};
  const auto plan = buildDeferredPointLightingDrawPlan(
      {.state = {.path = DeferredPointLightingPath::Stencil,
                 .stencilLightCount = 2u},
       .pointLights = lights,
       .lightVolumeIndexCount = 36u});

  EXPECT_EQ(plan.path, DeferredPointLightingPath::Stencil);
  EXPECT_EQ(plan.stencilRouteCount, 2u);
  EXPECT_EQ(plan.lightVolumeIndexCount, 36u);
  expectLightEq(plan.stencilRoutes[0].light, lights[0]);
  expectLightEq(plan.stencilRoutes[1].light, lights[1]);
}

TEST(DeferredPointLightingDrawPlannerTests,
     StencilPathClampsToAvailableLightsAndMaxDeferredLights) {
  std::vector<container::gpu::PointLightData> lights;
  lights.reserve(container::gpu::kMaxDeferredPointLights + 2u);
  for (uint32_t index = 0u;
       index < container::gpu::kMaxDeferredPointLights + 2u; ++index) {
    lights.push_back(light(static_cast<float>(index)));
  }

  const auto maxPlan = buildDeferredPointLightingDrawPlan(
      {.state = {.path = DeferredPointLightingPath::Stencil,
                 .stencilLightCount =
                     container::gpu::kMaxDeferredPointLights + 2u},
       .pointLights = lights});
  EXPECT_EQ(maxPlan.stencilRouteCount,
            container::gpu::kMaxDeferredPointLights);

  lights.resize(3u);
  const auto availablePlan = buildDeferredPointLightingDrawPlan(
      {.state = {.path = DeferredPointLightingPath::Stencil,
                 .stencilLightCount = 6u},
       .pointLights = lights});
  EXPECT_EQ(availablePlan.stencilRouteCount, 3u);
}

TEST(DeferredPointLightingDrawPlannerTests,
     DebugStencilFlagSelectsDebugPipeline) {
  const auto normalPlan = buildDeferredPointLightingDrawPlan(
      {.state = {.path = DeferredPointLightingPath::Stencil}});
  EXPECT_EQ(normalPlan.stencilPipeline,
            DeferredPointLightingStencilPipeline::PointLight);

  const auto debugPlan = buildDeferredPointLightingDrawPlan(
      {.state = {.path = DeferredPointLightingPath::Stencil},
       .debugVisualizePointLightStencil = true});
  EXPECT_EQ(debugPlan.stencilPipeline,
            DeferredPointLightingStencilPipeline::PointLightStencilDebug);
}

TEST(DeferredPointLightingDrawPlannerTests,
     FullscreenFallbackKeepsLightOrderAndUnboundedRangeWithoutVolume) {
  std::vector<container::gpu::PointLightData> lights;
  for (uint32_t i = 0; i < container::gpu::kMaxDeferredPointLights + 1u; ++i)
    lights.push_back(light(static_cast<float>(i)));
  lights.front().positionRadius.w = 0.0f;

  const auto plan = buildDeferredPointLightingDrawPlan(
      {.state = {.path = DeferredPointLightingPath::FullscreenFallback,
                 .stencilLightCount = static_cast<uint32_t>(lights.size())},
       .debugVisualizePointLightStencil = true,
       .pointLights = lights,
       .lightVolumeIndexCount = 36u});

  EXPECT_EQ(plan.path, DeferredPointLightingPath::FullscreenFallback);
  EXPECT_EQ(plan.stencilPipeline, DeferredPointLightingStencilPipeline::PointLight);
  EXPECT_EQ(plan.lightVolumeIndexCount, 0u);
  ASSERT_EQ(plan.stencilRouteCount, container::gpu::kMaxDeferredPointLights);
  for (uint32_t i = 0; i < plan.stencilRouteCount; ++i)
    expectLightEq(plan.stencilRoutes[i].light, lights[i]);
}

} // namespace
