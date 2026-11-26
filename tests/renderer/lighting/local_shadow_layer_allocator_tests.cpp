#include <gtest/gtest.h>

#include "Container/renderer/lighting/LocalShadowLayerAllocator.h"

#include <limits>
#include <vector>

namespace {

container::gpu::PointLightData makeLocalShadowLight(bool spot) {
  container::gpu::PointLightData light{};
  light.positionRadius.w = 4.0f;
  light.colorIntensity.a = 1.0f;
  light.coneOuterCosType.y = spot ? 1.0f : 0.0f;
  return light;
}

void expectMetadata(const container::gpu::PointLightData &light,
                    uint32_t baseLayerPlusOne, uint32_t layerCount) {
  EXPECT_FLOAT_EQ(light.coneOuterCosType.z,
                  static_cast<float>(baseLayerPlusOne));
  EXPECT_FLOAT_EQ(light.coneOuterCosType.w, static_cast<float>(layerCount));
}

} // namespace

TEST(LocalShadowLayerAllocatorTests, ZeroLayerBudgetClearsPointMetadata) {
  std::vector<container::gpu::PointLightData> lights{
      makeLocalShadowLight(false), makeLocalShadowLight(true)};
  lights[0].coneOuterCosType.z = 7.0f;
  lights[0].coneOuterCosType.w = 6.0f;
  lights[1].coneOuterCosType.z = 13.0f;
  lights[1].coneOuterCosType.w = 1.0f;

  const auto result = container::renderer::assignLocalShadowLayerMetadata(
      lights, {.omniPointBudget = 4u, .layerBudget = 0u});

  EXPECT_EQ(result.assignedOmniPointCount, 0u);
  EXPECT_EQ(result.assignedSpotCount, 0u);
  EXPECT_EQ(result.usedLayerCount, 0u);
  expectMetadata(lights[0], 0u, 0u);
  expectMetadata(lights[1], 0u, 0u);
}

TEST(LocalShadowLayerAllocatorTests,
     ZeroPointBudgetStillAllowsSpotLightMetadata) {
  std::vector<container::gpu::PointLightData> lights;
  for (uint32_t i = 0; i < 6u; ++i) {
    lights.push_back(makeLocalShadowLight(true));
  }
  lights.push_back(makeLocalShadowLight(false));

  const auto result = container::renderer::assignLocalShadowLayerMetadata(
      lights, {.omniPointBudget = 0u, .layerBudget = 24u});

  EXPECT_EQ(result.assignedOmniPointCount, 0u);
  EXPECT_EQ(result.assignedSpotCount, 6u);
  EXPECT_EQ(result.usedLayerCount, 6u);
  for (uint32_t i = 0; i < 6u; ++i) {
    expectMetadata(lights[i], i + 1u, 1u);
  }
  expectMetadata(lights.back(), 0u, 0u);
}

TEST(LocalShadowLayerAllocatorTests, PointLightsConsumeSixLayersAndBudget) {
  std::vector<container::gpu::PointLightData> lights;
  for (uint32_t i = 0; i < 5u; ++i) {
    lights.push_back(makeLocalShadowLight(false));
  }

  const auto result = container::renderer::assignLocalShadowLayerMetadata(
      lights, {.omniPointBudget = 4u, .layerBudget = 24u});

  EXPECT_EQ(result.assignedOmniPointCount, 4u);
  EXPECT_EQ(result.assignedSpotCount, 0u);
  EXPECT_EQ(result.usedLayerCount, 24u);
  expectMetadata(lights[0], 1u, 6u);
  expectMetadata(lights[1], 7u, 6u);
  expectMetadata(lights[2], 13u, 6u);
  expectMetadata(lights[3], 19u, 6u);
  expectMetadata(lights[4], 0u, 0u);
}

TEST(LocalShadowLayerAllocatorTests, MixedPointAndSpotLightsShareLayerBudget) {
  std::vector<container::gpu::PointLightData> lights{
      makeLocalShadowLight(true),
      makeLocalShadowLight(false),
      makeLocalShadowLight(true),
      makeLocalShadowLight(false),
  };

  const auto result = container::renderer::assignLocalShadowLayerMetadata(
      lights, {.omniPointBudget = 4u, .layerBudget = 8u});

  EXPECT_EQ(result.assignedOmniPointCount, 1u);
  EXPECT_EQ(result.assignedSpotCount, 2u);
  EXPECT_EQ(result.usedLayerCount, 8u);
  expectMetadata(lights[0], 1u, 1u);
  expectMetadata(lights[1], 2u, 6u);
  expectMetadata(lights[2], 8u, 1u);
  expectMetadata(lights[3], 0u, 0u);
}

TEST(LocalShadowLayerAllocatorTests, UnboundedIesUsesRemainingOmniShadows) {
  auto ordinaryUnbounded = makeLocalShadowLight(false);
  ordinaryUnbounded.positionRadius.w = 0.0f;
  auto measured = ordinaryUnbounded;
  measured.iesProfileAddress = 1u;
  std::vector<container::gpu::PointLightData> lights{
      ordinaryUnbounded, measured, makeLocalShadowLight(false)};

  const auto capped = container::renderer::assignLocalShadowLayerMetadata(
      lights, {.omniPointBudget = 1u, .layerBudget = 12u});
  EXPECT_EQ(capped.assignedOmniPointCount, 1u);
  expectMetadata(lights[0], 0u, 0u);
  expectMetadata(lights[1], 0u, 0u);
  expectMetadata(lights[2], 1u, 6u);

  const auto expanded = container::renderer::assignLocalShadowLayerMetadata(
      lights, {.omniPointBudget = 2u, .layerBudget = 12u});
  EXPECT_EQ(expanded.assignedOmniPointCount, 2u);
  expectMetadata(lights[0], 0u, 0u);
  expectMetadata(lights[1], 7u, 6u);
  expectMetadata(lights[2], 1u, 6u);
  EXPECT_FLOAT_EQ(lights[1].positionRadius.w, 0.0f);
}

TEST(LocalShadowLayerAllocatorTests, IesCubeFilterFollowsBoundedCasterGeometry) {
  using container::renderer::ShadowCasterSceneBounds;
  using container::renderer::localPointShadowFilterRange;
  using container::renderer::kMaxIesRasterShadowDistance;

  auto light = makeLocalShadowLight(false);
  light.positionRadius = {1.0f, 2.0f, 3.0f, 0.0f};
  const ShadowCasterSceneBounds localScene{{0.0f, 0.0f, 0.0f}, {4.0f, 6.0f, 15.0f}};
  EXPECT_FLOAT_EQ(localPointShadowFilterRange(light, &localScene), 0.0f);
  light.iesProfileAddress = 1u;
  EXPECT_FLOAT_EQ(localPointShadowFilterRange(light, &localScene), 14.3f);
  EXPECT_FLOAT_EQ(localPointShadowFilterRange(light, nullptr), kMaxIesRasterShadowDistance);

  const ShadowCasterSceneBounds oversizedScene{{-1.0e38f, -4.0f, -4.0f}, {1.0e38f, 4.0f, 4.0f}};
  EXPECT_FLOAT_EQ(localPointShadowFilterRange(light, &oversizedScene), kMaxIesRasterShadowDistance);
  const ShadowCasterSceneBounds invalidScene{{0.0f, 3.0f, 0.0f}, {1.0f, 2.0f, 1.0f}};
  EXPECT_FLOAT_EQ(localPointShadowFilterRange(light, &invalidScene), kMaxIesRasterShadowDistance);
  light.positionRadius.x = std::numeric_limits<float>::infinity();
  EXPECT_FLOAT_EQ(localPointShadowFilterRange(light, &localScene), 0.0f);
  EXPECT_FLOAT_EQ(localPointShadowFilterRange(makeLocalShadowLight(false), nullptr), 4.0f);
}
