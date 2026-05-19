#include "Container/renderer/scene/DrawCommand.h"
#include "Container/renderer/scene/SceneRasterPassPlanner.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using container::renderer::DrawCommand;
using container::renderer::SceneOpaqueIndirectSource;
using container::renderer::SceneOpaqueDrawRouteKind;
using container::renderer::SceneRasterPassKind;
using container::renderer::SceneRasterPassPlanInputs;
using container::renderer::buildSceneRasterPassPlan;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

std::vector<DrawCommand> drawCommands() {
  return {DrawCommand{.indexCount = 3u, .instanceCount = 1u}};
}

SceneRasterPassPlanInputs readyInputs(SceneRasterPassKind kind) {
  return {.kind = kind,
          .pipelines = {.primary = fakeHandle<VkPipeline>(0x1),
                        .frontCull = fakeHandle<VkPipeline>(0x2),
                        .noCull = fakeHandle<VkPipeline>(0x3)}};
}

} // namespace

TEST(SceneRasterPassPlannerTests, DepthPrepassUsesDepthClearContract) {
  const auto plan =
      buildSceneRasterPassPlan(readyInputs(SceneRasterPassKind::DepthPrepass));

  ASSERT_EQ(plan.clearValues.count, 1u);
  EXPECT_FLOAT_EQ(plan.clearValues.values[0].depthStencil.depth, 0.0f);
}

TEST(SceneRasterPassPlannerTests, GBufferUsesFullAttachmentClearContract) {
  const auto plan =
      buildSceneRasterPassPlan(readyInputs(SceneRasterPassKind::GBuffer));

  EXPECT_EQ(plan.clearValues.count, 6u);
  EXPECT_FLOAT_EQ(plan.clearValues.values[1].color.float32[0], 0.5f);
  EXPECT_FLOAT_EQ(plan.clearValues.values[1].color.float32[2], 1.0f);
  EXPECT_EQ(plan.clearValues.values[5].color.uint32[0], 0u);
}

TEST(SceneRasterPassPlannerTests, CullVariantPipelinesFallbackToPrimary) {
  SceneRasterPassPlanInputs inputs = readyInputs(SceneRasterPassKind::GBuffer);
  inputs.pipelines.frontCull = VK_NULL_HANDLE;
  inputs.pipelines.noCull = VK_NULL_HANDLE;

  const auto plan = buildSceneRasterPassPlan(inputs);

  EXPECT_EQ(plan.pipelines.primary, inputs.pipelines.primary);
  EXPECT_EQ(plan.pipelines.frontCull, inputs.pipelines.primary);
  EXPECT_EQ(plan.pipelines.noCull, inputs.pipelines.primary);
}

TEST(SceneRasterPassPlannerTests,
     GpuIndirectAvailabilityFeedsOpaqueDrawPlan) {
  const auto singleSided = drawCommands();
  auto inputs = readyInputs(SceneRasterPassKind::DepthPrepass);
  inputs.gpuIndirectAvailable = true;
  inputs.draws.singleSided = &singleSided;

  const auto plan = buildSceneRasterPassPlan(inputs);

  EXPECT_TRUE(plan.drawPlan.useGpuIndirectSingleSided);
  EXPECT_EQ(plan.drawPlan.gpuIndirectRoute.commands, &singleSided);
  EXPECT_EQ(plan.drawPlan.gpuIndirectRoute.indirectSource,
            SceneOpaqueIndirectSource::FrustumCull);
}

TEST(SceneRasterPassPlannerTests,
     GBufferPrefersOcclusionCullIndirectOutputWhenAvailable) {
  const auto singleSided = drawCommands();
  auto inputs = readyInputs(SceneRasterPassKind::GBuffer);
  inputs.gpuIndirectAvailable = true;
  inputs.occludedGpuIndirectAvailable = true;
  inputs.preferOccludedGpuIndirect = true;
  inputs.draws.singleSided = &singleSided;

  const auto plan = buildSceneRasterPassPlan(inputs);

  EXPECT_TRUE(plan.drawPlan.useGpuIndirectSingleSided);
  EXPECT_EQ(plan.drawPlan.gpuIndirectRoute.indirectSource,
            SceneOpaqueIndirectSource::OcclusionCull);
}

TEST(SceneRasterPassPlannerTests,
     CpuDrawRoutesKeepOpaqueDrawPlannerOrdering) {
  const auto singleSided = drawCommands();
  const auto windingFlipped = drawCommands();
  const auto doubleSided = drawCommands();
  auto inputs = readyInputs(SceneRasterPassKind::DepthPrepass);
  inputs.draws = {.singleSided = &singleSided,
                  .windingFlipped = &windingFlipped,
                  .doubleSided = &doubleSided};

  const auto plan = buildSceneRasterPassPlan(inputs);

  ASSERT_EQ(plan.drawPlan.cpuRouteCount, 3u);
  EXPECT_EQ(plan.drawPlan.cpuRoutes[0].kind,
            SceneOpaqueDrawRouteKind::CpuSingleSided);
  EXPECT_EQ(plan.drawPlan.cpuRoutes[1].kind,
            SceneOpaqueDrawRouteKind::CpuWindingFlipped);
  EXPECT_EQ(plan.drawPlan.cpuRoutes[2].kind,
            SceneOpaqueDrawRouteKind::CpuDoubleSided);
}
