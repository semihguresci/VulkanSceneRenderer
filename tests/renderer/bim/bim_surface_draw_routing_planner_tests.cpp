#include "Container/renderer/bim/BimManager.h"
#include "Container/renderer/bim/BimSurfaceDrawRoutingPlanner.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

using container::renderer::BimDrawCompactionSlot;
using container::renderer::BimSurfaceDrawKind;
using container::renderer::BimSurfaceDrawLists;
using container::renderer::BimSurfaceDrawRouteKind;
using container::renderer::buildBimSurfaceDrawRoutingPlan;
using container::renderer::DrawCommand;
using container::renderer::hasBimSurfaceOpaqueDrawCommands;
using container::renderer::hasBimSurfaceTransparentDrawCommands;

[[nodiscard]] std::vector<DrawCommand> drawCommands(uint32_t firstIndex) {
  return {DrawCommand{.objectIndex = firstIndex,
                      .firstIndex = firstIndex,
                      .indexCount = 3u,
                      .instanceCount = 1u}};
}

TEST(BimSurfaceDrawRoutingPlannerTests, OpaqueAggregateMapsToSingleSidedRoute) {
  const auto aggregate = drawCommands(1u);

  const auto plan = buildBimSurfaceDrawRoutingPlan(
      {.kind = BimSurfaceDrawKind::Opaque,
       .draws = {.opaqueDrawCommands = &aggregate}});

  ASSERT_EQ(plan.routeCount, 3u);
  EXPECT_EQ(plan.routes[0].kind, BimSurfaceDrawRouteKind::SingleSided);
  EXPECT_EQ(plan.routes[0].gpuSlot, BimDrawCompactionSlot::OpaqueSingleSided);
  EXPECT_EQ(plan.routes[0].cpuCommands, &aggregate);
  EXPECT_FALSE(plan.routes[0].gpuCompactionAllowed);
  EXPECT_TRUE(plan.routes[0].cpuFallbackAllowed);
}

TEST(BimSurfaceDrawRoutingPlannerTests,
     OpaqueSplitsTakePrecedenceOverAggregate) {
  const auto aggregate = drawCommands(2u);
  const auto singleSided = drawCommands(3u);
  const auto windingFlipped = drawCommands(4u);
  const auto doubleSided = drawCommands(5u);

  const auto plan = buildBimSurfaceDrawRoutingPlan(
      {.kind = BimSurfaceDrawKind::Opaque,
       .draws = {.opaqueDrawCommands = &aggregate,
                 .opaqueSingleSidedDrawCommands = &singleSided,
                 .opaqueWindingFlippedDrawCommands = &windingFlipped,
                 .opaqueDoubleSidedDrawCommands = &doubleSided}});

  ASSERT_EQ(plan.routeCount, 3u);
  EXPECT_EQ(plan.routes[0].cpuCommands, &singleSided);
  EXPECT_EQ(plan.routes[1].kind, BimSurfaceDrawRouteKind::WindingFlipped);
  EXPECT_EQ(plan.routes[1].gpuSlot,
            BimDrawCompactionSlot::OpaqueWindingFlipped);
  EXPECT_EQ(plan.routes[1].cpuCommands, &windingFlipped);
  EXPECT_EQ(plan.routes[2].kind, BimSurfaceDrawRouteKind::DoubleSided);
  EXPECT_EQ(plan.routes[2].gpuSlot, BimDrawCompactionSlot::OpaqueDoubleSided);
  EXPECT_EQ(plan.routes[2].cpuCommands, &doubleSided);
}

TEST(BimSurfaceDrawRoutingPlannerTests,
     TransparentAggregateMapsToSingleSidedRoute) {
  const auto aggregate = drawCommands(6u);

  const auto plan = buildBimSurfaceDrawRoutingPlan(
      {.kind = BimSurfaceDrawKind::Transparent,
       .draws = {.transparentDrawCommands = &aggregate}});

  ASSERT_EQ(plan.routeCount, 3u);
  EXPECT_EQ(plan.routes[0].kind, BimSurfaceDrawRouteKind::SingleSided);
  EXPECT_EQ(plan.routes[0].gpuSlot,
            BimDrawCompactionSlot::TransparentSingleSided);
  EXPECT_EQ(plan.routes[0].cpuCommands, &aggregate);
}

TEST(BimSurfaceDrawRoutingPlannerTests,
     TransparentSplitsTakePrecedenceOverAggregate) {
  const auto aggregate = drawCommands(7u);
  const auto singleSided = drawCommands(8u);
  const auto windingFlipped = drawCommands(9u);
  const auto doubleSided = drawCommands(10u);

  const auto plan = buildBimSurfaceDrawRoutingPlan(
      {.kind = BimSurfaceDrawKind::Transparent,
       .draws = {.transparentDrawCommands = &aggregate,
                 .transparentSingleSidedDrawCommands = &singleSided,
                 .transparentWindingFlippedDrawCommands = &windingFlipped,
                 .transparentDoubleSidedDrawCommands = &doubleSided}});

  ASSERT_EQ(plan.routeCount, 3u);
  EXPECT_EQ(plan.routes[0].cpuCommands, &singleSided);
  EXPECT_EQ(plan.routes[1].gpuSlot,
            BimDrawCompactionSlot::TransparentWindingFlipped);
  EXPECT_EQ(plan.routes[1].cpuCommands, &windingFlipped);
  EXPECT_EQ(plan.routes[2].gpuSlot,
            BimDrawCompactionSlot::TransparentDoubleSided);
  EXPECT_EQ(plan.routes[2].cpuCommands, &doubleSided);
}

TEST(BimSurfaceDrawRoutingPlannerTests,
     GpuVisibilitySuppressesCpuFallbackForAllRoutes) {
  const auto aggregate = drawCommands(11u);

  const auto plan = buildBimSurfaceDrawRoutingPlan(
      {.kind = BimSurfaceDrawKind::Opaque,
       .draws = {.opaqueDrawCommands = &aggregate},
       .gpuCompactionEligible = true,
       .gpuVisibilityOwnsCpuFallback = true});

  ASSERT_EQ(plan.routeCount, 3u);
  EXPECT_TRUE(plan.routes[0].gpuCompactionAllowed);
  EXPECT_FALSE(plan.routes[1].gpuCompactionAllowed);
  EXPECT_FALSE(plan.routes[2].gpuCompactionAllowed);
  for (uint32_t routeIndex = 0; routeIndex < plan.routeCount; ++routeIndex) {
    EXPECT_FALSE(plan.routes[routeIndex].cpuFallbackAllowed);
  }
}

TEST(BimSurfaceDrawRoutingPlannerTests,
     GpuCompactionEligibilityDoesNotSuppressCpuFallback) {
  const auto aggregate = drawCommands(12u);

  const auto plan = buildBimSurfaceDrawRoutingPlan(
      {.kind = BimSurfaceDrawKind::Transparent,
       .draws = {.transparentDrawCommands = &aggregate},
       .gpuCompactionEligible = true});

  ASSERT_EQ(plan.routeCount, 3u);
  EXPECT_TRUE(plan.routes[0].gpuCompactionAllowed);
  EXPECT_FALSE(plan.routes[1].gpuCompactionAllowed);
  EXPECT_FALSE(plan.routes[2].gpuCompactionAllowed);
  for (uint32_t routeIndex = 0; routeIndex < plan.routeCount; ++routeIndex) {
    EXPECT_TRUE(plan.routes[routeIndex].cpuFallbackAllowed);
  }
}

TEST(BimSurfaceDrawRoutingPlannerTests,
     GpuVisibilityFallbackOwnershipDoesNotImplyCompactionEligibility) {
  const auto aggregate = drawCommands(13u);

  const auto plan = buildBimSurfaceDrawRoutingPlan(
      {.kind = BimSurfaceDrawKind::Transparent,
       .draws = {.transparentDrawCommands = &aggregate},
       .gpuVisibilityOwnsCpuFallback = true});

  ASSERT_EQ(plan.routeCount, 3u);
  for (uint32_t routeIndex = 0; routeIndex < plan.routeCount; ++routeIndex) {
    EXPECT_FALSE(plan.routes[routeIndex].gpuCompactionAllowed);
    EXPECT_FALSE(plan.routes[routeIndex].cpuFallbackAllowed);
  }
}

TEST(BimSurfaceDrawRoutingPlannerTests,
     GpuCompactionEligibilityFollowsRouteSources) {
  const auto aggregate = drawCommands(14u);
  const auto windingFlipped = drawCommands(15u);

  const auto plan = buildBimSurfaceDrawRoutingPlan(
      {.kind = BimSurfaceDrawKind::Transparent,
       .draws = {.transparentDrawCommands = &aggregate,
                 .transparentWindingFlippedDrawCommands = &windingFlipped},
       .gpuCompactionEligible = true,
       .gpuVisibilityOwnsCpuFallback = true});

  ASSERT_EQ(plan.routeCount, 3u);
  EXPECT_FALSE(plan.routes[0].gpuCompactionAllowed);
  EXPECT_TRUE(plan.routes[1].gpuCompactionAllowed);
  EXPECT_FALSE(plan.routes[2].gpuCompactionAllowed);
  for (uint32_t routeIndex = 0; routeIndex < plan.routeCount; ++routeIndex) {
    EXPECT_FALSE(plan.routes[routeIndex].cpuFallbackAllowed);
  }
}

TEST(BimSurfaceDrawRoutingPlannerTests, ViewportCullingUsesDedicatedOpaqueSlots) {
  using Slot = BimDrawCompactionSlot;
  using container::renderer::bimViewportCompactionSlot;
  EXPECT_EQ(bimViewportCompactionSlot(Slot::OpaqueSingleSided), Slot::ViewportOpaqueSingleSided);
  EXPECT_EQ(bimViewportCompactionSlot(Slot::OpaqueWindingFlipped), Slot::ViewportOpaqueWindingFlipped);
  EXPECT_EQ(bimViewportCompactionSlot(Slot::OpaqueDoubleSided), Slot::ViewportOpaqueDoubleSided);
  EXPECT_FALSE(bimViewportCompactionSlot(Slot::TransparentSingleSided));
  EXPECT_FALSE(bimViewportCompactionSlot(Slot::TransparentDoubleSided));
  EXPECT_FALSE(bimViewportCompactionSlot(Slot::NativePointOpaque));
  EXPECT_FALSE(bimViewportCompactionSlot(Slot::NativeCurveOpaque));
}

TEST(BimSurfaceDrawRoutingPlannerTests, DetectsOpaqueAndTransparentCommands) {
  const auto opaque = drawCommands(16u);
  const auto transparent = drawCommands(17u);

  EXPECT_TRUE(hasBimSurfaceOpaqueDrawCommands(
      {.opaqueSingleSidedDrawCommands = &opaque}));
  EXPECT_TRUE(hasBimSurfaceTransparentDrawCommands(
      {.transparentWindingFlippedDrawCommands = &transparent}));
  EXPECT_FALSE(hasBimSurfaceOpaqueDrawCommands({}));
  EXPECT_FALSE(hasBimSurfaceTransparentDrawCommands({}));
}

} // namespace
