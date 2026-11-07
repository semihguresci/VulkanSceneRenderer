#include "Container/renderer/scene/CameraController.h"

#include "Container/ecs/World.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/InputManager.h"
#include "Container/utility/SceneGraph.h"
#include "Container/utility/SwapChainManager.h"
#include "Container/utility/TransformScale.h"
#if CONTAINER_HAS_JOLT
#include "Container/physics/PhysicsElementBindings.h"
#include "Container/renderer/bim/BimObjectTransforms.h"
#endif

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace {

glm::mat4 transform(glm::vec3 position, glm::vec3 angles, glm::vec3 scale) {
  return glm::translate(glm::mat4{1.0f}, position) *
      glm::mat4_cast(glm::quat{glm::radians(angles)}) *
      glm::scale(glm::mat4{1.0f}, scale);
}

void expectTransform(const glm::mat4 &actual, const glm::mat4 &expected) {
  for (int column = 0; column < 4; ++column) {
    const double magnitude = column == 3 ? 1.0 : glm::length(glm::dvec3{expected[column]});
    for (int row = 0; row < 4; ++row)
      EXPECT_NEAR(actual[column][row], expected[column][row], magnitude * 2e-5);
  }
}

class CameraControllerTransformTests : public testing::Test {
protected:
  // These transform APIs only use the graph. Default allocation state and a
  // null-handle swapchain require no Vulkan instance, window or device.
  container::gpu::AllocationManager allocation;
  container::gpu::SwapChainManager swapchain{nullptr, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
  container::scene::SceneGraph graph;
  container::ecs::World world;
  container::window::InputManager input;
  container::renderer::CameraController controller{nullptr, allocation, swapchain,
      graph, nullptr, nullptr, world, input};
};

TEST_F(CameraControllerTransformTests, RetiredStableSlotsCannotBeSelectedOrTransformed) {
  const auto node = graph.createNode(glm::mat4(1), 0, false,
                                      container::scene::SceneGraph::kInvalidNode, "Gone");
  auto prepared = graph.prepareRetireSubtree(node);
  ASSERT_TRUE(graph.publishRetirement(prepared));
  uint32_t selected = node;
  controller.selectMeshNode(node, selected);
  EXPECT_EQ(selected, container::scene::SceneGraph::kInvalidNode);
  auto controls = controller.nodeTransformControls(node);
  controls.position = {9, 8, 7};
  EXPECT_FALSE(controller.previewNodeTransform(node, controls));
  EXPECT_FALSE(controller.applyNodeTransform(node, node, controls));
  EXPECT_EQ(graph.getNode(node)->localTransform, glm::mat4(1));
}

TEST_F(CameraControllerTransformTests, PreviewIsReadOnlyAndMatchesAppliedTransform) {
  const auto authored = transform({3, 4, 5}, {0, 0, 0}, {-2, 1, 3});
  const auto parent = graph.createNode(transform({1, 2, 3}, {0, 20, 0}, {1, 1, 1}), 0);
  const auto node = graph.createNode(authored, 0);
  ASSERT_TRUE(graph.setParent(node, parent));
  graph.updateWorldTransforms();
  const auto worldBefore = graph.getNode(node)->worldTransform;
  const auto revision = graph.revision();
  auto controls = controller.nodeTransformControls(node);
  controls.position = {6, 7, 8};
  controls.rotationDegrees = {0, 0, 90};
  controls.scale = {-4, 2, 1};
  const auto proposed = controller.previewNodeTransform(node, controls);
  ASSERT_TRUE(proposed);
  EXPECT_EQ(graph.revision(), revision);
  EXPECT_EQ(graph.getNode(node)->localTransform, authored);
  EXPECT_EQ(graph.getNode(node)->worldTransform, worldBefore);
  expectTransform(*proposed, transform({6, 7, 8}, {0, 0, 90}, {-4, 2, 1}));
  controller.applyNodeTransform(node, parent, controls);
  EXPECT_EQ(graph.getNode(node)->localTransform, *proposed);
  expectTransform(graph.getNode(node)->worldTransform,
                  graph.getNode(parent)->worldTransform * *proposed);
  EXPECT_FALSE(controller.previewNodeTransform(container::scene::SceneGraph::kInvalidNode, controls));
}

TEST_F(CameraControllerTransformTests, PreviewPreservesCompensatedBasisForNoOpAndTranslation) {
  for (const float scaleX : {2.f, -2.f}) {
    SCOPED_TRACE(scaleX);
    const auto parentWorld = transform({1, 2, 3}, {0, 0, 45}, {scaleX, 1, 1});
    const auto desiredWorld = transform({4, 5, 6}, {0, 0, 0}, {1, 1, 1});
    const auto local = glm::inverse(parentWorld) * desiredWorld;
    const auto parent = graph.createNode(parentWorld, 0);
    const auto node = graph.createNode(local, 0);
    ASSERT_TRUE(graph.setParent(node, parent));
    graph.updateWorldTransforms();
    const auto revision = graph.revision();
    auto controls = controller.nodeTransformControls(node);
    const auto unchanged = controller.previewNodeTransform(node, controls);
    ASSERT_TRUE(unchanged);
    EXPECT_EQ(*unchanged, local);
    controls.position += glm::vec3{1, -.5f, 2};
    const auto moved = controller.previewNodeTransform(node, controls);
    ASSERT_TRUE(moved);
    for (int column = 0; column < 3; ++column)
      EXPECT_EQ((*moved)[column], local[column]);
    EXPECT_EQ((*moved)[3], glm::vec4(controls.position, local[3].w));
    EXPECT_EQ(graph.getNode(node)->localTransform, local);
    EXPECT_EQ(graph.revision(), revision);
    controller.applyNodeTransform(node, parent, controls);
    EXPECT_EQ(graph.getNode(node)->localTransform, *moved);
  }
}

TEST_F(CameraControllerTransformTests, ApplyCallbackRunsOnlyForChangedTargetsBeforeGraphWrites) {
  const auto authored = transform({3, 4, 5}, {0, 0, 0}, {-2, 1, 3});
  const auto node = graph.createNode(authored, 0);
  graph.updateWorldTransforms();
  const auto revision = graph.revision();
  const auto worldBefore = graph.getNode(node)->worldTransform;
  auto controls = controller.nodeTransformControls(node);
  uint32_t calls = 0;
  const auto rejecting = [&](const glm::mat4 &) {
    ++calls;
    EXPECT_EQ(graph.revision(), revision);
    EXPECT_EQ(graph.getNode(node)->localTransform, authored);
    throw std::invalid_argument("candidate rejected");
  };
  EXPECT_FALSE(controller.applyNodeTransform(container::scene::SceneGraph::kInvalidNode,
                                             node, controls, rejecting));
  EXPECT_FALSE(controller.applyNodeTransform(node, node, controls, rejecting));
  EXPECT_EQ(calls, 0u);
  controls.position.x += 1;
  EXPECT_THROW(controller.applyNodeTransform(node, node, controls, rejecting), std::invalid_argument);
  EXPECT_EQ(calls, 1u);
  EXPECT_EQ(graph.revision(), revision);
  EXPECT_EQ(graph.getNode(node)->localTransform, authored);
  EXPECT_EQ(graph.getNode(node)->worldTransform, worldBefore);
  const auto proposed = controller.previewNodeTransform(node, controls);
  ASSERT_TRUE(proposed);
  EXPECT_TRUE(controller.applyNodeTransform(node, node, controls, [&](const glm::mat4 &candidate) {
    ++calls;
    EXPECT_EQ(candidate, *proposed);
    EXPECT_EQ(graph.revision(), revision);
    EXPECT_EQ(graph.getNode(node)->localTransform, authored);
  }));
  EXPECT_EQ(calls, 2u);
  EXPECT_EQ(graph.getNode(node)->localTransform, *proposed);
}

TEST_F(CameraControllerTransformTests, InspectorRoundTripPreservesReflectionsAndImportedScaleRanges) {
  const std::array scales{glm::vec3{-1.5f, 2.0f, .6f}, glm::vec3{1.5f, -2.0f, .6f},
      glm::vec3{1.5f, 2.0f, -.6f}, glm::vec3{-1.5f, -2.0f, .6f},
      glm::vec3{-1e-12f, 2e-10f, 3e-8f}, glm::vec3{-2000.0f, 10000.0f, .5f}};
  for (const auto scale : scales) {
    SCOPED_TRACE(testing::Message() << "scale=" << scale.x << ',' << scale.y << ',' << scale.z);
    const auto authored = transform({3.0f, 4.0f, 5.0f}, {25.0f, -35.0f, 20.0f}, scale);
    const auto node = graph.createNode(authored, 0u);
    const auto controls = controller.nodeTransformControls(node);
    controller.applyNodeTransform(node, node, controls);
    expectTransform(graph.getNode(node)->localTransform, authored);
    EXPECT_EQ(glm::determinant(glm::dmat3{graph.getNode(node)->localTransform}) < 0,
              glm::determinant(glm::dmat3{authored}) < 0);
  }
}

TEST_F(CameraControllerTransformTests, MovingMirroredParentPreservesChildGeometry) {
  const auto authored = transform({3.0f, 4.0f, 5.0f}, {25.0f, -35.0f, 20.0f}, {-1.5f, 2.0f, .6f});
  const auto parent = graph.createNode(authored, 0u);
  const auto childLocal = transform({2.0f, 0.0f, -1.0f}, {0.0f, 20.0f, 0.0f}, {1.0f, 2.0f, 1.0f});
  const auto child = graph.createNode(childLocal, 0u);
  ASSERT_TRUE(graph.setParent(child, parent));
  graph.updateWorldTransforms();
  const auto previousChildWorld = graph.getNode(child)->worldTransform;
  auto controls = controller.nodeTransformControls(parent);
  const glm::vec3 delta{4.0f, -3.0f, 2.0f};
  controls.position += delta;
  controller.applyNodeTransform(parent, parent, controls);
  expectTransform(graph.getNode(parent)->localTransform,
                  glm::translate(glm::mat4{1.0f}, delta) * authored);
  expectTransform(graph.getNode(child)->worldTransform,
                  glm::translate(glm::mat4{1.0f}, delta) * previousChildWorld);
}

TEST_F(CameraControllerTransformTests, InspectorNoOpPreservesCompensatedLocalBasis) {
  for (const float scaleX : {2.f, -2.f}) {
    SCOPED_TRACE(scaleX);
    const auto parentWorld = transform({1, 2, 3}, {0, 0, 45}, {scaleX, 1, 1});
    const auto desiredWorld = transform({4, 5, 6}, {0, 0, 0}, {1, 1, 1});
    const auto local = glm::inverse(parentWorld) * desiredWorld;
    ASSERT_GT(std::abs(glm::dot(glm::normalize(glm::vec3{local[0]}),
                               glm::normalize(glm::vec3{local[1]}))), .1f);
    const auto parent = graph.createNode(parentWorld, 0);
    const auto node = graph.createNode(local, 0);
    ASSERT_TRUE(graph.setParent(node, parent));
    graph.updateWorldTransforms();
    controller.applyNodeTransform(node, parent, controller.nodeTransformControls(node));
    expectTransform(graph.getNode(node)->localTransform, local);
    expectTransform(graph.getNode(node)->worldTransform, desiredWorld);
  }
}

TEST_F(CameraControllerTransformTests, MovingCompensatedLocalPreservesWorldGeometryBasis) {
  const auto parentWorld = transform({1, 2, 3}, {0, 0, 45}, {2, 1, 1});
  const auto desiredWorld = transform({4, 5, 6}, {0, 0, 0}, {1, 1, 1});
  const auto local = glm::inverse(parentWorld) * desiredWorld;
  const auto parent = graph.createNode(parentWorld, 0);
  const auto node = graph.createNode(local, 0);
  ASSERT_TRUE(graph.setParent(node, parent));
  graph.updateWorldTransforms();
  auto controls = controller.nodeTransformControls(node);
  const glm::vec3 delta{1, -.5f, 2};
  controls.position += delta;
  controller.applyNodeTransform(node, parent, controls);
  auto expectedLocal = local;
  expectedLocal[3] += glm::vec4{delta, 0};
  expectTransform(graph.getNode(node)->localTransform, expectedLocal);
  expectTransform(graph.getNode(node)->worldTransform, parentWorld * expectedLocal);
  for (int column = 0; column < 3; ++column)
    EXPECT_EQ(graph.getNode(node)->localTransform[column], local[column]);
}

#if CONTAINER_HAS_JOLT
TEST_F(CameraControllerTransformTests, UnchangedAndRejectedEditsRetainDynamicPlaybackAndSubframeTime) {
  using namespace container::physics;
  for (const bool editAncestor : {false, true}) {
    SCOPED_TRACE(editAncestor);
    const auto parent = graph.createNode(transform({0, 0, 0}, {0, 0, 0},
        editAncestor ? glm::vec3{1} : glm::vec3{2, 1, 1}), 0);
    const auto node = graph.createNode(transform({0, 4, 0},
        editAncestor ? glm::vec3{0, 0, 45} : glm::vec3{0}, {1, 1, 1}), 0);
    ASSERT_TRUE(graph.setParent(node, parent));
    graph.updateWorldTransforms();
    PhysicsScene physics{{.gravity = {0, 0, 0}, .maxBodies = 4, .workerThreads = 1}};
    const auto body = physics.attachBox(graph, node, glm::vec3{-.5f}, glm::vec3{.5f},
                                        {.linearVelocity = {1, 0, 0}});
    physics.setPlaying(true);
    physics.update(graph, PhysicsScene::kFixedStepSeconds / 2);
    const auto edited = editAncestor ? parent : node;
    const auto revision = graph.revision();
    const auto localBefore = graph.getNode(edited)->localTransform;
    const auto worldBefore = graph.getNode(node)->worldTransform;
    const auto poseBefore = *physics.bodyPose(node);
    const auto accumulated = physics.statistics().accumulatedSeconds;
    uint32_t calls = 0;
    const auto beforeApply = [&](const glm::mat4 &candidate) {
      ++calls;
      physics.validateTransform(graph, edited, candidate);
      if (physics.hasDynamicBodyInSubtree(graph, edited))
        physics.setPlaying(false);
    };
    auto controls = controller.nodeTransformControls(edited);
    // A small snapped gizmo movement resolves to these unchanged controls.
    EXPECT_FALSE(controller.applyNodeTransform(edited, parent, controls, beforeApply));
    EXPECT_EQ(calls, 0u);
    if (editAncestor) controls.scale.x = 2;
    else controls.rotationDegrees.z = 45;
    EXPECT_THROW(controller.applyNodeTransform(edited, parent, controls, beforeApply),
                 std::invalid_argument);
    EXPECT_EQ(calls, 1u);
    EXPECT_EQ(graph.revision(), revision);
    EXPECT_EQ(graph.getNode(edited)->localTransform, localBefore);
    EXPECT_EQ(graph.getNode(node)->worldTransform, worldBefore);
    EXPECT_TRUE(physics.playing());
    EXPECT_EQ(physics.statistics().accumulatedSeconds, accumulated);
    EXPECT_EQ(physics.bodyId(node), body);
    EXPECT_EQ(physics.bodyPose(node)->position, poseBefore.position);
    EXPECT_EQ(physics.bodyPose(node)->rotation, poseBefore.rotation);
    physics.update(graph, PhysicsScene::kFixedStepSeconds / 2);
    EXPECT_EQ(physics.statistics().totalSteps, 1u);
    EXPECT_GT(physics.bodyPose(node)->position.x, poseBefore.position.x);
    controls = controller.nodeTransformControls(edited);
    controls.position.x += 1;
    const auto proposed = controller.previewNodeTransform(edited, controls);
    ASSERT_TRUE(proposed);
    EXPECT_TRUE(controller.applyNodeTransform(edited, parent, controls, beforeApply));
    EXPECT_EQ(calls, 2u);
    EXPECT_FALSE(physics.playing());
    EXPECT_EQ(physics.statistics().accumulatedSeconds, 0);
    EXPECT_EQ(graph.getNode(edited)->localTransform, *proposed);
    EXPECT_NO_THROW(physics.update(graph, 0));
    EXPECT_EQ(physics.bodyId(node), body);
    const auto expected = glm::dvec3{graph.getNode(node)->worldTransform[3]};
    EXPECT_NEAR(glm::length(physics.bodyPose(node)->position - expected), 0, 1e-5);
  }
}

TEST_F(CameraControllerTransformTests, PhysicsPreflightRejectsPreviewBeforeGraphOrPlaybackChanges) {
  using namespace container::physics;
  for (const auto motion : {PhysicsMotionType::Static, PhysicsMotionType::Dynamic}) {
    for (const bool editAncestor : {false, true}) {
      SCOPED_TRACE(testing::Message() << "motion=" << int(motion)
                                     << " ancestor=" << editAncestor);
      PhysicsScene physics{{.gravity = {0, 0, 0}, .maxBodies = 16, .workerThreads = 1}};
      const auto parent = graph.createNode(transform({0, 0, 0}, {0, 0, 0},
                                                     editAncestor ? glm::vec3{1} : glm::vec3{2, 1, 1}), 0);
      const auto node = graph.createNode(transform({0, 4, 0},
          editAncestor ? glm::vec3{0, 0, 45} : glm::vec3{0}, {1, 1, 1}), 0);
      ASSERT_TRUE(graph.setParent(node, parent));
      graph.updateWorldTransforms();
      const auto body = physics.attachBox(graph, node, glm::vec3{-.5f}, glm::vec3{.5f},
                                          {.motionType = motion});
      physics.setPlaying(true);
      const auto edited = editAncestor ? parent : node;
      const auto localBefore = graph.getNode(edited)->localTransform;
      const auto worldBefore = graph.getNode(node)->worldTransform;
      const auto poseBefore = *physics.bodyPose(node);
      const auto revision = graph.revision();
      auto controls = controller.nodeTransformControls(edited);
      if (editAncestor) controls.scale.x = 2;
      else controls.rotationDegrees.z = 45;
      const auto proposed = controller.previewNodeTransform(edited, controls);
      ASSERT_TRUE(proposed);
      EXPECT_THROW(physics.validateTransform(graph, edited, *proposed), std::invalid_argument);
      EXPECT_EQ(graph.revision(), revision);
      EXPECT_EQ(graph.getNode(edited)->localTransform, localBefore);
      EXPECT_EQ(graph.getNode(node)->worldTransform, worldBefore);
      EXPECT_EQ(physics.bodyId(node), body);
      EXPECT_EQ(physics.bodyPose(node)->position, poseBefore.position);
      EXPECT_EQ(physics.bodyPose(node)->rotation, poseBefore.rotation);
      EXPECT_TRUE(physics.playing());
      EXPECT_EQ(physics.statistics().totalSteps, 0);

      controls = controller.nodeTransformControls(edited);
      const auto unchanged = controller.previewNodeTransform(edited, controls);
      ASSERT_TRUE(unchanged);
      EXPECT_NO_THROW(physics.validateTransform(graph, edited, *unchanged));
      EXPECT_EQ(graph.revision(), revision);
      EXPECT_TRUE(physics.playing());
      controls.position += glm::vec3{1, 0, 0};
      const auto moved = controller.previewNodeTransform(edited, controls);
      ASSERT_TRUE(moved);
      ASSERT_NO_THROW(physics.validateTransform(graph, edited, *moved));
      physics.setPlaying(false);
      controller.applyNodeTransform(edited, parent, controls);
      ASSERT_NO_THROW(physics.update(graph, 0));
      expectTransform(graph.getNode(edited)->localTransform, *moved);
      EXPECT_EQ(physics.bodyId(node), body);
      EXPECT_EQ(physics.statistics().totalSteps, 0);
    }
  }
}

TEST_F(CameraControllerTransformTests, FragmentPublicationPreflightRejectsBeforePhysicsOrEditorMutation) {
  using namespace container::physics;
  using namespace container::renderer;
  for (const bool editRoot : {false, true}) {
    SCOPED_TRACE(editRoot);
    PhysicsScene physics{{.gravity = {0, 0, 0}, .maxBodies = 4, .workerThreads = 1}};
    PhysicsElementBindings bindings;
    const auto rotation = glm::mat4_cast(glm::quat{glm::radians(glm::vec3{35, 45, 20})});
    const auto basis = glm::mat3{rotation};
    const glm::vec3 half{
        .5f * (std::abs(basis[0].x) + std::abs(basis[1].x) + std::abs(basis[2].x)),
        .5f * (std::abs(basis[0].y) + std::abs(basis[1].y) + std::abs(basis[2].y)),
        .5f * (std::abs(basis[0].z) + std::abs(basis[1].z) + std::abs(basis[2].z))};
    std::array<PhysicsElementFragment, 2> fragments;
    std::array<container::gpu::ObjectData, 3> objects;
    std::array<BimElementMetadata, 3> metadata;
    for (uint32_t index = 0; index < fragments.size(); ++index) {
      const glm::vec3 center{index == 0 ? -1.f : 1.f, 0, 0};
      const auto model = glm::translate(glm::mat4{1}, center) * rotation;
      fragments[index] = {.objectIndex = index, .model = model,
          .worldMin = center - half, .worldMax = center + half};
      objects[index].model = model;
      objects[index].boundingSphere = {center, glm::length(half)};
      metadata[index].objectIndex = index;
      metadata[index].bounds = {.valid = true, .min = center - half, .max = center + half,
          .center = center, .size = half * 2.f, .radius = glm::length(half),
          .floorElevation = -half.y};
    }
    objects[2].model = glm::translate(glm::mat4{1}, glm::vec3{20, 0, 0});
    const auto body = bindings.attachOrReplace(graph, physics, 1, 1, fragments);
    const auto node = *bindings.bodyNodeForObject(0);
    const auto edited = editRoot ? graph.getNode(node)->parent : node;
    physics.setPlaying(true);
    physics.update(graph, PhysicsScene::kFixedStepSeconds / 2);
    const auto local = graph.getNode(edited)->localTransform;
    const auto bodyWorld = graph.getNode(node)->worldTransform;
    const auto pose = *physics.bodyPose(node);
    const auto revision = graph.revision();
    const auto accumulated = physics.statistics().accumulatedSeconds;
    const auto originals = objects;
    BimObjectTransforms publication;
    const auto beforeApply = [&](const glm::mat4 &candidate) {
      physics.validateTransform(graph, edited, candidate);
      const auto updates = bindings.previewTransforms(graph, edited, candidate);
      publication.validate(objects, metadata, updates);
      if (physics.hasDynamicBodyInSubtree(graph, edited)) physics.setPlaying(false);
    };
    auto controls = controller.nodeTransformControls(edited);
    controls.scale = {1000, .001f, .001f};
    const auto proposed = controller.previewNodeTransform(edited, controls);
    ASSERT_TRUE(proposed);
    // The collider itself is valid; the composed imported fragment is too
    // ill-conditioned for the renderer's existing publication contract.
    ASSERT_NO_THROW(physics.validateTransform(graph, edited, *proposed));
    EXPECT_THROW(controller.applyNodeTransform(edited, edited, controls, beforeApply),
                 std::invalid_argument);
    EXPECT_EQ(graph.revision(), revision);
    EXPECT_EQ(graph.getNode(edited)->localTransform, local);
    EXPECT_EQ(graph.getNode(node)->worldTransform, bodyWorld);
    EXPECT_EQ(physics.bodyId(node), body);
    EXPECT_EQ(physics.bodyPose(node)->position, pose.position);
    EXPECT_EQ(physics.bodyPose(node)->rotation, pose.rotation);
    EXPECT_TRUE(physics.playing());
    EXPECT_EQ(physics.statistics().accumulatedSeconds, accumulated);
    EXPECT_EQ(publication.changedObjectCount(), 0);
    for (size_t index = 0; index < objects.size(); ++index) {
      EXPECT_EQ(objects[index].model, originals[index].model);
      EXPECT_EQ(objects[index].boundingSphere, originals[index].boundingSphere);
    }
    // Retaining the half tick matters: the rejected edit cannot silently
    // consume timing even though the original handle and matrices survived.
    physics.update(graph, PhysicsScene::kFixedStepSeconds / 2);
    EXPECT_EQ(physics.statistics().totalSteps, 1);
    controls.scale = {2, .5f, 1};
    ASSERT_TRUE(controller.applyNodeTransform(edited, edited, controls, beforeApply));
    EXPECT_FALSE(physics.playing());
    ASSERT_NO_THROW(physics.update(graph, 0));
    EXPECT_NE(physics.bodyId(node), body);
    const auto updates = bindings.collectTransforms(graph);
    ASSERT_EQ(updates.size(), 2);
    ASSERT_TRUE(publication.apply(objects, metadata, updates));
    EXPECT_EQ(publication.changedObjectCount(), 2);
    const glm::vec3 scale{2, .5f, 1};
    for (uint32_t index = 0; index < fragments.size(); ++index) {
      expectTransform(objects[index].model,
                      glm::scale(glm::mat4{1}, scale) * fragments[index].model);
      const auto bounds = publication.boundsForObject(index, metadata[index].bounds);
      for (int axis = 0; axis < 3; ++axis) {
        EXPECT_NEAR(bounds.min[axis], fragments[index].worldMin[axis] * scale[axis], 1e-5f);
        EXPECT_NEAR(bounds.max[axis], fragments[index].worldMax[axis] * scale[axis], 1e-5f);
      }
    }
    EXPECT_EQ(objects[2].model, originals[2].model);
  }
}

TEST_F(CameraControllerTransformTests, MovingNewBimProxyUnderEditedRootRemainsPublishable) {
  using namespace container::physics;
  PhysicsScene physics{{.gravity = {0, 0, 0}, .maxBodies = 16, .workerThreads = 1}};
  PhysicsElementBindings bindings;
  const auto fragment = [](uint32_t object, glm::vec3 center) {
    return PhysicsElementFragment{.objectIndex = object,
        .model = glm::translate(glm::mat4{1}, center),
        .worldMin = center - glm::vec3{.5f}, .worldMax = center + glm::vec3{.5f}};
  };
  const PhysicsBodySettings settings{.motionType = PhysicsMotionType::Static};
  const std::array first{fragment(0, {0, 5, 0})};
  (void)bindings.attachOrReplace(graph, physics, 1, 1, first, settings);
  const auto root = graph.getNode(*bindings.bodyNodeForObject(0))->parent;
  const auto parentWorld = transform({0, 0, 0}, {0, 0, 45}, {2, 1, 1});
  graph.setLocalTransform(root, parentWorld);
  ASSERT_NO_THROW(physics.update(graph, 0));
  const std::array second{fragment(1, {5, 5, 0})};
  (void)bindings.attachOrReplace(graph, physics, 1, 2, second, settings);
  const auto proxy = *bindings.bodyNodeForObject(1);
  ASSERT_NO_THROW(physics.update(graph, 0));
  const auto localBefore = graph.getNode(proxy)->localTransform;
  auto controls = controller.nodeTransformControls(proxy);
  const glm::vec3 delta{1, 0, 0};
  controls.position += delta;
  controller.applyNodeTransform(proxy, root, controls);
  auto expectedLocal = localBefore;
  expectedLocal[3] += glm::vec4{delta, 0};
  expectTransform(graph.getNode(proxy)->worldTransform, parentWorld * expectedLocal);
  ASSERT_NO_THROW(physics.update(graph, 0));
  EXPECT_EQ(physics.bodyCount(), 2);
  const auto updates = bindings.collectTransforms(graph);
  const auto found = std::find_if(updates.begin(), updates.end(),
      [](const auto &update) { return update.objectIndex == 1; });
  ASSERT_NE(found, updates.end());
  expectTransform(found->model, parentWorld * expectedLocal);
}
#endif

TEST_F(CameraControllerTransformTests, RotatingMirroredNodePreservesTinyAndLargeMagnitudes) {
  for (const auto scale : {glm::vec3{-1.5f, 2.0f, .6f},
                           glm::vec3{-1e-12f, 2e-10f, 3e-8f},
                           glm::vec3{-2000.0f, 10000.0f, .5f}}) {
    const auto node = graph.createNode(transform({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, scale), 0u);
    auto controls = controller.nodeTransformControls(node);
    controls.rotationDegrees.z = 90.0f;
    controller.applyNodeTransform(node, node, controls);
    glm::mat4 expected{1.0f};
    expected[0] = {0.0f, scale.x, 0.0f, 0.0f};
    expected[1] = {-scale.y, 0.0f, 0.0f, 0.0f};
    expected[2] = {0.0f, 0.0f, scale.z, 0.0f};
    expectTransform(graph.getNode(node)->localTransform, expected);
  }
}

TEST_F(CameraControllerTransformTests, ExplicitZeroScaleKeepsSignedNonsingularMinimum) {
  const auto node = graph.createNode(glm::mat4{1.0f}, 0u);
  container::ui::TransformControls controls;
  controls.scale = {-0.0f, 0.0f, 2.0f};
  controller.applyNodeTransform(node, node, controls);
  EXPECT_FLOAT_EQ(graph.getNode(node)->localTransform[0][0], -1e-6f);
  EXPECT_FLOAT_EQ(graph.getNode(node)->localTransform[1][1], 1e-6f);
  EXPECT_FLOAT_EQ(graph.getNode(node)->localTransform[2][2], 2.0f);
}

TEST(TransformScaleTests, UniformAndAxisEditingPreserveSignsAndMagnitudeLimits) {
  EXPECT_EQ(container::ui::clampSignedScale(glm::vec3{-2.0f, 3.0f, -.5f} * .5f),
            glm::vec3(-1.0f, 1.5f, -.25f));
  EXPECT_FLOAT_EQ(container::ui::clampSignedScale(-.0001f), -.001f);
  EXPECT_FLOAT_EQ(container::ui::clampSignedScale(-2000.0f), -1000.0f);
  EXPECT_FLOAT_EQ(container::ui::clampSignedScale(2000.0f), 1000.0f);
}

TEST(TransformScaleTests, SnappingCannotFlipReflectionsOrClampUnchangedImportedAxes) {
  const auto snapped = container::ui::snapSignedScale({-1.5f, .03f, 2000.0f},
                                                     {-1.79f, .03f, 2000.0f}, .1f);
  EXPECT_NEAR(snapped.x, -1.8f, 1e-6f);
  EXPECT_FLOAT_EQ(snapped.y, .03f);
  EXPECT_FLOAT_EQ(snapped.z, 2000.0f);
  const auto shrinking = container::ui::snapSignedScale({-.08f, .08f, 1.0f},
                                                       {-.001f, .001f, 1.0f}, .1f);
  EXPECT_FLOAT_EQ(shrinking.x, -.001f);
  EXPECT_FLOAT_EQ(shrinking.y, .001f);
  EXPECT_FLOAT_EQ(shrinking.z, 1.0f);
}

} // namespace
