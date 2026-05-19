#include "Container/renderer/scene/DrawCommand.h"
#include "Container/renderer/scene/SceneOpaqueDrawPlanner.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

using container::renderer::DrawCommand;
using container::renderer::SceneOpaqueIndirectSource;
using container::renderer::SceneOpaqueDrawPipeline;
using container::renderer::SceneOpaqueDrawRouteKind;
using container::renderer::buildSceneOpaqueDrawPlan;

[[nodiscard]] std::vector<DrawCommand> drawCommands(uint32_t firstIndex) {
  return {DrawCommand{.objectIndex = firstIndex,
                      .firstIndex = firstIndex,
                      .indexCount = 3u,
                      .instanceCount = 1u}};
}

TEST(SceneOpaqueDrawPlannerTests,
     GpuIndirectSingleSidedSuppressesCpuSingleSidedFallback) {
  const auto singleSided = drawCommands(1u);
  const auto windingFlipped = drawCommands(2u);
  const auto doubleSided = drawCommands(3u);

  const auto plan = buildSceneOpaqueDrawPlan(
      {.gpuIndirectAvailable = true,
       .draws = {.singleSided = &singleSided,
                 .windingFlipped = &windingFlipped,
                 .doubleSided = &doubleSided}});

  EXPECT_TRUE(plan.useGpuIndirectSingleSided);
  EXPECT_EQ(plan.gpuIndirectRoute.kind,
            SceneOpaqueDrawRouteKind::GpuIndirectSingleSided);
  EXPECT_EQ(plan.gpuIndirectRoute.pipeline, SceneOpaqueDrawPipeline::Primary);
  EXPECT_EQ(plan.gpuIndirectRoute.indirectSource,
            SceneOpaqueIndirectSource::FrustumCull);
  EXPECT_EQ(plan.gpuIndirectRoute.commands, &singleSided);
  ASSERT_EQ(plan.cpuRouteCount, 2u);
  EXPECT_EQ(plan.cpuRoutes[0].kind, SceneOpaqueDrawRouteKind::CpuWindingFlipped);
  EXPECT_EQ(plan.cpuRoutes[0].pipeline, SceneOpaqueDrawPipeline::FrontCull);
  EXPECT_EQ(plan.cpuRoutes[0].commands, &windingFlipped);
  EXPECT_EQ(plan.cpuRoutes[1].kind, SceneOpaqueDrawRouteKind::CpuDoubleSided);
  EXPECT_EQ(plan.cpuRoutes[1].pipeline, SceneOpaqueDrawPipeline::NoCull);
  EXPECT_EQ(plan.cpuRoutes[1].commands, &doubleSided);
}

TEST(SceneOpaqueDrawPlannerTests,
     MissingSingleSidedDrawsDisableGpuIndirectRoute) {
  const auto windingFlipped = drawCommands(4u);

  const auto plan = buildSceneOpaqueDrawPlan(
      {.gpuIndirectAvailable = true,
       .draws = {.windingFlipped = &windingFlipped}});

  EXPECT_FALSE(plan.useGpuIndirectSingleSided);
  EXPECT_EQ(plan.gpuIndirectRoute.commands, nullptr);
  ASSERT_EQ(plan.cpuRouteCount, 1u);
  EXPECT_EQ(plan.cpuRoutes[0].kind, SceneOpaqueDrawRouteKind::CpuWindingFlipped);
}

TEST(SceneOpaqueDrawPlannerTests,
     CpuSingleSidedRouteIsUsedWhenGpuIndirectUnavailable) {
  const auto singleSided = drawCommands(5u);

  const auto plan =
      buildSceneOpaqueDrawPlan({.gpuIndirectAvailable = false,
                                .draws = {.singleSided = &singleSided}});

  EXPECT_FALSE(plan.useGpuIndirectSingleSided);
  ASSERT_EQ(plan.cpuRouteCount, 1u);
  EXPECT_EQ(plan.cpuRoutes[0].kind, SceneOpaqueDrawRouteKind::CpuSingleSided);
  EXPECT_EQ(plan.cpuRoutes[0].pipeline, SceneOpaqueDrawPipeline::Primary);
  EXPECT_EQ(plan.cpuRoutes[0].commands, &singleSided);
}

TEST(SceneOpaqueDrawPlannerTests, CpuRoutesKeepStableCullVariantOrder) {
  const auto singleSided = drawCommands(6u);
  const auto windingFlipped = drawCommands(7u);
  const auto doubleSided = drawCommands(8u);

  const auto plan =
      buildSceneOpaqueDrawPlan({.draws = {.singleSided = &singleSided,
                                          .windingFlipped = &windingFlipped,
                                          .doubleSided = &doubleSided}});

  ASSERT_EQ(plan.cpuRouteCount, 3u);
  EXPECT_EQ(plan.cpuRoutes[0].kind, SceneOpaqueDrawRouteKind::CpuSingleSided);
  EXPECT_EQ(plan.cpuRoutes[1].kind, SceneOpaqueDrawRouteKind::CpuWindingFlipped);
  EXPECT_EQ(plan.cpuRoutes[2].kind, SceneOpaqueDrawRouteKind::CpuDoubleSided);
}

TEST(SceneOpaqueDrawPlannerTests, EmptyDrawListsProduceNoRoutes) {
  const auto empty = std::vector<DrawCommand>{};

  const auto plan = buildSceneOpaqueDrawPlan(
      {.gpuIndirectAvailable = true,
       .draws = {.singleSided = &empty, .doubleSided = &empty}});

  EXPECT_FALSE(plan.useGpuIndirectSingleSided);
  EXPECT_EQ(plan.cpuRouteCount, 0u);
}

TEST(SceneOpaqueDrawPlannerTests,
     OcclusionIndirectRouteIsSelectedWhenPreferredAndAvailable) {
  const auto singleSided = drawCommands(9u);

  const auto plan = buildSceneOpaqueDrawPlan(
      {.gpuIndirectAvailable = true,
       .occludedGpuIndirectAvailable = true,
       .preferOccludedGpuIndirect = true,
       .draws = {.singleSided = &singleSided}});

  EXPECT_TRUE(plan.useGpuIndirectSingleSided);
  EXPECT_EQ(plan.gpuIndirectRoute.indirectSource,
            SceneOpaqueIndirectSource::OcclusionCull);
  EXPECT_EQ(plan.gpuIndirectRoute.commands, &singleSided);
}

TEST(SceneOpaqueDrawPlannerTests,
     AggregateDrawsProvideCpuFallbackWhenSplitListsAreMissing) {
  const auto aggregate = drawCommands(10u);

  const auto plan = buildSceneOpaqueDrawPlan(
      {.gpuIndirectAvailable = false, .draws = {.aggregate = &aggregate}});

  EXPECT_FALSE(plan.useGpuIndirectSingleSided);
  ASSERT_EQ(plan.cpuRouteCount, 1u);
  EXPECT_EQ(plan.cpuRoutes[0].kind, SceneOpaqueDrawRouteKind::CpuSingleSided);
  EXPECT_EQ(plan.cpuRoutes[0].pipeline, SceneOpaqueDrawPipeline::Primary);
  EXPECT_EQ(plan.cpuRoutes[0].commands, &aggregate);
}

} // namespace
