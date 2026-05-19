#include "Container/renderer/deferred/DeferredRasterSceneGpuCullRoutePlanner.h"

#include "Container/renderer/scene/DrawCommand.h"
#include "Container/renderer/scene/SceneRasterPassPlanner.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

using container::renderer::DeferredRasterSceneGpuCullRoutePlanInputs;
using container::renderer::DrawCommand;
using container::renderer::SceneOpaqueIndirectSource;
using container::renderer::SceneRasterPassKind;
using container::renderer::buildDeferredRasterSceneGpuCullRoutePlan;
using container::renderer::buildSceneRasterPassPlan;

std::vector<DrawCommand> singleSidedDraws() {
  return {{.objectIndex = 5u, .firstIndex = 0u, .indexCount = 3u}};
}

DeferredRasterSceneGpuCullRoutePlanInputs readyGBufferRouteInputs() {
  return {.kind = SceneRasterPassKind::GBuffer,
          .gpuCullManagerReady = true,
          .frustumCullActive = true,
          .frustumDrawsValid = true,
          .occlusionCullActive = true,
          .occlusionDrawsValid = true};
}

} // namespace

TEST(DeferredRasterSceneGpuCullRoutePlannerTests,
     FrustumRouteRequiresReadyManagerActivePassAndValidOutput) {
  auto inputs = readyGBufferRouteInputs();

  EXPECT_TRUE(buildDeferredRasterSceneGpuCullRoutePlan(inputs)
                  .gpuIndirectAvailable);

  inputs.gpuCullManagerReady = false;
  EXPECT_FALSE(buildDeferredRasterSceneGpuCullRoutePlan(inputs)
                   .gpuIndirectAvailable);

  inputs = readyGBufferRouteInputs();
  inputs.frustumCullActive = false;
  EXPECT_FALSE(buildDeferredRasterSceneGpuCullRoutePlan(inputs)
                   .gpuIndirectAvailable);

  inputs = readyGBufferRouteInputs();
  inputs.frustumDrawsValid = false;
  EXPECT_FALSE(buildDeferredRasterSceneGpuCullRoutePlan(inputs)
                   .gpuIndirectAvailable);
}

TEST(DeferredRasterSceneGpuCullRoutePlannerTests,
     GBufferPublishesOcclusionRouteOnlyWhenOutputIsValid) {
  auto inputs = readyGBufferRouteInputs();

  EXPECT_TRUE(buildDeferredRasterSceneGpuCullRoutePlan(inputs)
                  .occludedGpuIndirectAvailable);

  inputs.occlusionDrawsValid = false;
  EXPECT_FALSE(buildDeferredRasterSceneGpuCullRoutePlan(inputs)
                   .occludedGpuIndirectAvailable);

  inputs = readyGBufferRouteInputs();
  inputs.occlusionCullActive = false;
  EXPECT_FALSE(buildDeferredRasterSceneGpuCullRoutePlan(inputs)
                   .occludedGpuIndirectAvailable);
}

TEST(DeferredRasterSceneGpuCullRoutePlannerTests,
     DepthPrepassNeverPrefersOccludedIndirectOutput) {
  auto inputs = readyGBufferRouteInputs();
  inputs.kind = SceneRasterPassKind::DepthPrepass;

  const auto plan = buildDeferredRasterSceneGpuCullRoutePlan(inputs);

  EXPECT_TRUE(plan.gpuIndirectAvailable);
  EXPECT_FALSE(plan.occludedGpuIndirectAvailable);
  EXPECT_FALSE(plan.preferOccludedGpuIndirect);
}

TEST(DeferredRasterSceneGpuCullRoutePlannerTests,
     GBufferFallsBackToFrustumIndirectWhenOcclusionOutputIsInvalid) {
  const auto singleSided = singleSidedDraws();
  auto routeInputs = readyGBufferRouteInputs();
  routeInputs.occlusionDrawsValid = false;
  const auto routePlan =
      buildDeferredRasterSceneGpuCullRoutePlan(routeInputs);

  const auto rasterPlan = buildSceneRasterPassPlan(
      {.kind = SceneRasterPassKind::GBuffer,
       .gpuIndirectAvailable = routePlan.gpuIndirectAvailable,
       .occludedGpuIndirectAvailable =
           routePlan.occludedGpuIndirectAvailable,
       .preferOccludedGpuIndirect = routePlan.preferOccludedGpuIndirect,
       .draws = {.singleSided = &singleSided}});

  EXPECT_TRUE(rasterPlan.drawPlan.useGpuIndirectSingleSided);
  EXPECT_EQ(rasterPlan.drawPlan.gpuIndirectRoute.indirectSource,
            SceneOpaqueIndirectSource::FrustumCull);
}
