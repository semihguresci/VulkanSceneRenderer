#include "Container/renderer/bim/BimObjectTransforms.h"
#include "Container/scene/SceneProvider.h"

#include <gtest/gtest.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using container::gpu::ObjectData;
using container::gpu::ObjectTransformUpdate;
using container::renderer::BimElementBounds;
using container::renderer::BimElementMetadata;
using container::renderer::BimObjectTransforms;

glm::mat4 moved(float x, float y = 0, float z = 0) {
  return glm::translate(glm::mat4{1}, glm::vec3{x, y, z});
}

BimElementBounds box(glm::vec3 minimum = glm::vec3{-1},
                     glm::vec3 maximum = glm::vec3{1}) {
  return {.valid = true, .min = minimum, .max = maximum,
          .center = (minimum + maximum) * 0.5f,
          .size = maximum - minimum,
          .radius = glm::length(maximum - minimum) * 0.5f,
          .floorElevation = minimum.y};
}

class BimObjectTransformTests : public ::testing::Test {
protected:
  void SetUp() override {
    for (uint32_t index = 0; index < objects.size(); ++index) {
      metadata[index].objectIndex = index;
      metadata[index].guid = "fragment-" + std::to_string(index);
      metadata[index].bounds = box();
      objects[index].boundingSphere = {0, 0, 0, std::sqrt(3.0f)};
      objects[index].objectInfo = {7, 2, 0x80000000u, 9};
      objects[index].temporalInfo = {3, 1, 11, 12};
      objects[index].previousModel = moved(-3);
    }
  }

  bool apply(std::initializer_list<ObjectTransformUpdate> updates,
             glm::vec3 root = glm::vec3{0}) {
    return state.apply(objects, metadata,
                       std::span<const ObjectTransformUpdate>{updates.begin(), updates.size()}, root);
  }
  BimElementBounds bounds(uint32_t index, glm::vec3 root = glm::vec3{0}) {
    return state.boundsForObject(index, metadata[index].bounds, root);
  }
  void expectNear(glm::vec3 actual, glm::vec3 expected) {
    for (int axis = 0; axis < 3; ++axis) EXPECT_NEAR(actual[axis], expected[axis], 1e-5f);
  }
  void expectNormal(const ObjectData &object) {
    const auto expected = glm::transpose(glm::inverse(glm::mat3{object.model}));
    expectNear(glm::vec3{object.normalMatrix0}, expected[0]);
    expectNear(glm::vec3{object.normalMatrix1}, expected[1]);
    expectNear(glm::vec3{object.normalMatrix2}, expected[2]);
    EXPECT_EQ(object.normalMatrix0.w, 0);
    EXPECT_EQ(object.normalMatrix1.w, 0);
    EXPECT_EQ(object.normalMatrix2.w, 0);
  }

  BimObjectTransforms state;
  std::array<ObjectData, 2> objects;
  std::array<BimElementMetadata, 2> metadata;
};

TEST_F(BimObjectTransformTests, TranslationMovesBoundsAndPreservesRenderIdentity) {
  const auto untouched = objects[1];
  const auto original = objects[0];
  ASSERT_TRUE(apply({{0, moved(4, -2, 3)}}));
  expectNear(bounds(0).min, {3, -3, 2});
  expectNear(bounds(0).max, {5, -1, 4});
  expectNear(bounds(0).center, {4, -2, 3});
  EXPECT_EQ(bounds(0).floorElevation, -3);
  expectNear(glm::vec3{objects[0].boundingSphere}, bounds(0).center);
  EXPECT_NEAR(objects[0].boundingSphere.w, std::sqrt(3.0f), 1e-6f);
  expectNormal(objects[0]);
  EXPECT_EQ(objects[0].objectInfo, original.objectInfo);
  EXPECT_EQ(objects[0].temporalInfo, original.temporalInfo);
  EXPECT_EQ(objects[0].previousModel, original.previousModel);
  EXPECT_EQ(objects[1].model, untouched.model);
  EXPECT_EQ(metadata[0].bounds.min, glm::vec3{-1});
  EXPECT_EQ(metadata[0].guid, "fragment-0");
}

TEST_F(BimObjectTransformTests, RotationUsesOriginalWorldBoundsAndNormalMatrix) {
  metadata[0].bounds = box({-2, -1, -.5f}, {2, 1, .5f});
  const auto rotation = glm::rotate(glm::mat4{1}, glm::radians(90.0f), glm::vec3{0, 0, 1});
  ASSERT_TRUE(apply({{0, moved(3) * rotation}}));
  expectNear(bounds(0).min, {2, -2, -.5f});
  expectNear(bounds(0).max, {4, 2, .5f});
  expectNormal(objects[0]);
}

TEST_F(BimObjectTransformTests, NonuniformScaleUpdatesBoundsNormalsAndSphere) {
  const auto scaled = glm::scale(glm::mat4{1}, glm::vec3{2, .5f, 3});
  ASSERT_TRUE(apply({{0, scaled}}));
  expectNear(bounds(0).min, {-2, -.5f, -3});
  expectNear(bounds(0).max, {2, .5f, 3});
  expectNormal(objects[0]);
  EXPECT_NEAR(objects[0].boundingSphere.w, glm::length(glm::vec3{2, .5f, 3}), 1e-5f);
}

TEST_F(BimObjectTransformTests, ExistingMirroredTransformKeepsItsHandedness) {
  const auto mirror = glm::scale(glm::mat4{1}, glm::vec3{-1, 2, .5f});
  objects[0].model = mirror;
  metadata[0].bounds = box({-1, -2, -.5f}, {1, 2, .5f});
  const auto rotation = glm::rotate(glm::mat4{1}, glm::radians(90.0f), glm::vec3{0, 0, 1});
  ASSERT_TRUE(apply({{0, moved(4) * rotation * mirror}}));
  expectNear(bounds(0).min, {2, -1, -.5f});
  expectNear(bounds(0).max, {6, 1, .5f});
  expectNormal(objects[0]);
  EXPECT_LT(glm::determinant(glm::mat3{objects[0].model}), 0);
}

TEST_F(BimObjectTransformTests, WindingChangeRejectsEntireBatch) {
  const auto mirror = glm::scale(glm::mat4{1}, glm::vec3{-1, 1, 1});
  EXPECT_THROW(apply({{0, moved(3)}, {1, mirror}}), std::invalid_argument);
  EXPECT_EQ(objects[0].model, glm::mat4{1});
  EXPECT_EQ(objects[1].model, glm::mat4{1});
  EXPECT_EQ(state.changedObjectCount(), 0);
}

TEST_F(BimObjectTransformTests, InvalidIndexLeavesBothNewAndExistingRowsUntouched) {
  ASSERT_TRUE(apply({{0, moved(2)}}));
  const auto originalBounds = bounds(0);
  EXPECT_THROW(apply({{0, moved(6)}, {2, moved(10)}}), std::invalid_argument);
  EXPECT_EQ(objects[0].model, moved(2));
  EXPECT_EQ(bounds(0).min, originalBounds.min);
  EXPECT_EQ(state.changedObjectCount(), 1);
}

TEST_F(BimObjectTransformTests, SingularNonfiniteAndProjectiveTransformsRejectBatch) {
  std::vector<glm::mat4> invalid{
      glm::scale(glm::mat4{1}, glm::vec3{1, 0, 1}), glm::mat4{1}, glm::mat4{1}, glm::mat4{1}};
  invalid[1][1][0] = std::numeric_limits<float>::quiet_NaN();
  invalid[2][3][0] = std::numeric_limits<float>::infinity();
  invalid[3][0][3] = .1f;
  for (const auto &model : invalid) {
    EXPECT_THROW(apply({{0, moved(5)}, {1, model}}), std::invalid_argument);
    EXPECT_EQ(objects[0].model, glm::mat4{1});
    EXPECT_EQ(state.changedObjectCount(), 0);
  }
}

TEST_F(BimObjectTransformTests, DuplicateIndicesAreRejectedBeforeMutation) {
  EXPECT_THROW(apply({{0, moved(1)}, {0, moved(2)}}), std::invalid_argument);
  EXPECT_EQ(objects[0].model, glm::mat4{1});
  EXPECT_EQ(state.changedObjectCount(), 0);
}

TEST_F(BimObjectTransformTests, UnchangedAndEmptyBatchesAreNoops) {
  EXPECT_FALSE(apply({}));
  EXPECT_FALSE(apply({{0, objects[0].model}, {1, objects[1].model}}));
  EXPECT_EQ(state.changedObjectCount(), 0);
  ASSERT_TRUE(apply({{0, moved(2)}}));
  const auto original = objects[0];
  EXPECT_FALSE(apply({{0, moved(2)}}));
  EXPECT_EQ(objects[0].boundingSphere, original.boundingSphere);
  EXPECT_EQ(state.changedObjectCount(), 1);
}

TEST_F(BimObjectTransformTests, RepeatedRotationsAndResetDoNotInflateBounds) {
  metadata[0].bounds = box({-3, -1, -.5f}, {3, 1, .5f});
  for (unsigned turn = 1; turn <= 100; ++turn) {
    const auto rotation = glm::rotate(glm::mat4{1}, glm::radians(float(turn) * 5.0f), glm::vec3{0, 0, 1});
    ASSERT_TRUE(apply({{0, rotation}}));
  }
  ASSERT_TRUE(apply({{0, glm::mat4{1}}}));
  expectNear(bounds(0).min, metadata[0].bounds.min);
  expectNear(bounds(0).max, metadata[0].bounds.max);
}

TEST_F(BimObjectTransformTests, RootTranslationBeforeAndAfterAttachmentIsAppliedOnce) {
  for (auto &object : objects) {
    object.model = moved(10);
    object.boundingSphere.x = 10;
  }
  expectNear(bounds(0, {10, 0, 0}).min, {9, -1, -1});
  ASSERT_TRUE(apply({{0, moved(12)}}, {10, 0, 0}));
  expectNear(bounds(0).min, {11, -1, -1});
  state.translate({3, 2, 0});
  for (auto &object : objects) {
    object.model[3] += glm::vec4{3, 2, 0, 0};
    object.boundingSphere += glm::vec4{3, 2, 0, 0};
  }
  expectNear(bounds(0, {13, 2, 0}).min, {14, 1, -1});
  expectNear(bounds(1, {13, 2, 0}).min, {12, 1, -1});
  ASSERT_TRUE(apply({{0, moved(14, 4, 0)}}, {13, 2, 0}));
  expectNear(bounds(0).min, {13, 3, -1});
}

TEST_F(BimObjectTransformTests, ProviderBoundsContainStaticAndMovedObjects) {
  ASSERT_TRUE(apply({{0, moved(10, -5)}}));
  container::scene::SceneProviderBounds sceneBounds{.min = {-1, -1, -1}, .max = {1, 1, 1}, .valid = true};
  state.extendSceneBounds(sceneBounds);
  expectNear(sceneBounds.min, {-1, -6, -1});
  expectNear(sceneBounds.max, {11, 1, 1});
  state.clear();
  EXPECT_EQ(state.changedObjectCount(), 0);
  expectNear(bounds(0).min, {-1, -1, -1});
}

TEST_F(BimObjectTransformTests, MissingImportBoundsUseOwnedOriginalSphere) {
  metadata[0].bounds = {};
  objects[0].boundingSphere = {0, 0, 0, 2};
  ASSERT_TRUE(apply({{0, moved(3)}}));
  expectNear(bounds(0).min, {1, -2, -2});
  expectNear(bounds(0).max, {5, 2, 2});
  ASSERT_TRUE(apply({{0, moved(4)}}));
  expectNear(bounds(0).min, {2, -2, -2});
}

TEST_F(BimObjectTransformTests, BoundsOverflowIsRejectedAtomically) {
  metadata[1].bounds = box(glm::vec3{-2}, glm::vec3{2});
  const auto enormous = glm::scale(glm::mat4{1}, glm::vec3{std::numeric_limits<float>::max()});
  EXPECT_THROW(apply({{0, moved(1)}, {1, enormous}}), std::invalid_argument);
  EXPECT_EQ(objects[0].model, glm::mat4{1});
  EXPECT_EQ(state.changedObjectCount(), 0);
}

TEST_F(BimObjectTransformTests, ConstValidationPreparesExistingAndNewRowsWithoutPublishing) {
  ASSERT_TRUE(apply({{0, moved(2)}}));
  const auto originalObjects = objects;
  const auto originalBounds = bounds(0);
  const std::array updates{ObjectTransformUpdate{0, moved(4, 2)},
      ObjectTransformUpdate{1, moved(-3) * glm::scale(glm::mat4{1}, glm::vec3{2, .5f, 1})}};
  const BimObjectTransforms &readOnly = state;
  EXPECT_NO_THROW(readOnly.validate(objects, metadata, updates));
  EXPECT_EQ(state.changedObjectCount(), 1);
  EXPECT_EQ(objects[0].model, originalObjects[0].model);
  EXPECT_EQ(objects[1].model, originalObjects[1].model);
  EXPECT_EQ(objects[0].boundingSphere, originalObjects[0].boundingSphere);
  EXPECT_EQ(bounds(0).min, originalBounds.min);
  EXPECT_EQ(bounds(0).max, originalBounds.max);
  EXPECT_EQ(bounds(1).min, metadata[1].bounds.min);
  ASSERT_TRUE(state.apply(objects, metadata, updates));
  EXPECT_EQ(state.changedObjectCount(), 2);
  EXPECT_EQ(objects[0].model, updates[0].model);
  EXPECT_EQ(objects[1].model, updates[1].model);
  expectNear(bounds(0).min, {3, 1, -1});
  expectNear(bounds(1).min, {-5, -.5f, -1});
  expectNormal(objects[1]);
}

TEST_F(BimObjectTransformTests, ConstValidationAndApplyRejectTheSameInvalidBatchesWithoutMutation) {
  ASSERT_TRUE(apply({{0, moved(2)}}));
  metadata[1].bounds = box(glm::vec3{-2}, glm::vec3{2});
  const auto rotation = glm::mat4_cast(glm::quat{glm::radians(glm::vec3{35, 45, 20})});
  const std::array invalidModels{
      glm::scale(glm::mat4{1}, glm::vec3{1000, .001f, .001f}) * rotation,
      glm::scale(glm::mat4{1}, glm::vec3{-1, 1, 1}),
      glm::scale(glm::mat4{1}, glm::vec3{std::numeric_limits<float>::max()}),
      glm::scale(glm::mat4{1}, glm::vec3{1e-40f}),
      glm::scale(glm::mat4{1}, glm::vec3{1, 0, 1})};
  const auto originalObjects = objects;
  const auto originalBounds = bounds(0);
  const BimObjectTransforms &readOnly = state;
  for (size_t index = 0; index < invalidModels.size(); ++index) {
    SCOPED_TRACE(index);
    const std::array updates{ObjectTransformUpdate{0, moved(8)},
                            ObjectTransformUpdate{1, invalidModels[index]}};
    EXPECT_THROW(readOnly.validate(objects, metadata, updates), std::invalid_argument);
    EXPECT_THROW(state.apply(objects, metadata, updates), std::invalid_argument);
    EXPECT_EQ(state.changedObjectCount(), 1);
    EXPECT_EQ(objects[0].model, originalObjects[0].model);
    EXPECT_EQ(objects[1].model, originalObjects[1].model);
    EXPECT_EQ(objects[0].boundingSphere, originalObjects[0].boundingSphere);
    EXPECT_EQ(bounds(0).min, originalBounds.min);
    EXPECT_EQ(bounds(0).max, originalBounds.max);
    EXPECT_EQ(bounds(1).min, metadata[1].bounds.min);
  }
}

} // namespace
