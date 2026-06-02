#include "Container/renderer/bim/BimManager.h"
#include "Container/renderer/scene/DrawCommand.h"
#include "Container/renderer/bim/BimSurfacePassPlanner.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

using container::renderer::BimDrawCompactionSlot;
using container::renderer::BimSurfaceDrawRouteKind;
using container::renderer::BimSurfacePassInputs;
using container::renderer::BimSurfacePassKind;
using container::renderer::BimSurfacePassSource;
using container::renderer::BimSurfacePassSourceKind;
using container::renderer::DrawCommand;
using container::renderer::buildBimSurfacePassPlan;

[[nodiscard]] std::vector<DrawCommand> drawCommands(uint32_t firstIndex) {
  return {DrawCommand{.objectIndex = firstIndex,
                      .firstIndex = firstIndex,
                      .indexCount = 3u,
                      .instanceCount = 1u}};
}

[[nodiscard]] BimSurfacePassInputs readyInputs() {
  return {.kind = BimSurfacePassKind::DepthPrepass,
          .passReady = true,
          .geometryReady = true,
          .descriptorSetReady = true,
          .bindlessPushConstantsReady = true,
          .basePipelineReady = true};
}

void appendSource(BimSurfacePassInputs &inputs, BimSurfacePassSource source) {
  ASSERT_LT(inputs.sourceCount, inputs.sources.size());
  inputs.sources[inputs.sourceCount] = source;
  ++inputs.sourceCount;
}

TEST(BimSurfacePassPlannerTests, InactiveWhenCommonReadinessMissing) {
  const auto opaque = drawCommands(1u);
  auto inputs = readyInputs();
  appendSource(inputs, {.draws = {.opaqueDrawCommands = &opaque}});

  inputs.passReady = false;
  EXPECT_FALSE(buildBimSurfacePassPlan(inputs).active);

  inputs = readyInputs();
  appendSource(inputs, {.draws = {.opaqueDrawCommands = &opaque}});
  inputs.geometryReady = false;
  EXPECT_FALSE(buildBimSurfacePassPlan(inputs).active);

  inputs = readyInputs();
  appendSource(inputs, {.draws = {.opaqueDrawCommands = &opaque}});
  inputs.descriptorSetReady = false;
  EXPECT_FALSE(buildBimSurfacePassPlan(inputs).active);

  inputs = readyInputs();
  appendSource(inputs, {.draws = {.opaqueDrawCommands = &opaque}});
  inputs.bindlessPushConstantsReady = false;
  EXPECT_FALSE(buildBimSurfacePassPlan(inputs).active);

  inputs = readyInputs();
  appendSource(inputs, {.draws = {.opaqueDrawCommands = &opaque}});
  inputs.basePipelineReady = false;
  EXPECT_FALSE(buildBimSurfacePassPlan(inputs).active);
}

TEST(BimSurfacePassPlannerTests, InactiveWhenNoOpaqueSurfaceRoutes) {
  const auto transparent = drawCommands(2u);
  auto inputs = readyInputs();
  appendSource(inputs,
               {.draws = {.transparentDrawCommands = &transparent}});

  const auto plan = buildBimSurfacePassPlan(inputs);

  EXPECT_FALSE(plan.active);
  EXPECT_EQ(plan.sourceCount, 0u);
}

TEST(BimSurfacePassPlannerTests,
     SourceOrderIsMeshPointPlaceholdersCurvePlaceholders) {
  const auto mesh = drawCommands(3u);
  const auto points = drawCommands(4u);
  const auto curves = drawCommands(5u);
  auto inputs = readyInputs();
  appendSource(inputs, {.source = BimSurfacePassSourceKind::Mesh,
                        .draws = {.opaqueDrawCommands = &mesh}});
  appendSource(inputs, {.source = BimSurfacePassSourceKind::PointPlaceholders,
                        .draws = {.opaqueDrawCommands = &points}});
  appendSource(inputs, {.source = BimSurfacePassSourceKind::CurvePlaceholders,
                        .draws = {.opaqueDrawCommands = &curves}});

  const auto plan = buildBimSurfacePassPlan(inputs);

  ASSERT_TRUE(plan.active);
  ASSERT_EQ(plan.sourceCount, 3u);
  EXPECT_EQ(plan.sources[0].source, BimSurfacePassSourceKind::Mesh);
  EXPECT_EQ(plan.sources[1].source,
            BimSurfacePassSourceKind::PointPlaceholders);
  EXPECT_EQ(plan.sources[2].source,
            BimSurfacePassSourceKind::CurvePlaceholders);
}

TEST(BimSurfacePassPlannerTests, OnlyMeshSourceAllowsGpuCompaction) {
  const auto mesh = drawCommands(6u);
  const auto points = drawCommands(7u);
  const auto curves = drawCommands(8u);
  auto inputs = readyInputs();
  appendSource(inputs, {.source = BimSurfacePassSourceKind::Mesh,
                        .draws = {.opaqueDrawCommands = &mesh},
                        .gpuCompactionEligible = true});
  appendSource(inputs, {.source = BimSurfacePassSourceKind::PointPlaceholders,
                        .draws = {.opaqueDrawCommands = &points},
                        .gpuCompactionEligible = true});
  appendSource(inputs, {.source = BimSurfacePassSourceKind::CurvePlaceholders,
                        .draws = {.opaqueDrawCommands = &curves},
                        .gpuCompactionEligible = true});

  const auto plan = buildBimSurfacePassPlan(inputs);

  ASSERT_TRUE(plan.active);
  ASSERT_EQ(plan.sourceCount, 3u);
  EXPECT_TRUE(plan.sources[0].routes[0].gpuCompactionAllowed);
  EXPECT_FALSE(plan.sources[1].routes[0].gpuCompactionAllowed);
  EXPECT_FALSE(plan.sources[2].routes[0].gpuCompactionAllowed);
}

TEST(BimSurfacePassPlannerTests,
     GpuVisibilityOwnsCpuFallbackOnlyForMesh) {
  const auto mesh = drawCommands(9u);
  const auto points = drawCommands(10u);
  const auto curves = drawCommands(11u);
  auto inputs = readyInputs();
  appendSource(inputs, {.source = BimSurfacePassSourceKind::Mesh,
                        .draws = {.opaqueDrawCommands = &mesh},
                        .gpuVisibilityOwnsCpuFallback = true});
  appendSource(inputs, {.source = BimSurfacePassSourceKind::PointPlaceholders,
                        .draws = {.opaqueDrawCommands = &points},
                        .gpuVisibilityOwnsCpuFallback = true});
  appendSource(inputs, {.source = BimSurfacePassSourceKind::CurvePlaceholders,
                        .draws = {.opaqueDrawCommands = &curves},
                        .gpuVisibilityOwnsCpuFallback = true});

  const auto plan = buildBimSurfacePassPlan(inputs);

  ASSERT_TRUE(plan.active);
  EXPECT_FALSE(plan.sources[0].routes[0].gpuCompactionAllowed);
  EXPECT_FALSE(plan.sources[0].routes[0].cpuFallbackAllowed);
  EXPECT_TRUE(plan.sources[1].routes[0].cpuFallbackAllowed);
  EXPECT_TRUE(plan.sources[2].routes[0].cpuFallbackAllowed);
}

TEST(BimSurfacePassPlannerTests,
     RoutesPreserveSingleWindingDoubleOrder) {
  const auto single = drawCommands(12u);
  const auto winding = drawCommands(13u);
  const auto doubleSided = drawCommands(14u);
  auto inputs = readyInputs();
  appendSource(inputs,
               {.draws = {.opaqueSingleSidedDrawCommands = &single,
                           .opaqueWindingFlippedDrawCommands = &winding,
                           .opaqueDoubleSidedDrawCommands = &doubleSided},
                .gpuCompactionEligible = true});

  const auto plan = buildBimSurfacePassPlan(inputs);

  ASSERT_TRUE(plan.active);
  ASSERT_EQ(plan.sourceCount, 1u);
  ASSERT_EQ(plan.sources[0].routeCount, 3u);
  EXPECT_EQ(plan.sources[0].routes[0].kind,
            BimSurfaceDrawRouteKind::SingleSided);
  EXPECT_EQ(plan.sources[0].routes[0].gpuSlot,
            BimDrawCompactionSlot::OpaqueSingleSided);
  EXPECT_EQ(plan.sources[0].routes[1].kind,
            BimSurfaceDrawRouteKind::WindingFlipped);
  EXPECT_EQ(plan.sources[0].routes[1].gpuSlot,
            BimDrawCompactionSlot::OpaqueWindingFlipped);
  EXPECT_EQ(plan.sources[0].routes[2].kind,
            BimSurfaceDrawRouteKind::DoubleSided);
  EXPECT_EQ(plan.sources[0].routes[2].gpuSlot,
            BimDrawCompactionSlot::OpaqueDoubleSided);
}

TEST(BimSurfacePassPlannerTests, GBufferPlanCarriesSemanticColorMode) {
  const auto opaque = drawCommands(15u);
  auto inputs = readyInputs();
  inputs.kind = BimSurfacePassKind::GBuffer;
  inputs.semanticColorMode = 42u;
  appendSource(inputs, {.draws = {.opaqueDrawCommands = &opaque}});

  const auto plan = buildBimSurfacePassPlan(inputs);

  ASSERT_TRUE(plan.active);
  EXPECT_TRUE(plan.writesSemanticColorMode);
  EXPECT_EQ(plan.semanticColorMode, 42u);
}

TEST(BimSurfacePassPlannerTests,
     OpaqueLightingUsesOpaqueRoutesAndCarriesSemanticColorMode) {
  const auto opaque = drawCommands(23u);
  auto inputs = readyInputs();
  inputs.kind = BimSurfacePassKind::OpaqueLighting;
  inputs.semanticColorMode = 123u;
  appendSource(inputs, {.draws = {.opaqueDrawCommands = &opaque}});

  const auto plan = buildBimSurfacePassPlan(inputs);

  ASSERT_TRUE(plan.active);
  EXPECT_TRUE(plan.writesSemanticColorMode);
  EXPECT_EQ(plan.semanticColorMode, 123u);
  ASSERT_EQ(plan.sourceCount, 1u);
  ASSERT_EQ(plan.sources[0].routeCount, 3u);
  EXPECT_EQ(plan.sources[0].routes[0].kind,
            BimSurfaceDrawRouteKind::SingleSided);
  EXPECT_EQ(plan.sources[0].routes[0].gpuSlot,
            BimDrawCompactionSlot::OpaqueSingleSided);
  EXPECT_EQ(plan.sources[0].routes[1].gpuSlot,
            BimDrawCompactionSlot::OpaqueWindingFlipped);
  EXPECT_EQ(plan.sources[0].routes[2].gpuSlot,
            BimDrawCompactionSlot::OpaqueDoubleSided);
}

TEST(BimSurfacePassPlannerTests, DepthPlanDoesNotRequireSemanticColorMode) {
  const auto opaque = drawCommands(16u);
  auto inputs = readyInputs();
  inputs.semanticColorMode = 77u;
  appendSource(inputs, {.draws = {.opaqueDrawCommands = &opaque}});

  const auto plan = buildBimSurfacePassPlan(inputs);

  ASSERT_TRUE(plan.active);
  EXPECT_FALSE(plan.writesSemanticColorMode);
  EXPECT_EQ(plan.semanticColorMode, 0u);
}

TEST(BimSurfacePassPlannerTests,
     GpuCompactionPlanDoesNotRequireRuntimeReadiness) {
  const auto opaque = drawCommands(17u);
  auto inputs = readyInputs();
  appendSource(inputs, {.draws = {.opaqueDrawCommands = &opaque},
                        .gpuCompactionEligible = true});

  const auto plan = buildBimSurfacePassPlan(inputs);

  ASSERT_TRUE(plan.active);
  ASSERT_EQ(plan.sourceCount, 1u);
  EXPECT_TRUE(plan.sources[0].routes[0].gpuCompactionAllowed);
  EXPECT_TRUE(plan.sources[0].routes[0].cpuFallbackAllowed);
}

TEST(BimSurfacePassPlannerTests, TransparentPickUsesTransparentRoutes) {
  const auto transparent = drawCommands(18u);
  auto inputs = readyInputs();
  inputs.kind = BimSurfacePassKind::TransparentPick;
  appendSource(inputs, {.draws = {.transparentDrawCommands = &transparent},
                        .gpuCompactionEligible = true});

  const auto plan = buildBimSurfacePassPlan(inputs);

  ASSERT_TRUE(plan.active);
  ASSERT_EQ(plan.sourceCount, 1u);
  ASSERT_EQ(plan.sources[0].routeCount, 3u);
  EXPECT_EQ(plan.sources[0].routes[0].gpuSlot,
            BimDrawCompactionSlot::TransparentSingleSided);
  EXPECT_TRUE(plan.sources[0].routes[0].gpuCompactionAllowed);
  EXPECT_FALSE(plan.writesSemanticColorMode);
}

TEST(BimSurfacePassPlannerTests,
     TransparentLightingCarriesSemanticColorMode) {
  const auto transparent = drawCommands(19u);
  auto inputs = readyInputs();
  inputs.kind = BimSurfacePassKind::TransparentLighting;
  inputs.semanticColorMode = 9u;
  appendSource(inputs, {.draws = {.transparentDrawCommands = &transparent}});

  const auto plan = buildBimSurfacePassPlan(inputs);

  ASSERT_TRUE(plan.active);
  EXPECT_TRUE(plan.writesSemanticColorMode);
  EXPECT_EQ(plan.semanticColorMode, 9u);
}

TEST(BimSurfacePassPlannerTests,
     TransparentGpuVisibilitySuppressesOnlyMeshFallback) {
  const auto mesh = drawCommands(20u);
  const auto points = drawCommands(21u);
  const auto curves = drawCommands(22u);
  auto inputs = readyInputs();
  inputs.kind = BimSurfacePassKind::TransparentLighting;
  appendSource(inputs, {.source = BimSurfacePassSourceKind::Mesh,
                        .draws = {.transparentDrawCommands = &mesh},
                        .gpuCompactionEligible = true,
                        .gpuVisibilityOwnsCpuFallback = true});
  appendSource(inputs, {.source = BimSurfacePassSourceKind::PointPlaceholders,
                        .draws = {.transparentDrawCommands = &points},
                        .gpuVisibilityOwnsCpuFallback = true});
  appendSource(inputs, {.source = BimSurfacePassSourceKind::CurvePlaceholders,
                        .draws = {.transparentDrawCommands = &curves},
                        .gpuVisibilityOwnsCpuFallback = true});

  const auto plan = buildBimSurfacePassPlan(inputs);

  ASSERT_TRUE(plan.active);
  ASSERT_EQ(plan.sourceCount, 3u);
  EXPECT_TRUE(plan.sources[0].routes[0].gpuCompactionAllowed);
  EXPECT_FALSE(plan.sources[0].routes[0].cpuFallbackAllowed);
  EXPECT_FALSE(plan.sources[1].routes[0].gpuCompactionAllowed);
  EXPECT_TRUE(plan.sources[1].routes[0].cpuFallbackAllowed);
  EXPECT_FALSE(plan.sources[2].routes[0].gpuCompactionAllowed);
  EXPECT_TRUE(plan.sources[2].routes[0].cpuFallbackAllowed);
}

} // namespace
