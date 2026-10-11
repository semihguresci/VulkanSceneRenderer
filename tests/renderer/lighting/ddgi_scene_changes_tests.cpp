#include "Container/renderer/lighting/DdgiSceneChanges.h"
#include <gtest/gtest.h>
#include <array>
#include <glm/gtc/matrix_transform.hpp>

namespace {
using namespace container::renderer;
struct Fixture {
  std::vector<container::geometry::Vertex> vertices{3};
  std::vector<uint32_t> indices{0, 1, 2};
  std::vector<container::gpu::ObjectData> objects{1};
  std::vector<DrawCommand> draws{{0, 0, 3, 1}};
  Fixture() {
    vertices[0].position = {0, 0, 0};
    vertices[1].position = {1, 0, 0};
    vertices[2].position = {0, 1, 0};
    objects[0].boundingSphere = {9000, 9000, 9000, 1};
  }
  RaySceneProviderSource source(std::string id = "mesh", uint64_t revision = 1) {
    return {{std::move(id)}, revision, vertices, indices, objects, draws};
  }
};
TEST(DdgiSceneChanges, InitialBoundsUseReferencedGeometryAndUnchangedSceneIsClean) {
  Fixture f; DdgiSceneChangeTracker tracker;
  const auto sources = std::array{f.source()};
  auto dirty = tracker.update(sources);
  ASSERT_EQ(dirty.size(), 1);
  EXPECT_EQ(dirty[0].minimum, glm::vec3(0));
  EXPECT_EQ(dirty[0].maximum, glm::vec3(1, 1, 0));
  EXPECT_TRUE(tracker.update(sources).empty());
}
TEST(DdgiSceneChanges, MovementDirtiesOldAndNewWorldBounds) {
  Fixture f; DdgiSceneChangeTracker tracker;
  const auto sources = std::array{f.source()};
  (void)tracker.update(sources);
  f.objects[0].model = glm::translate(glm::mat4(1), {10, 0, 0});
  const auto dirty = tracker.update(sources);
  ASSERT_EQ(dirty.size(), 2);
  EXPECT_EQ(dirty[0].minimum.x, 0);
  EXPECT_EQ(dirty[1].minimum.x, 10);
  EXPECT_EQ(dirty[1].maximum.x, 11);
}
TEST(DdgiSceneChanges, RemovalAndReappearanceDoNotLoseOldCoverage) {
  Fixture f; DdgiSceneChangeTracker tracker;
  (void)tracker.update(std::array{f.source()});
  auto dirty = tracker.update({});
  ASSERT_EQ(dirty.size(), 1);
  EXPECT_EQ(dirty[0].maximum.x, 1);
  EXPECT_TRUE(tracker.update({}).empty());
  EXPECT_EQ(tracker.update(std::array{f.source()}).size(), 1);
}
TEST(DdgiSceneChanges, MaterialAndGeometryRevisionsRefreshWithoutTransformChanges) {
  Fixture f; DdgiSceneChangeTracker tracker;
  uint64_t material = 1;
  const auto revision = [&](uint32_t) { return material; };
  (void)tracker.update(std::array{f.source()}, revision);
  material = 2;
  EXPECT_EQ(tracker.update(std::array{f.source()}, revision).size(), 2);
  f.vertices[1].position.x = 5;
  auto dirty = tracker.update(std::array{f.source("mesh", 2)}, revision);
  ASSERT_EQ(dirty.size(), 2);
  EXPECT_EQ(dirty[0].maximum.x, 1);
  EXPECT_EQ(dirty[1].maximum.x, 5);
}
TEST(DdgiSceneChanges, DistinctProvidersAndDuplicateDrawsRemainDistinctObjects) {
  Fixture f; DdgiSceneChangeTracker tracker;
  f.draws.push_back(f.draws.front());
  EXPECT_EQ(tracker.update(std::array{f.source("mesh"), f.source("bim")}).size(), 2);
  EXPECT_TRUE(tracker.update(std::array{f.source("mesh"), f.source("bim")}).empty());
  EXPECT_EQ(tracker.update(std::array{f.source("bim")}).size(), 1);
}
TEST(DdgiSceneChanges, VisibilityAndInstancedObjectChangesUseSemanticDraws) {
  Fixture f; DdgiSceneChangeTracker tracker;
  f.objects.resize(2);
  f.objects[1].model = glm::translate(glm::mat4(1), {0, 20, 0});
  f.draws[0].instanceCount = 2;
  auto dirty = tracker.update(std::array{f.source()});
  ASSERT_EQ(dirty.size(), 2);
  EXPECT_EQ(dirty[1].minimum.y, 20);
  f.draws[0].instanceCount = 1;
  dirty = tracker.update(std::array{f.source()});
  ASSERT_EQ(dirty.size(), 1);
  EXPECT_EQ(dirty[0].minimum.y, 20);
}
TEST(DdgiSceneChanges, InvalidDrawRangesFailBeforeEnumeratingInstances) {
  Fixture f; DdgiSceneChangeTracker tracker;
  f.draws[0].instanceCount = UINT32_MAX;
  EXPECT_THROW((void)tracker.update(std::array{f.source()}), std::invalid_argument);
  f.draws[0].indexCount = 0;
  EXPECT_TRUE(tracker.update(std::array{f.source()}).empty());
}
} // namespace
