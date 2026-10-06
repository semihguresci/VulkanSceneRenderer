#include "Container/renderer/raytracing/RayScene.h"
#include "Container/renderer/raytracing/RaySceneExtraction.h"
#include "Container/renderer/raytracing/RayShadowSettings.h"
#include "Container/utility/RayQuerySupport.h"
#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>
#include <limits>

using namespace container::renderer;

TEST(RayQuerySupport, EveryDependencyIsRequired) {
  const container::gpu::RayQuerySupport complete{true, true, true,
                                                 true, true, true};
  EXPECT_TRUE(complete.supported());
  EXPECT_TRUE(complete.unavailableReason().empty());
  for (int missing = 0; missing < 6; ++missing) {
    auto support = complete;
    switch (missing) {
    case 0:
      support.accelerationStructureExtension = false;
      break;
    case 1:
      support.rayQueryExtension = false;
      break;
    case 2:
      support.deferredHostOperationsExtension = false;
      break;
    case 3:
      support.accelerationStructureFeature = false;
      break;
    case 4:
      support.rayQueryFeature = false;
      break;
    case 5:
      support.bufferDeviceAddressEnabled = false;
      break;
    }
    EXPECT_FALSE(support.supported());
    EXPECT_FALSE(support.unavailableReason().empty());
  }
}

TEST(RayScene, ConvertsAffineTransformToVulkanRowsWithoutChangingHandedness) {
  const auto transform = glm::translate(glm::mat4(1), glm::vec3(7, 8, 9)) *
                         glm::rotate(glm::mat4(1), 0.6f, glm::vec3(0, 1, 0)) *
                         glm::scale(glm::mat4(1), glm::vec3(-2, 3, 4));
  const auto packed = rayInstanceTransform(transform);
  const glm::vec4 point(2, -1, 5, 1);
  const auto expected = transform * point;
  for (int row = 0; row < 3; ++row) {
    float actual = 0;
    for (int column = 0; column < 4; ++column)
      actual += packed.matrix[row][column] * point[column];
    EXPECT_FLOAT_EQ(actual, expected[row]);
  }
  EXPECT_EQ(rayInstanceFlags(false), 0u);
  EXPECT_EQ(rayInstanceFlags(true),
            VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR);
}

TEST(RayScene, RejectsInvalidTransformsAndAcceptsSmallInvertibleScales) {
  EXPECT_NO_THROW(
      (void)rayInstanceTransform(glm::scale(glm::mat4(1), glm::vec3(1e-9f))));
  EXPECT_THROW(
      (void)rayInstanceTransform(glm::scale(glm::mat4(1), glm::vec3(0, 1, 1))),
      std::invalid_argument);
  auto transform = glm::mat4(1);
  transform[0][3] = 0.1f;
  EXPECT_THROW((void)rayInstanceTransform(transform), std::invalid_argument);
  transform = glm::mat4(1);
  transform[3][0] = std::numeric_limits<float>::quiet_NaN();
  EXPECT_THROW((void)rayInstanceTransform(transform), std::invalid_argument);
}

TEST(RayScene, ValidatesGeometryAndProviderLocalIdentities) {
  std::vector<container::geometry::Vertex> vertices(3);
  std::vector<uint32_t> indices{0, 1, 2};
  std::vector<RaySceneGeometry> geometries{
      {{"mesh"}, 1, 1, vertices, indices},
      {{"bim-usd"}, 1, 1, vertices, indices}};
  std::vector<RaySceneInstance> instances{{0, 11}, {1, 12}};
  EXPECT_NO_THROW(validateRayScene({geometries, instances}));
  geometries[1].provider.value = "mesh";
  EXPECT_THROW(validateRayScene({geometries, instances}),
               std::invalid_argument);
  geometries.pop_back();
  instances.pop_back();
  indices[2] = 3;
  EXPECT_THROW(validateRayScene({geometries, instances}),
               std::invalid_argument);
  indices[2] = 2;
  geometries[0].indices = std::span(indices).first(2);
  EXPECT_THROW(validateRayScene({geometries, instances}),
               std::invalid_argument);
  geometries[0].indices = indices;
  vertices[0].position.x = std::numeric_limits<float>::infinity();
  EXPECT_THROW(validateRayScene({geometries, instances}),
               std::invalid_argument);
  vertices[0].position.x = 0;
  instances[0].customIndex = 0x1000000u;
  EXPECT_THROW(validateRayScene({geometries, instances}),
               std::invalid_argument);
  instances[0].customIndex = 0;
  instances[0].geometryIndex = 1;
  EXPECT_THROW(validateRayScene({geometries, instances}),
               std::invalid_argument);
  EXPECT_NO_THROW(validateRayScene({}));
}

TEST(RayExtraction, CompactsReferencedVerticesAndSharesInstancedGeometry) {
  std::vector<container::geometry::Vertex> vertices(100);
  vertices[77].position = {0, 0, 0};
  vertices[88].position = {1, 0, 0};
  vertices[99].position = {0, 1, 0};
  vertices[88].texCoord = {0.7f, 0.9f};
  vertices[88].texCoord1 = {0.2f, 0.3f};
  const std::vector<uint32_t> indices{77, 88, 99};
  std::vector<container::gpu::ObjectData> objects(2);
  objects[0].objectInfo = {13, 1, 0, 0};
  objects[1].objectInfo = {29, 4, 0, 0};
  objects[1].model = glm::translate(glm::mat4(1), glm::vec3(1000, 2, 3)) *
                     glm::scale(glm::mat4(1), glm::vec3(-2, 3, 4));
  const std::vector<DrawCommand> draws{{0, 0, 3, 2}};
  const std::array providers{
      RaySceneProviderSource{{"mesh"}, 7, vertices, indices, objects, draws}};
  auto scene = extractRayScene(providers);
  ASSERT_EQ(scene.geometry.size(), 1u);
  EXPECT_EQ(scene.geometry[0].vertices.size(), 3u);
  EXPECT_EQ(scene.geometry[0].indices, (std::vector<uint32_t>{0, 1, 2}));
  EXPECT_EQ(scene.geometry[0].revision, 7u);
  ASSERT_EQ(scene.instances.size(), 2u);
  EXPECT_EQ(scene.instances[0].geometryIndex, scene.instances[1].geometryIndex);
  EXPECT_FALSE(scene.instances[0].doubleSided);
  EXPECT_TRUE(scene.instances[1].doubleSided);
  EXPECT_EQ(scene.instances[1].transform, objects[1].model);
  EXPECT_EQ(scene.instanceData[0].info.y, 13u);
  EXPECT_EQ(scene.instanceData[1].info.y, 29u);
  EXPECT_EQ(scene.triangles[0].uv[1], glm::vec4(0.7f, 0.9f, 0.2f, 0.3f));
  // Moving the owning snapshot must not leave dangling spans in its input.
  auto moved = std::move(scene);
  auto views = moved.geometryViews();
  EXPECT_NO_THROW(validateRayScene({views, moved.instances}));
  EXPECT_FALSE(views[0].opaque);
}

TEST(RayExtraction, ProviderNamespacesAndDistinctRangesStayIndependent) {
  const std::vector<container::geometry::Vertex> vertices(4);
  const std::vector<uint32_t> indices{0, 1, 2, 0, 2, 3};
  const std::vector<container::gpu::ObjectData> objects(1);
  const std::vector<DrawCommand> mesh{{0, 0, 3}, {0, 3, 3}, {0, 0, 3}};
  const std::vector<DrawCommand> bim{{0, 0, 6}};
  const std::array providers{
      RaySceneProviderSource{{"mesh"}, 2, vertices, indices, objects, mesh},
      RaySceneProviderSource{{"bim"}, 8, vertices, indices, objects, bim}};
  const auto scene = extractRayScene(providers);
  ASSERT_EQ(scene.geometry.size(), 3u);
  EXPECT_EQ(scene.instances.size(), 4u);
  EXPECT_EQ(scene.triangles.size(), 4u);
  EXPECT_EQ(scene.instanceData[0].info.x, 0u);
  EXPECT_EQ(scene.instanceData[1].info.x, 1u);
  EXPECT_EQ(scene.instanceData[2].info.x, 0u);
  EXPECT_EQ(scene.instanceData[3].info.x, 2u);
  EXPECT_EQ(scene.geometry[2].revision, 8u);
}

TEST(RayExtraction, RejectsBadDrawsBeforeAnyGpuRecording) {
  const std::vector<container::geometry::Vertex> vertices(3);
  std::vector<uint32_t> indices{0, 1, 2};
  const std::vector<container::gpu::ObjectData> objects(1);
  std::vector<DrawCommand> draws{{0, 0, 3}};
  std::array providers{
      RaySceneProviderSource{{"mesh"}, 1, vertices, indices, objects, draws}};
  draws[0].firstIndex = std::numeric_limits<uint32_t>::max();
  EXPECT_THROW(extractRayScene(providers), std::invalid_argument);
  draws[0] = {0, 0, 2};
  EXPECT_THROW(extractRayScene(providers), std::invalid_argument);
  draws[0] = {0, 0, 3, 2};
  EXPECT_THROW(extractRayScene(providers), std::invalid_argument);
  draws[0] = {0, 0, 3};
  indices[2] = 3;
  EXPECT_THROW(extractRayScene(providers), std::invalid_argument);
  indices[2] = 2;
  auto duplicate = std::vector{providers[0], providers[0]};
  EXPECT_THROW(extractRayScene(duplicate), std::invalid_argument);
  EXPECT_TRUE(extractRayScene({}).geometry.empty());
}

TEST(RayShadows, BudgetsAreFiniteAndInvalidModesPreserveRasterFallback) {
  auto settings = sanitizeRayShadowSettings(
      {static_cast<RayShadowMode>(9), 999, 999, true, 99});
  EXPECT_EQ(settings.mode, RayShadowMode::Raster);
  EXPECT_EQ(settings.areaSamples, 32u);
  EXPECT_EQ(settings.localLightBudget, 8u);
  EXPECT_EQ(settings.debugLayer, 10u);
  settings.areaSamples = 0;
  EXPECT_EQ(sanitizeRayShadowSettings(settings).areaSamples, 1u);
}
