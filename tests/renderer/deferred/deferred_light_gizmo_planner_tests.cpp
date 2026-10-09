#include "Container/renderer/deferred/DeferredLightGizmoPlanner.h"
#include "Container/renderer/lighting/EditableLight.h"
#include "Container/renderer/lighting/LightGizmoIconAtlas.h"

#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <vector>

namespace {

using container::renderer::buildDeferredLightGizmoPlan;
using container::renderer::decodeEditableLightPickId;
using container::renderer::DeferredLightGizmoPlanInputs;
using container::renderer::encodeEditableLightPickId;
using container::renderer::kLightGizmoCoverageArea;
using container::renderer::kLightGizmoCoverageDirectional;
using container::renderer::kLightGizmoCoveragePoint;
using container::renderer::kLightGizmoCoverageSceneRadiusScale;
using container::renderer::kLightGizmoCoverageSpot;
using container::renderer::kLightGizmoDirectionalCoverageSceneRadiusScale;
using container::renderer::kLightGizmoMaxIconExtent;
using container::renderer::kLightGizmoMinIconExtent;
using container::renderer::kMaxDeferredLightGizmos;
using container::renderer::lightGizmoIconLayerForType;

void expectVec3Near(const glm::vec3 &actual, const glm::vec3 &expected) {
  EXPECT_NEAR(actual.x, expected.x, 1.0e-5f);
  EXPECT_NEAR(actual.y, expected.y, 1.0e-5f);
  EXPECT_NEAR(actual.z, expected.z, 1.0e-5f);
}

container::gpu::PointLightData pointLight(const glm::vec3 &position,
                                          const glm::vec3 &color) {
  container::gpu::PointLightData light{};
  light.positionRadius = glm::vec4(position, 10.0f);
  light.colorIntensity = glm::vec4(color, 5.0f);
  return light;
}

container::renderer::EditableLightEntity
editableLight(container::renderer::EditableLightType type, uint32_t index,
              const glm::vec3 &position, bool selected) {
  container::renderer::EditableLightEntity light{};
  light.id = {.type = type,
              .source = container::renderer::EditableLightSource::Manual,
              .index = index};
  light.type = type;
  light.source = container::renderer::EditableLightSource::Manual;
  light.position = position;
  light.direction = {0.0f, -1.0f, 0.0f};
  light.tangent = {1.0f, 0.0f, 0.0f};
  light.bitangent = {0.0f, 0.0f, 1.0f};
  light.color = {0.25f, 0.5f, 1.0f};
  light.intensity = 4.0f;
  light.range = 6.0f;
  light.areaHalfSize = {1.5f, 0.75f};
  light.selected = selected;
  return light;
}

} // namespace

TEST(DeferredLightGizmoPlannerTests, EmitsDirectionalGizmoWithoutPointLights) {
  DeferredLightGizmoPlanInputs inputs{};
  inputs.sceneCenter = {1.0f, 2.0f, 3.0f};
  inputs.sceneWorldRadius = 4.0f;
  inputs.cameraPosition = {1.0f, 2.0f, 3.0f};
  inputs.directionalDirection = {0.0f, -1.0f, 0.0f};
  inputs.directionalColor = {0.25f, 0.5f, 1.0f};

  const auto plan = buildDeferredLightGizmoPlan(inputs);

  ASSERT_EQ(plan.pushConstantCount, 1u);
  expectVec3Near(glm::vec3(plan.pushConstants[0].positionRadius),
                 glm::vec3(1.0f, 6.6f, 3.0f));
  EXPECT_FLOAT_EQ(plan.pushConstants[0].positionRadius.w,
                  kLightGizmoMinIconExtent);
  expectVec3Near(glm::vec3(plan.pushConstants[0].colorIntensity),
                 glm::vec3(0.9424f, 0.7392f, 0.2824f));
  EXPECT_FLOAT_EQ(plan.pushConstants[0].colorIntensity.w, 1.0f);
  ASSERT_EQ(plan.coveragePushConstantCount, 1u);
  EXPECT_EQ(plan.coveragePushConstants[0].localShadowEnabled,
            kLightGizmoCoverageDirectional);
  expectVec3Near(glm::vec3(plan.coveragePushConstants[0].directionInnerCos),
                 inputs.directionalDirection);
  const float arrowLength =
      inputs.sceneWorldRadius * kLightGizmoDirectionalCoverageSceneRadiusScale;
  EXPECT_FLOAT_EQ(plan.coveragePushConstants[0].positionRadius.w, arrowLength);
  EXPECT_FLOAT_EQ(plan.coveragePushConstants[0].directionInnerCos.w,
                  arrowLength * 0.13f);
  EXPECT_FLOAT_EQ(plan.coveragePushConstants[0].coneOuterCosType.w,
                  arrowLength * 0.24f);
}

TEST(DeferredLightGizmoPlannerTests, PointGizmosCopyPositionsAndUseExtentBias) {
  const std::array<container::gpu::PointLightData, 2> pointLights = {
      pointLight({2.0f, 0.0f, 0.0f}, {2.0f, 1.0f, 0.5f}),
      pointLight({0.0f, 4.0f, 0.0f}, {0.2f, 0.4f, 1.0f}),
  };

  DeferredLightGizmoPlanInputs inputs{};
  inputs.sceneWorldRadius = 10.0f;
  inputs.cameraPosition = {0.0f, 0.0f, 0.0f};
  inputs.pointLights = pointLights;

  const auto plan = buildDeferredLightGizmoPlan(inputs);

  ASSERT_EQ(plan.pushConstantCount, 3u);
  expectVec3Near(glm::vec3(plan.pushConstants[1].positionRadius),
                 glm::vec3(2.0f, 0.0f, 0.0f));
  expectVec3Near(glm::vec3(plan.pushConstants[2].positionRadius),
                 glm::vec3(0.0f, 4.0f, 0.0f));
  EXPECT_NEAR(plan.pushConstants[1].positionRadius.w, 0.09f, 1.0e-5f);
  EXPECT_NEAR(plan.pushConstants[2].positionRadius.w, 0.105f, 1.0e-5f);
}

TEST(DeferredLightGizmoPlannerTests,
     IconExtentsStaySubtleAtNearAndFarDistances) {
  EXPECT_FLOAT_EQ(kLightGizmoMinIconExtent, 0.07f);
  EXPECT_FLOAT_EQ(kLightGizmoMaxIconExtent, 1.1f);
  EXPECT_FLOAT_EQ(kLightGizmoCoverageSceneRadiusScale, 0.14f);
  EXPECT_FLOAT_EQ(kLightGizmoDirectionalCoverageSceneRadiusScale, 0.16f);
}

TEST(DeferredLightGizmoPlannerTests,
     PointColorsBlendAuthoredHueWithTypeAccent) {
  const std::array<container::gpu::PointLightData, 1> pointLights = {
      pointLight({0.0f, 0.0f, 0.0f}, {2.0f, 1.0f, 0.0f}),
  };
  DeferredLightGizmoPlanInputs inputs{};
  inputs.pointLights = pointLights;

  const auto plan = buildDeferredLightGizmoPlan(inputs);

  ASSERT_EQ(plan.pushConstantCount, 2u);
  expectVec3Near(glm::vec3(plan.pushConstants[1].colorIntensity),
                 glm::vec3(0.571f, 0.7808f, 0.8416f));
}

TEST(DeferredLightGizmoPlannerTests, PointGizmoCountClampsToMaximum) {
  std::vector<container::gpu::PointLightData> pointLights(
      kMaxDeferredLightGizmos + 8u);
  for (uint32_t i = 0u; i < pointLights.size(); ++i) {
    pointLights[i] =
        pointLight({static_cast<float>(i), 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f});
  }

  DeferredLightGizmoPlanInputs inputs{};
  inputs.pointLights = pointLights;

  const auto plan = buildDeferredLightGizmoPlan(inputs);

  EXPECT_EQ(plan.pushConstantCount, kMaxDeferredLightGizmos + 1u);
}

TEST(DeferredLightGizmoPlannerTests, ExtentsClampToSupportedRange) {
  const std::array<container::gpu::PointLightData, 2> pointLights = {
      pointLight({1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}),
      pointLight({1000.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}),
  };
  DeferredLightGizmoPlanInputs inputs{};
  inputs.sceneWorldRadius = 1.0f;
  inputs.cameraPosition = {0.0f, 0.0f, 0.0f};
  inputs.pointLights = pointLights;

  const auto plan = buildDeferredLightGizmoPlan(inputs);

  ASSERT_EQ(plan.pushConstantCount, 3u);
  EXPECT_LT(plan.pushConstants[1].positionRadius.w, 0.25f);
  EXPECT_FLOAT_EQ(plan.pushConstants[2].positionRadius.w,
                  kLightGizmoMaxIconExtent);
}

TEST(DeferredLightGizmoPlannerTests,
     EditableLightGizmosEncodeStableGpuPickIds) {
  const std::array editableLights = {
      editableLight(container::renderer::EditableLightType::Directional, 0u,
                    {0.0f, 5.0f, 0.0f}, false),
      editableLight(container::renderer::EditableLightType::Spot, 2u,
                    {2.0f, 1.0f, 0.0f}, true),
      editableLight(container::renderer::EditableLightType::Area, 3u,
                    {-2.0f, 1.0f, 0.0f}, false),
  };

  DeferredLightGizmoPlanInputs inputs{};
  inputs.sceneWorldRadius = 8.0f;
  inputs.cameraPosition = {0.0f, 0.0f, 0.0f};
  inputs.editableLights = editableLights;

  const auto plan = buildDeferredLightGizmoPlan(inputs);

  ASSERT_EQ(plan.pushConstantCount, editableLights.size());
  ASSERT_NE(plan.pushConstants[0].padding2, container::gpu::kPickIdNone);
  ASSERT_NE(plan.pushConstants[1].padding2, container::gpu::kPickIdNone);
  ASSERT_NE(plan.pushConstants[2].padding2, container::gpu::kPickIdNone);
  EXPECT_EQ(decodeEditableLightPickId(plan.pushConstants[0].padding2),
            editableLights[0].id);
  EXPECT_EQ(decodeEditableLightPickId(plan.pushConstants[1].padding2),
            editableLights[1].id);
  EXPECT_EQ(decodeEditableLightPickId(plan.pushConstants[2].padding2),
            editableLights[2].id);
}

TEST(DeferredLightGizmoPlannerTests,
     PushConstantsStoreStableIconLayersPerLightType) {
  const std::array editableLights = {
      editableLight(container::renderer::EditableLightType::Directional, 0u,
                    {0.0f, 5.0f, 0.0f}, false),
      editableLight(container::renderer::EditableLightType::Point, 1u,
                    {1.0f, 2.0f, 0.0f}, false),
      editableLight(container::renderer::EditableLightType::Spot, 2u,
                    {2.0f, 1.0f, 0.0f}, false),
      editableLight(container::renderer::EditableLightType::Area, 3u,
                    {-2.0f, 1.0f, 0.0f}, false),
  };

  DeferredLightGizmoPlanInputs inputs{};
  inputs.sceneWorldRadius = 8.0f;
  inputs.cameraPosition = {0.0f, 0.0f, 0.0f};
  inputs.editableLights = editableLights;

  const auto plan = buildDeferredLightGizmoPlan(inputs);

  ASSERT_EQ(plan.pushConstantCount, editableLights.size());
  EXPECT_FLOAT_EQ(plan.pushConstants[0].coneOuterCosType.x,
                  static_cast<float>(lightGizmoIconLayerForType(
                      container::renderer::EditableLightType::Directional)));
  EXPECT_FLOAT_EQ(plan.pushConstants[1].coneOuterCosType.x,
                  static_cast<float>(lightGizmoIconLayerForType(
                      container::renderer::EditableLightType::Point)));
  EXPECT_FLOAT_EQ(plan.pushConstants[2].coneOuterCosType.x,
                  static_cast<float>(lightGizmoIconLayerForType(
                      container::renderer::EditableLightType::Spot)));
  EXPECT_FLOAT_EQ(plan.pushConstants[3].coneOuterCosType.x,
                  static_cast<float>(lightGizmoIconLayerForType(
                      container::renderer::EditableLightType::Area)));
}

TEST(DeferredLightGizmoPlannerTests,
     EditableLightGizmosUseReadableTypeColorPalette) {
  const std::array editableLights = {
      editableLight(container::renderer::EditableLightType::Directional, 0u,
                    {0.0f, 5.0f, 0.0f}, false),
      editableLight(container::renderer::EditableLightType::Point, 1u,
                    {1.0f, 2.0f, 0.0f}, false),
      editableLight(container::renderer::EditableLightType::Spot, 2u,
                    {2.0f, 1.0f, 0.0f}, false),
      editableLight(container::renderer::EditableLightType::Area, 3u,
                    {-2.0f, 1.0f, 0.0f}, false),
  };

  DeferredLightGizmoPlanInputs inputs{};
  inputs.sceneWorldRadius = 8.0f;
  inputs.cameraPosition = {0.0f, 0.0f, 0.0f};
  inputs.editableLights = editableLights;

  const auto plan = buildDeferredLightGizmoPlan(inputs);

  ASSERT_EQ(plan.pushConstantCount, editableLights.size());
  expectVec3Near(glm::vec3(plan.pushConstants[0].colorIntensity),
                 glm::vec3(0.9424f, 0.7392f, 0.2824f));
  expectVec3Near(glm::vec3(plan.pushConstants[1].colorIntensity),
                 glm::vec3(0.4126f, 0.7808f, 1.0f));
  expectVec3Near(glm::vec3(plan.pushConstants[2].colorIntensity),
                 glm::vec3(0.632f, 0.5528f, 1.0f));
  expectVec3Near(glm::vec3(plan.pushConstants[3].colorIntensity),
                 glm::vec3(0.928f, 0.464f, 0.73f));
}

TEST(DeferredLightGizmoPlannerTests,
     CoveragePushConstantsDescribePointSpotAndAreaExtents) {
  auto point = editableLight(container::renderer::EditableLightType::Point, 1u,
                             {1.0f, 2.0f, 0.0f}, false);
  point.range = 6.0f;
  auto spot = editableLight(container::renderer::EditableLightType::Spot, 2u,
                            {2.0f, 1.0f, 0.0f}, false);
  spot.range = 10.0f;
  spot.outerConeDegrees = 30.0f;
  spot.direction = {0.0f, 0.0f, -1.0f};
  spot.tangent = {1.0f, 0.0f, 0.0f};
  auto area = editableLight(container::renderer::EditableLightType::Area, 3u,
                            {-2.0f, 1.0f, 0.0f}, false);
  area.areaHalfSize = {1.5f, 0.75f};
  area.direction = {0.0f, -1.0f, 0.0f};
  area.tangent = {1.0f, 0.0f, 0.0f};

  const std::array editableLights = {point, spot, area};
  DeferredLightGizmoPlanInputs inputs{};
  inputs.sceneWorldRadius = 8.0f;
  inputs.cameraPosition = {0.0f, 0.0f, 0.0f};
  inputs.editableLights = editableLights;

  const auto plan = buildDeferredLightGizmoPlan(inputs);

  ASSERT_EQ(plan.coveragePushConstantCount, 3u);
  EXPECT_EQ(plan.coveragePushConstants[0].localShadowEnabled,
            kLightGizmoCoveragePoint);
  const float compactCoverageRange =
      inputs.sceneWorldRadius * kLightGizmoCoverageSceneRadiusScale;
  EXPECT_FLOAT_EQ(plan.coveragePushConstants[0].positionRadius.w,
                  compactCoverageRange);
  EXPECT_FLOAT_EQ(plan.coveragePushConstants[0].colorIntensity.w, 0.36f);

  EXPECT_EQ(plan.coveragePushConstants[1].localShadowEnabled,
            kLightGizmoCoverageSpot);
  EXPECT_FLOAT_EQ(plan.coveragePushConstants[1].positionRadius.w,
                  compactCoverageRange);
  EXPECT_FLOAT_EQ(plan.coveragePushConstants[1].colorIntensity.w, 0.36f);
  EXPECT_NEAR(plan.coveragePushConstants[1].coneOuterCosType.w,
              compactCoverageRange *
                  std::tan(glm::radians(spot.outerConeDegrees)),
              1.0e-5f);
  expectVec3Near(glm::vec3(plan.coveragePushConstants[1].coneOuterCosType),
                 spot.tangent);

  EXPECT_EQ(plan.coveragePushConstants[2].localShadowEnabled,
            kLightGizmoCoverageArea);
  EXPECT_FLOAT_EQ(plan.coveragePushConstants[2].directionInnerCos.w,
                  area.areaHalfSize.x * 0.5f);
  EXPECT_FLOAT_EQ(plan.coveragePushConstants[2].coneOuterCosType.w,
                  area.areaHalfSize.y * 0.5f);
  EXPECT_FLOAT_EQ(plan.coveragePushConstants[2].colorIntensity.w, 0.36f);
  expectVec3Near(glm::vec3(plan.coveragePushConstants[2].coneOuterCosType),
                 area.tangent);
}

TEST(DeferredLightGizmoPlannerTests,
     DirectionalCoveragePushConstantDescribesLightDirection) {
  auto directional =
      editableLight(container::renderer::EditableLightType::Directional, 0u,
                    {0.0f, 5.0f, 0.0f}, false);
  directional.direction = {0.0f, -2.0f, 0.0f};
  directional.tangent = {1.0f, 0.0f, 0.0f};

  const std::array editableLights = {directional};
  DeferredLightGizmoPlanInputs inputs{};
  inputs.sceneWorldRadius = 8.0f;
  inputs.cameraPosition = {0.0f, 0.0f, 0.0f};
  inputs.editableLights = editableLights;

  const auto plan = buildDeferredLightGizmoPlan(inputs);

  ASSERT_EQ(plan.coveragePushConstantCount, 1u);
  EXPECT_EQ(plan.coveragePushConstants[0].localShadowEnabled,
            kLightGizmoCoverageDirectional);
  expectVec3Near(glm::vec3(plan.coveragePushConstants[0].positionRadius),
                 directional.position);
  expectVec3Near(glm::vec3(plan.coveragePushConstants[0].directionInnerCos),
                 glm::vec3(0.0f, -1.0f, 0.0f));
  expectVec3Near(glm::vec3(plan.coveragePushConstants[0].coneOuterCosType),
                 directional.tangent);
  const float arrowLength =
      inputs.sceneWorldRadius * kLightGizmoDirectionalCoverageSceneRadiusScale;
  EXPECT_FLOAT_EQ(plan.coveragePushConstants[0].positionRadius.w, arrowLength);
  EXPECT_FLOAT_EQ(plan.coveragePushConstants[0].directionInnerCos.w,
                  arrowLength * 0.13f);
  EXPECT_FLOAT_EQ(plan.coveragePushConstants[0].coneOuterCosType.w,
                  arrowLength * 0.24f);
}

TEST(DeferredLightGizmoPlannerTests,
     SelectedEditableLightUsesEmphasizedMarker) {
  const std::array editableLights = {
      editableLight(container::renderer::EditableLightType::Point, 0u,
                    {2.0f, 0.0f, 0.0f}, false),
      editableLight(container::renderer::EditableLightType::Point, 1u,
                    {2.0f, 0.0f, 0.0f}, true),
  };

  DeferredLightGizmoPlanInputs inputs{};
  inputs.sceneWorldRadius = 4.0f;
  inputs.cameraPosition = {0.0f, 0.0f, 0.0f};
  inputs.editableLights = editableLights;

  const auto plan = buildDeferredLightGizmoPlan(inputs);

  ASSERT_EQ(plan.pushConstantCount, 2u);
  EXPECT_GT(plan.pushConstants[1].positionRadius.w,
            plan.pushConstants[0].positionRadius.w);
  EXPECT_GT(plan.pushConstants[1].colorIntensity.g,
            plan.pushConstants[0].colorIntensity.g);
}

TEST(DeferredLightGizmoPlannerTests, EditableLightPickIdsRoundTrip) {
  const container::renderer::EditableLightId id{
      .type = container::renderer::EditableLightType::Area,
      .source = container::renderer::EditableLightSource::Imported,
      .index = 42u};

  const uint32_t pickId = encodeEditableLightPickId(id);

  EXPECT_NE(pickId, container::gpu::kPickIdNone);
  EXPECT_NE(pickId & container::gpu::kPickIdLightMask, 0u);
  EXPECT_EQ(decodeEditableLightPickId(pickId), id);
}

TEST(DeferredLightGizmoPlannerTests, EditableLightPickIdsRejectInvalidValues) {
  container::renderer::EditableLightId invalid{
      .type = container::renderer::EditableLightType::Point,
      .source = container::renderer::EditableLightSource::Manual,
      .index = container::renderer::kInvalidEditableLightIndex};

  EXPECT_EQ(encodeEditableLightPickId(invalid), container::gpu::kPickIdNone);
  EXPECT_EQ(decodeEditableLightPickId(container::gpu::kPickIdNone),
            std::nullopt);
  EXPECT_EQ(decodeEditableLightPickId(1u), std::nullopt);
}

TEST(DeferredLightGizmoPlannerTests, EmitterDebugKeepsAuthoredSizeAndOrientation) {
  auto light = editableLight(container::renderer::EditableLightType::Area,
                             0u, {2.0f, 3.0f, 4.0f}, false);
  light.areaHalfSize = {7.0f, 0.003f};
  light.direction = {0.0f, 0.0f, -1.0f};
  light.tangent = {1.0f, 0.0f, 0.0f};
  const std::array lights{light};
  DeferredLightGizmoPlanInputs inputs{};
  inputs.editableLights = lights;
  inputs.areaEmitterDebug = true;
  const auto plan = buildDeferredLightGizmoPlan(inputs);
  ASSERT_EQ(plan.coveragePushConstantCount, 1u);
  const auto &coverage = plan.coveragePushConstants[0];
  EXPECT_FLOAT_EQ(coverage.directionInnerCos.w, 7.0f);
  EXPECT_FLOAT_EQ(coverage.coneOuterCosType.w, 0.003f);
  expectVec3Near(glm::vec3(coverage.positionRadius), light.position);
  expectVec3Near(glm::vec3(coverage.directionInnerCos), light.direction);
  expectVec3Near(glm::vec3(coverage.coneOuterCosType), light.tangent);
  EXPECT_FLOAT_EQ(coverage.bounceIntensity,
                  container::gpu::kAreaLightTypeRectangle);
}

TEST(DeferredLightGizmoPlannerTests, EmitterDebugPreservesDiskRadiusAndShape) {
  auto light = editableLight(container::renderer::EditableLightType::Area,
                             0u, {0.0f, 1.0f, 0.0f}, false);
  light.areaShape = container::gpu::kAreaLightTypeDisk;
  light.areaHalfSize = {3.0f, 0.1f};
  const std::array lights{light};
  DeferredLightGizmoPlanInputs inputs{};
  inputs.editableLights = lights;
  inputs.areaEmitterDebug = true;
  const auto plan = buildDeferredLightGizmoPlan(inputs);
  ASSERT_EQ(plan.coveragePushConstantCount, 1u);
  const auto &coverage = plan.coveragePushConstants[0];
  EXPECT_FLOAT_EQ(coverage.directionInnerCos.w, 3.0f);
  EXPECT_FLOAT_EQ(coverage.coneOuterCosType.w, 3.0f);
  EXPECT_FLOAT_EQ(coverage.bounceIntensity, container::gpu::kAreaLightTypeDisk);
}
