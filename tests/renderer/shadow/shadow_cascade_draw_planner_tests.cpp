#include "Container/renderer/shadow/ShadowCascadeDrawPlanner.h"

#include <gtest/gtest.h>

#include <limits>
#include <vector>

namespace {

using container::gpu::kShadowCascadeCount;
using container::gpu::ObjectData;
using container::renderer::buildShadowCascadeDrawPlan;
using container::renderer::computeShadowCascadeDrawSignature;
using container::renderer::DrawCommand;
using container::renderer::ShadowCascadeDrawPlannerInputs;

[[nodiscard]] DrawCommand drawCommand(uint32_t objectIndex,
                                      uint32_t instanceCount = 1u) {
  return DrawCommand{.objectIndex = objectIndex,
                     .firstIndex = objectIndex * 3u,
                     .indexCount = 3u,
                     .instanceCount = instanceCount};
}

[[nodiscard]] ObjectData objectWithBounds(float cascadeMarker, float radius) {
  ObjectData object{};
  object.boundingSphere = {cascadeMarker, 0.0f, 0.0f, radius};
  return object;
}

void activateAllCascades(ShadowCascadeDrawPlannerInputs &inputs) {
  inputs.shadowPassActive.fill(true);
}

TEST(ShadowCascadeDrawPlannerTests, DistributesCommandsWithoutIntersector) {
  const std::vector<DrawCommand> singleSided = {drawCommand(2u, 2u)};

  ShadowCascadeDrawPlannerInputs inputs{};
  inputs.sceneDraws = {.singleSided = &singleSided};
  activateAllCascades(inputs);

  const auto plan = buildShadowCascadeDrawPlan(inputs);

  for (uint32_t cascadeIndex = 0; cascadeIndex < kShadowCascadeCount;
       ++cascadeIndex) {
    ASSERT_EQ(plan.sceneSingleSided[cascadeIndex].size(), 1u);
    EXPECT_EQ(plan.sceneSingleSided[cascadeIndex][0].objectIndex, 2u);
    EXPECT_EQ(plan.cpuCommandCount(cascadeIndex, true), 1u);
  }
}

TEST(ShadowCascadeDrawPlannerTests, InactiveCascadesStayEmpty) {
  const std::vector<DrawCommand> singleSided = {drawCommand(1u)};

  ShadowCascadeDrawPlannerInputs inputs{};
  inputs.sceneDraws = {.singleSided = &singleSided};
  inputs.shadowPassActive[0] = true;

  const auto plan = buildShadowCascadeDrawPlan(inputs);

  ASSERT_EQ(plan.sceneSingleSided[0].size(), 1u);
  for (uint32_t cascadeIndex = 1; cascadeIndex < kShadowCascadeCount;
       ++cascadeIndex) {
    EXPECT_TRUE(plan.sceneSingleSided[cascadeIndex].empty());
  }
}

TEST(ShadowCascadeDrawPlannerTests, SplitsVisibleInstanceRunsPerCascade) {
  const std::vector<DrawCommand> singleSided = {drawCommand(0u, 3u)};
  const std::vector<ObjectData> objectData = {
      objectWithBounds(0.0f, 1.0f),
      objectWithBounds(1.0f, 1.0f),
      objectWithBounds(0.0f, 1.0f),
  };

  ShadowCascadeDrawPlannerInputs inputs{};
  inputs.scene = {.objectData = &objectData};
  inputs.sceneDraws = {.singleSided = &singleSided};
  activateAllCascades(inputs);
  inputs.cascadeIntersectsSphere = [](uint32_t cascadeIndex,
                                      const glm::vec4 &bounds) {
    return static_cast<uint32_t>(bounds.x) == cascadeIndex;
  };

  const auto plan = buildShadowCascadeDrawPlan(inputs);

  ASSERT_EQ(plan.sceneSingleSided[0].size(), 2u);
  EXPECT_EQ(plan.sceneSingleSided[0][0].objectIndex, 0u);
  EXPECT_EQ(plan.sceneSingleSided[0][0].instanceCount, 1u);
  EXPECT_EQ(plan.sceneSingleSided[0][1].objectIndex, 2u);
  EXPECT_EQ(plan.sceneSingleSided[0][1].instanceCount, 1u);

  ASSERT_EQ(plan.sceneSingleSided[1].size(), 1u);
  EXPECT_EQ(plan.sceneSingleSided[1][0].objectIndex, 1u);
  EXPECT_EQ(plan.sceneSingleSided[1][0].instanceCount, 1u);
}

TEST(ShadowCascadeDrawPlannerTests, InvalidBoundsFailOpen) {
  const std::vector<DrawCommand> singleSided = {drawCommand(0u)};
  const std::vector<ObjectData> objectData = {objectWithBounds(0.0f, 0.0f)};

  ShadowCascadeDrawPlannerInputs inputs{};
  inputs.scene = {.objectData = &objectData};
  inputs.sceneDraws = {.singleSided = &singleSided};
  activateAllCascades(inputs);
  inputs.cascadeIntersectsSphere = [](uint32_t, const glm::vec4 &) {
    return false;
  };

  const auto plan = buildShadowCascadeDrawPlan(inputs);

  for (uint32_t cascadeIndex = 0; cascadeIndex < kShadowCascadeCount;
       ++cascadeIndex) {
    ASSERT_EQ(plan.sceneSingleSided[cascadeIndex].size(), 1u);
    EXPECT_EQ(plan.sceneSingleSided[cascadeIndex][0].objectIndex, 0u);
  }
}

TEST(ShadowCascadeDrawPlannerTests, NonFiniteBoundsFailOpen) {
  const std::vector<DrawCommand> singleSided = {drawCommand(0u)};
  const std::vector<ObjectData> objectData = {
      objectWithBounds(std::numeric_limits<float>::quiet_NaN(), 1.0f)};

  ShadowCascadeDrawPlannerInputs inputs{};
  inputs.scene = {.objectData = &objectData};
  inputs.sceneDraws = {.singleSided = &singleSided};
  activateAllCascades(inputs);
  inputs.cascadeIntersectsSphere = [](uint32_t, const glm::vec4 &) {
    return false;
  };

  const auto plan = buildShadowCascadeDrawPlan(inputs);

  for (uint32_t cascadeIndex = 0; cascadeIndex < kShadowCascadeCount;
       ++cascadeIndex) {
    ASSERT_EQ(plan.sceneSingleSided[cascadeIndex].size(), 1u);
    EXPECT_EQ(plan.sceneSingleSided[cascadeIndex][0].objectIndex, 0u);
  }
}

TEST(ShadowCascadeDrawPlannerTests, GpuCulledSceneSingleSidedCascadeIsSkipped) {
  const std::vector<DrawCommand> singleSided = {drawCommand(1u)};
  const std::vector<DrawCommand> windingFlipped = {drawCommand(2u)};

  ShadowCascadeDrawPlannerInputs inputs{};
  inputs.sceneDraws = {.singleSided = &singleSided,
                       .windingFlipped = &windingFlipped};
  activateAllCascades(inputs);
  inputs.sceneSingleSidedUsesGpuCull[1] = true;

  const auto plan = buildShadowCascadeDrawPlan(inputs);

  EXPECT_TRUE(plan.sceneSingleSided[1].empty());
  ASSERT_EQ(plan.sceneWindingFlipped[1].size(), 1u);
  EXPECT_EQ(plan.sceneWindingFlipped[1][0].objectIndex, 2u);
  EXPECT_EQ(plan.cpuCommandCount(1u, false), 1u);
}

TEST(ShadowCascadeDrawPlannerTests, BimGpuVisibilitySuppressesMeshCpuFallback) {
  const std::vector<DrawCommand> rootMesh = {drawCommand(3u)};
  const std::vector<DrawCommand> placeholderPoint = {drawCommand(4u)};

  ShadowCascadeDrawPlannerInputs inputs{};
  activateAllCascades(inputs);
  inputs.hasBimShadowGeometry = true;
  inputs.bimDrawListCount = 2u;
  inputs.bimDraws[0] = {.singleSided = &rootMesh, .cpuFallbackAllowed = false};
  inputs.bimDraws[1] = {.singleSided = &placeholderPoint,
                        .cpuFallbackAllowed = true};

  const auto plan = buildShadowCascadeDrawPlan(inputs);

  for (uint32_t cascadeIndex = 0; cascadeIndex < kShadowCascadeCount;
       ++cascadeIndex) {
    ASSERT_EQ(plan.bimSingleSided[cascadeIndex].size(), 1u);
    EXPECT_EQ(plan.bimSingleSided[cascadeIndex][0].objectIndex, 4u);
  }
}

TEST(ShadowCascadeDrawPlannerTests, SignatureTracksPolicyAndDrawChanges) {
  const std::vector<DrawCommand> first = {drawCommand(5u)};
  const std::vector<DrawCommand> second = {drawCommand(6u)};

  ShadowCascadeDrawPlannerInputs inputs{};
  inputs.sceneDraws = {.singleSided = &first};
  activateAllCascades(inputs);
  const uint64_t initial = computeShadowCascadeDrawSignature(inputs);

  inputs.shadowCullPassActive[0] = true;
  EXPECT_NE(computeShadowCascadeDrawSignature(inputs), initial);

  inputs.shadowCullPassActive[0] = false;
  inputs.sceneSingleSidedUsesGpuCull[0] = true;
  EXPECT_NE(computeShadowCascadeDrawSignature(inputs), initial);

  inputs.sceneSingleSidedUsesGpuCull[0] = false;
  inputs.sceneDraws = {.singleSided = &second};
  EXPECT_NE(computeShadowCascadeDrawSignature(inputs), initial);
}

} // namespace
