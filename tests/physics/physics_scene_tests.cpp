#include "Container/physics/PhysicsScene.h"
#include "Container/utility/SceneGraph.h"

#include <gtest/gtest.h>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {

using container::physics::PhysicsBodySettings;
using container::physics::PhysicsMotionType;
using container::physics::PhysicsScene;
using container::physics::PhysicsWorldSettings;
using container::scene::SceneGraph;

TEST(PhysicsSceneRetirement, ExplicitAttachmentAndTransformCannotTargetRetainedRetiredSlots) {
  SceneGraph graph;
  const auto node = graph.createNode(glm::mat4(1), 0, false, SceneGraph::kInvalidNode, "Retired");
  auto retirement = graph.prepareRetireSubtree(node);
  ASSERT_TRUE(graph.publishRetirement(retirement));
  ASSERT_NE(graph.getNode(node), nullptr);
  PhysicsWorldSettings settings;
  settings.workerThreads = 0;
  PhysicsScene physics{settings};
  EXPECT_THROW(physics.attachBox(graph, node, glm::vec3(-.5f), glm::vec3(.5f)), std::invalid_argument);
  EXPECT_THROW(physics.validateTransform(graph, node, glm::mat4(1)), std::invalid_argument);
  EXPECT_EQ(physics.bodyCount(), 0u);
}

glm::mat4 translation(float x, float y, float z) {
  return glm::translate(glm::mat4{1.0f}, glm::vec3{x, y, z});
}

void expectMatrixNear(const glm::mat4 &actual, const glm::mat4 &expected,
                       float tolerance = 1e-4f) {
  for (int column = 0; column < 4; ++column)
    for (int row = 0; row < 4; ++row)
      EXPECT_NEAR(actual[column][row], expected[column][row], tolerance);
}

void expectPositionNear(glm::dvec3 actual, glm::dvec3 expected,
                         double tolerance = 1e-5) {
  EXPECT_NEAR(actual.x, expected.x, tolerance);
  EXPECT_NEAR(actual.y, expected.y, tolerance);
  EXPECT_NEAR(actual.z, expected.z, tolerance);
}

PhysicsWorldSettings noGravity() {
  PhysicsWorldSettings settings;
  settings.gravity = glm::vec3{0.0f};
  settings.workerThreads = 0u;
  return settings;
}

TEST(PhysicsSceneTests, FallingPauseSingleStepAndResetRestoreAuthoredState) {
  SceneGraph graph;
  const auto node = graph.createNode(translation(0.0f, 5.0f, 0.0f), 0u);
  const auto authored = graph.getNode(node)->localTransform;
  PhysicsScene physics;
  const auto body = physics.attachBox(graph, node, glm::vec3{-0.5f}, glm::vec3{0.5f});
  EXPECT_NE(body, container::physics::kInvalidPhysicsBodyId);
  EXPECT_TRUE(physics.hasBody(node));
  EXPECT_EQ(physics.motionType(node), PhysicsMotionType::Dynamic);
  physics.update(graph, 1.0);
  expectMatrixNear(graph.getNode(node)->localTransform, authored);
  EXPECT_EQ(physics.statistics().totalSteps, 0u);

  physics.singleStep(graph);
  EXPECT_FALSE(physics.playing());
  EXPECT_LT(graph.getNode(node)->worldTransform[3].y, 5.0f);
  EXPECT_EQ(physics.statistics().substeps, 1u);
  const auto afterSingle = graph.getNode(node)->localTransform;
  physics.update(graph, 1.0);
  expectMatrixNear(graph.getNode(node)->localTransform, afterSingle);

  physics.setPlaying(true);
  for (int frame = 0; frame < 30; ++frame)
    physics.update(graph, PhysicsScene::kFixedStepSeconds);
  EXPECT_LT(graph.getNode(node)->worldTransform[3].y, 4.0f);
  physics.reset(graph);
  EXPECT_FALSE(physics.playing());
  EXPECT_EQ(physics.statistics().totalSteps, 0u);
  expectMatrixNear(graph.getNode(node)->localTransform, authored);
  ASSERT_TRUE(physics.bodyPose(node));
  expectPositionNear(physics.bodyPose(node)->position, {0.0, 5.0, 0.0});
}

TEST(PhysicsSceneTests, BoxFallsOntoAuthoredStaticGround) {
  SceneGraph graph;
  const auto ground = graph.createNode(translation(0.0f, -0.5f, 0.0f), 0u);
  const auto box = graph.createNode(translation(0.0f, 3.0f, 0.0f), 0u);
  PhysicsScene physics;
  PhysicsBodySettings staticSettings;
  staticSettings.motionType = PhysicsMotionType::Static;
  (void)physics.attachBox(graph, ground, {-10.0f, -0.5f, -10.0f},
                          {10.0f, 0.5f, 10.0f}, staticSettings);
  (void)physics.attachBox(graph, box, glm::vec3{-0.5f}, glm::vec3{0.5f});
  physics.setPlaying(true);
  for (int frame = 0; frame < 180; ++frame)
    physics.update(graph, PhysicsScene::kFixedStepSeconds);
  EXPECT_NEAR(graph.getNode(box)->worldTransform[3].y, 0.5f, 0.05f);
  EXPECT_FLOAT_EQ(graph.getNode(ground)->worldTransform[3].y, -0.5f);
}

TEST(PhysicsSceneTests, FixedStepResultsDoNotDependOnFramePartitioning) {
  SceneGraph firstGraph, secondGraph;
  const auto firstNode = firstGraph.createNode(translation(0.0f, 20.0f, 0.0f), 0u);
  const auto secondNode = secondGraph.createNode(translation(0.0f, 20.0f, 0.0f), 0u);
  PhysicsScene first, second;
  (void)first.attachBox(firstGraph, firstNode, glm::vec3{-0.5f}, glm::vec3{0.5f});
  (void)second.attachBox(secondGraph, secondNode, glm::vec3{-0.5f}, glm::vec3{0.5f});
  first.setPlaying(true);
  second.setPlaying(true);
  for (int frame = 0; frame < 32; ++frame)
    first.update(firstGraph, 1.0 / 60.0);
  for (int frame = 0; frame < 64; ++frame)
    second.update(secondGraph, 1.0 / 120.0);
  EXPECT_EQ(first.statistics().totalSteps, 32u);
  EXPECT_EQ(second.statistics().totalSteps, 32u);
  expectMatrixNear(firstGraph.getNode(firstNode)->worldTransform,
                   secondGraph.getNode(secondNode)->worldTransform);
}

TEST(PhysicsSceneTests, LongAndInvalidFramesRespectSubstepAndTimeCaps) {
  SceneGraph graph;
  const auto node = graph.createNode(translation(0.0f, 20.0f, 0.0f), 0u);
  PhysicsScene physics;
  (void)physics.attachBox(graph, node, glm::vec3{-0.5f}, glm::vec3{0.5f});
  physics.setPlaying(true);
  physics.update(graph, 1.0);
  EXPECT_EQ(physics.statistics().substeps, PhysicsScene::kMaxSubsteps);
  EXPECT_EQ(physics.statistics().totalSteps, PhysicsScene::kMaxSubsteps);
  EXPECT_NEAR(physics.statistics().droppedSeconds, 1.0 - 8.0 / 60.0, 1e-8);
  EXPECT_NEAR(physics.statistics().accumulatedSeconds, 0.0, 1e-8);
  const auto afterCap = graph.getNode(node)->localTransform;
  physics.update(graph, -1.0);
  physics.update(graph, std::numeric_limits<double>::quiet_NaN());
  physics.update(graph, std::numeric_limits<double>::infinity());
  EXPECT_EQ(physics.statistics().totalSteps, PhysicsScene::kMaxSubsteps);
  expectMatrixNear(graph.getNode(node)->localTransform, afterCap);
  physics.update(graph, PhysicsScene::kFixedStepSeconds);
  EXPECT_EQ(physics.statistics().substeps, 1u);
  EXPECT_EQ(physics.statistics().totalSteps, PhysicsScene::kMaxSubsteps + 1u);
}

TEST(PhysicsSceneTests, BatchedSubstepsMatchIndividualNestedBodyUpdates) {
  SceneGraph batchedGraph, individualGraph;
  PhysicsScene batched{noGravity()}, individual{noGravity()};
  auto populate = [](SceneGraph &graph, PhysicsScene &physics) {
    const auto parent = graph.createNode(translation(0.0f, 20.0f, 0.0f), 0u);
    const auto child = graph.createNode(translation(0.0f, 10.0f, 0.0f), 0u);
    (void)graph.setParent(child, parent);
    PhysicsBodySettings settings;
    settings.linearVelocity = {0.0f, 0.0f, 1.0f};
    settings.angularVelocity = {0.7f, 0.0f, 0.0f};
    (void)physics.attachBox(graph, child, glm::vec3{-0.5f}, glm::vec3{0.5f}, settings);
    settings.linearVelocity = {1.0f, 0.0f, 0.0f};
    settings.angularVelocity = {0.0f, 0.5f, 0.0f};
    (void)physics.attachBox(graph, parent, glm::vec3{-0.5f}, glm::vec3{0.5f}, settings);
    physics.setPlaying(true);
    return std::array{parent, child};
  };
  const auto batchedNodes = populate(batchedGraph, batched);
  const auto individualNodes = populate(individualGraph, individual);
  batched.update(batchedGraph, 4.0 * PhysicsScene::kFixedStepSeconds);
  for (int index = 0; index < 4; ++index)
    individual.update(individualGraph, PhysicsScene::kFixedStepSeconds);
  EXPECT_EQ(batched.statistics().substeps, 4u);
  EXPECT_EQ(batched.statistics().totalSteps, individual.statistics().totalSteps);
  EXPECT_EQ(batched.statistics().transformPublications, 1u);
  EXPECT_EQ(individual.statistics().transformPublications, 4u);
  for (size_t index = 0; index < batchedNodes.size(); ++index)
    expectMatrixNear(batchedGraph.getNode(batchedNodes[index])->worldTransform,
                       individualGraph.getNode(individualNodes[index])->worldTransform);
}

TEST(PhysicsSceneTests, KinematicContactMotionMatchesAuthoredFramePartitioning) {
  SceneGraph batchedGraph, individualGraph;
  PhysicsScene batched{noGravity()}, individual{noGravity()};
  const auto populate = [](SceneGraph &graph, PhysicsScene &physics) {
    const auto pusher = graph.createNode(translation(-2, 0, 0), 0u);
    const auto pushed = graph.createNode(translation(0, 0, 0), 0u);
    (void)physics.attachBox(graph, pusher, glm::vec3{-.5f}, glm::vec3{.5f},
        {.motionType = PhysicsMotionType::Kinematic, .friction = 0});
    (void)physics.attachBox(graph, pushed, glm::vec3{-.5f}, glm::vec3{.5f},
        {.friction = 0});
    physics.setPlaying(true);
    return std::array{pusher, pushed};
  };
  const auto batchedNodes = populate(batchedGraph, batched);
  const auto individualNodes = populate(individualGraph, individual);
  // Both updates describe the same four-tick linear support movement. A slow
  // render frame must not concentrate the entire move into its first tick.
  batchedGraph.setLocalTransform(batchedNodes[0], translation(2, 0, 0));
  batched.update(batchedGraph, 4 * PhysicsScene::kFixedStepSeconds);
  for (int tick = 1; tick <= 4; ++tick) {
    individualGraph.setLocalTransform(individualNodes[0], translation(-2.0f + tick, 0, 0));
    individual.update(individualGraph, PhysicsScene::kFixedStepSeconds);
  }
  expectPositionNear(batched.bodyPose(batchedNodes[0])->position, {2, 0, 0}, 1e-4);
  expectPositionNear(individual.bodyPose(individualNodes[0])->position, {2, 0, 0}, 1e-4);
  EXPECT_GT(individual.bodyPose(individualNodes[1])->position.x, 2.5);
  expectPositionNear(batched.bodyPose(batchedNodes[1])->position,
      individual.bodyPose(individualNodes[1])->position, 1e-3);
}

TEST(PhysicsSceneTests, SingleStepHonorsPendingPausedDynamicTransformEdits) {
  SceneGraph graph;
  const auto node = graph.createNode(translation(0, 5, 0), 0u);
  PhysicsScene physics{noGravity()};
  (void)physics.attachBox(graph, node, glm::vec3{-.5f}, glm::vec3{.5f},
      {.linearVelocity = {2, 0, 0}});
  physics.singleStep(graph);
  const auto rotation = glm::angleAxis(.5f, glm::vec3{0, 1, 0});
  graph.setLocalTransform(node, translation(10, 20, 30) * glm::mat4_cast(rotation));
  // No ordinary update has run between the paused edit and the Step action.
  physics.singleStep(graph);
  ASSERT_TRUE(physics.bodyPose(node));
  const auto pose = *physics.bodyPose(node);
  EXPECT_NEAR(pose.position.x, 10 + 2 * PhysicsScene::kFixedStepSeconds, 1e-4);
  EXPECT_NEAR(pose.position.y, 20, 1e-5);
  EXPECT_NEAR(pose.position.z, 30, 1e-5);
  EXPECT_NEAR(std::abs(glm::dot(pose.rotation, rotation)), 1, 1e-6);
  EXPECT_FALSE(physics.playing());
  EXPECT_EQ(physics.statistics().substeps, 1u);
}

TEST(PhysicsSceneTests, PlayHonorsPendingPausedDynamicTransformEdits) {
  SceneGraph graph;
  const auto node = graph.createNode(translation(0, 5, 0), 0u);
  PhysicsScene physics{noGravity()};
  (void)physics.attachBox(graph, node, glm::vec3{-.5f}, glm::vec3{.5f},
      {.linearVelocity = {2, 0, 0}});
  physics.singleStep(graph);
  const auto rotation = glm::angleAxis(.5f, glm::vec3{0, 1, 0});
  graph.setLocalTransform(node, translation(10, 20, 30) * glm::mat4_cast(rotation));
  // Play may be chosen before any ordinary paused update reads this edit.
  physics.setPlaying(true);
  physics.update(graph, PhysicsScene::kFixedStepSeconds);
  const auto pose = *physics.bodyPose(node);
  expectPositionNear(pose.position, {10 + 2 * PhysicsScene::kFixedStepSeconds, 20, 30}, 1e-4);
  EXPECT_NEAR(std::abs(glm::dot(pose.rotation, rotation)), 1, 1e-6);
  EXPECT_TRUE(physics.playing());
  EXPECT_EQ(physics.statistics().totalSteps, 2u);
}

TEST(PhysicsSceneTests, PendingPlayEditSurvivesSubframeAndAppliesOnlyToFirstTick) {
  SceneGraph graph, referenceGraph;
  const auto node = graph.createNode(glm::mat4{1}, 0u);
  const auto referenceNode = referenceGraph.createNode(translation(10, 4, 0), 0u);
  PhysicsScene physics{noGravity()}, reference{noGravity()};
  (void)physics.attachBox(graph, node, glm::vec3{-.5f}, glm::vec3{.5f},
      {.linearVelocity = {3, 0, 0}});
  (void)reference.attachBox(referenceGraph, referenceNode, glm::vec3{-.5f}, glm::vec3{.5f},
      {.linearVelocity = {3, 0, 0}});
  graph.setLocalTransform(node, translation(10, 4, 0));
  physics.setPlaying(true);
  reference.setPlaying(true);
  physics.update(graph, PhysicsScene::kFixedStepSeconds / 2);
  reference.update(referenceGraph, PhysicsScene::kFixedStepSeconds / 2);
  EXPECT_EQ(physics.statistics().totalSteps, 0u);
  expectPositionNear(physics.bodyPose(node)->position, {0, 0, 0});
  physics.update(graph, PhysicsScene::kFixedStepSeconds * 3.5);
  reference.update(referenceGraph, PhysicsScene::kFixedStepSeconds * 3.5);
  EXPECT_EQ(physics.statistics().totalSteps, 4u);
  expectPositionNear(physics.bodyPose(node)->position,
      reference.bodyPose(referenceNode)->position, 1e-5);
  EXPECT_GT(physics.bodyPose(node)->position.x, 10.1);
  physics.update(graph, PhysicsScene::kFixedStepSeconds);
  reference.update(referenceGraph, PhysicsScene::kFixedStepSeconds);
  expectPositionNear(physics.bodyPose(node)->position,
      reference.bodyPose(referenceNode)->position, 1e-5);
}

TEST(PhysicsSceneTests, PendingPlayDoesNotTeleportAuthoredKinematicContactMotion) {
  SceneGraph graph;
  const auto pusher = graph.createNode(translation(-2, 0, 0), 0u);
  const auto pushed = graph.createNode(glm::mat4{1}, 0u);
  PhysicsScene physics{noGravity()};
  (void)physics.attachBox(graph, pusher, glm::vec3{-.5f}, glm::vec3{.5f},
      {.motionType = PhysicsMotionType::Kinematic, .friction = 0});
  (void)physics.attachBox(graph, pushed, glm::vec3{-.5f}, glm::vec3{.5f}, {.friction = 0});
  graph.setLocalTransform(pusher, translation(2, 0, 0));
  physics.setPlaying(true);
  physics.update(graph, PhysicsScene::kFixedStepSeconds * 4);
  expectPositionNear(physics.bodyPose(pusher)->position, {2, 0, 0}, 1e-4);
  EXPECT_GT(physics.bodyPose(pushed)->position.x, 2.5);
}

TEST(PhysicsSceneTests, PendingPlayEditSurvivesARejectedFirstTick) {
  SceneGraph graph;
  const auto node = graph.createNode(glm::mat4{1}, 0u);
  const auto support = graph.createNode(translation(20, 0, 0), 0u);
  PhysicsScene physics{noGravity()};
  (void)physics.attachBox(graph, node, glm::vec3{-.5f}, glm::vec3{.5f},
      {.linearVelocity = {2, 0, 0}});
  (void)physics.attachBox(graph, support, glm::vec3{-.5f}, glm::vec3{.5f},
      {.motionType = PhysicsMotionType::Static});
  graph.setLocalTransform(node, translation(10, 4, 0));
  physics.setPlaying(true);
  // Bypass the editor's preflight to reject the tick before any body changes.
  ASSERT_TRUE(graph.setParent(node, support));
  EXPECT_THROW(physics.update(graph, PhysicsScene::kFixedStepSeconds), std::invalid_argument);
  EXPECT_EQ(physics.statistics().totalSteps, 0u);
  expectPositionNear(physics.bodyPose(node)->position, {0, 0, 0});
  ASSERT_TRUE(graph.setParent(node, std::nullopt));
  physics.update(graph, PhysicsScene::kFixedStepSeconds);
  EXPECT_EQ(physics.statistics().totalSteps, 2u);
  EXPECT_NEAR(physics.bodyPose(node)->position.x, 10 + 2 * PhysicsScene::kFixedStepSeconds * 2, 1e-4);
  EXPECT_NEAR(physics.bodyPose(node)->position.y, 4, 1e-5);
}

TEST(PhysicsSceneTests, DetachPreservesAnotherPendingPausedEditAndVelocity) {
  for (bool restore : {false, true}) {
    for (bool resumeBeforeDetach : {false, true}) {
      SCOPED_TRACE(restore);
      SCOPED_TRACE(resumeBeforeDetach);
      SceneGraph graph;
      const auto kept = graph.createNode(glm::mat4{1}, 0u);
      const auto removed = graph.createNode(translation(20, 0, 0), 0u);
      PhysicsScene physics{noGravity()};
      (void)physics.attachBox(graph, kept, glm::vec3{-.5f}, glm::vec3{.5f},
          {.linearVelocity = {2, 0, 0}});
      (void)physics.attachBox(graph, removed, glm::vec3{-.5f}, glm::vec3{.5f},
          {.motionType = PhysicsMotionType::Static});
      physics.singleStep(graph);
      const auto rotation = glm::angleAxis(.4f, glm::vec3{0, 0, 1});
      const auto edit = translation(10, 20, 30) * glm::mat4_cast(rotation) *
          glm::scale(glm::mat4{1}, glm::vec3{2, 1.5f, .5f});
      graph.setLocalTransform(kept, edit);
      if (resumeBeforeDetach)
        physics.setPlaying(true);
      ASSERT_TRUE(physics.detach(graph, removed, restore));
      expectMatrixNear(graph.getNode(kept)->worldTransform, edit);
      physics.update(graph, 0);
      EXPECT_EQ(physics.statistics().totalSteps, 1u);
      if (!resumeBeforeDetach)
        expectPositionNear(physics.bodyPose(kept)->position, {10, 20, 30});
      physics.setPlaying(true);
      physics.update(graph, PhysicsScene::kFixedStepSeconds);
      expectPositionNear(physics.bodyPose(kept)->position,
          {10 + 2 * PhysicsScene::kFixedStepSeconds, 20, 30}, 1e-4);
      EXPECT_NEAR(glm::length(glm::vec3(graph.getNode(kept)->worldTransform[0])), 2, 1e-5);
    }
  }
}

TEST(PhysicsSceneTests, DetachDuringPlaybackPreservesPendingKinematicTravel) {
  SceneGraph graph;
  const auto pusher = graph.createNode(translation(-2, 0, 0), 0u);
  const auto pushed = graph.createNode(glm::mat4{1}, 0u);
  const auto removed = graph.createNode(translation(20, 0, 0), 0u);
  PhysicsScene physics{noGravity()};
  (void)physics.attachBox(graph, pusher, glm::vec3{-.5f}, glm::vec3{.5f},
      {.motionType = PhysicsMotionType::Kinematic, .friction = 0});
  (void)physics.attachBox(graph, pushed, glm::vec3{-.5f}, glm::vec3{.5f}, {.friction = 0});
  (void)physics.attachBox(graph, removed, glm::vec3{-.5f}, glm::vec3{.5f},
      {.motionType = PhysicsMotionType::Static});
  physics.setPlaying(true);
  graph.setLocalTransform(pusher, translation(2, 0, 0));
  ASSERT_TRUE(physics.detach(graph, removed));
  expectPositionNear(physics.bodyPose(pusher)->position, {-2, 0, 0});
  physics.update(graph, PhysicsScene::kFixedStepSeconds * 4);
  expectPositionNear(physics.bodyPose(pusher)->position, {2, 0, 0}, 1e-4);
  EXPECT_GT(physics.bodyPose(pushed)->position.x, 2.5);
}

TEST(PhysicsSceneTests, DetachDynamicAncestorRetainsDescendantWorldPoseOrEdit) {
  for (const auto [restore, editChild] : std::array{
           std::pair{false, false}, std::pair{false, true},
           std::pair{true, false}, std::pair{true, true}}) {
    SCOPED_TRACE(restore);
    SCOPED_TRACE(editChild);
    SceneGraph graph;
    const auto parent = graph.createNode(glm::mat4{1}, 0u);
    const auto child = graph.createNode(translation(2, 0, 0), 0u);
    ASSERT_TRUE(graph.setParent(child, parent));
    PhysicsScene physics{noGravity()};
    (void)physics.attachBox(graph, parent, glm::vec3{-.5f}, glm::vec3{.5f},
        {.linearVelocity = {1, 0, 0}});
    (void)physics.attachBox(graph, child, glm::vec3{-.5f}, glm::vec3{.5f},
        {.linearVelocity = {2, 0, 0}});
    physics.singleStep(graph);
    if (editChild) {
      graph.setLocalTransform(child, translation(10, 4, 0));
      graph.updateWorldTransforms();
    }
    const auto expectedWorld = graph.getNode(child)->worldTransform;
    ASSERT_TRUE(physics.detach(graph, parent, restore));
    expectMatrixNear(graph.getNode(child)->worldTransform, expectedWorld);
    physics.update(graph, 0);
    expectPositionNear(physics.bodyPose(child)->position, glm::dvec3{expectedWorld[3]}, 1e-4);
  }
}

TEST(PhysicsSceneTests, RejectedAuthoredBatchPreservesEveryBindingAndJoint) {
  for (int invalidKind = 0; invalidKind < 3; ++invalidKind) {
    for (bool swapNodes : {false, true}) {
      SCOPED_TRACE(invalidKind);
      SCOPED_TRACE(swapNodes);
      SceneGraph graph;
      const auto first = graph.createNode(glm::mat4{1}, 0u);
      const auto second = graph.createNode(glm::mat4{1}, 0u);
      const auto good = swapNodes ? second : first;
      const auto bad = swapNodes ? first : second;
      const auto badInitial = invalidKind == 2
          ? glm::scale(glm::mat4{1}, glm::vec3{1e-6f}) : glm::mat4{1};
      graph.setLocalTransform(bad, badInitial);
      PhysicsScene physics{noGravity()};
      // Keep insertion order unchanged while swapping the valid/invalid edit.
      // One variant reaches the valid candidate first for any map iteration.
      for (const auto node : {first, second}) {
        const float half = invalidKind == 2 && node == bad ? 1e20f : .5f;
        PhysicsBodySettings settings;
        if (invalidKind == 2 && node == bad)
          settings.motionType = PhysicsMotionType::Static;
        (void)physics.attachBox(graph, node, glm::vec3{-half}, glm::vec3{half}, settings);
      }
      container::physics::PhysicsJointSettings jointSettings;
      jointSettings.type = container::physics::PhysicsJointType::Fixed;
      const auto joint = physics.createJoint(graph, std::nullopt, good, jointSettings);
      const auto firstId = physics.bodyId(first), secondId = physics.bodyId(second);
      const auto firstPose = *physics.bodyPose(first), secondPose = *physics.bodyPose(second);
      const auto oldJoint = *physics.jointState(joint);
      graph.setLocalTransform(good, translation(10, 2, 0) * glm::scale(glm::mat4{1}, glm::vec3{2}));
      glm::mat4 rejected{1};
      if (invalidKind == 0)
        rejected[1][0] = .25f;
      else if (invalidKind == 1)
        rejected[3].x = std::numeric_limits<float>::max();
      else
        rejected = glm::scale(glm::mat4{1}, glm::vec3{1e19f});
      graph.setLocalTransform(bad, rejected);
      const auto statistics = physics.statistics();
      const auto revision = graph.revision();
      EXPECT_THROW(physics.update(graph, 0), std::invalid_argument);
      EXPECT_EQ(physics.bodyId(first), firstId);
      EXPECT_EQ(physics.bodyId(second), secondId);
      expectPositionNear(physics.bodyPose(first)->position, firstPose.position);
      expectPositionNear(physics.bodyPose(second)->position, secondPose.position);
      ASSERT_TRUE(physics.jointState(joint));
      EXPECT_EQ(physics.jointState(joint)->state.bodyB, oldJoint.state.bodyB);
      EXPECT_EQ(physics.bodyCount(), 2u);
      EXPECT_EQ(physics.jointCount(), 1u);
      EXPECT_EQ(physics.statistics().totalSteps, statistics.totalSteps);
      EXPECT_EQ(physics.statistics().authoredPoseUpdates, statistics.authoredPoseUpdates);
      EXPECT_EQ(physics.statistics().transformPublications, statistics.transformPublications);
      EXPECT_EQ(graph.revision(), revision);
      graph.setLocalTransform(bad, badInitial);
      EXPECT_NO_THROW(physics.update(graph, 0));
      expectPositionNear(physics.bodyPose(good)->position, {10, 2, 0});
      EXPECT_TRUE(physics.jointState(joint));
    }
  }
}

TEST(PhysicsSceneTests, RejectedDetachPublicationRetainsBodyJointsAndGraph) {
  for (bool restore : {false, true}) {
    SCOPED_TRACE(restore);
    SceneGraph graph;
    const auto parent = graph.createNode(glm::mat4{1}, 0u);
    const auto kept = graph.createNode(glm::mat4{1}, 0u);
    const auto removed = graph.createNode(translation(20, 0, 0), 0u);
    ASSERT_TRUE(graph.setParent(kept, parent));
    PhysicsScene physics{noGravity()};
    (void)physics.attachBox(graph, kept, glm::vec3{-.5f}, glm::vec3{.5f});
    const auto removedId = physics.attachBox(graph, removed, glm::vec3{-.5f}, glm::vec3{.5f});
    container::physics::PhysicsJointSettings settings;
    settings.type = container::physics::PhysicsJointType::Fixed;
    settings.anchor = {20, 0, 0};
    const auto joint = physics.createJoint(graph, std::nullopt, removed, settings);
    glm::mat4 shear{1};
    shear[1][0] = .25f;
    graph.setLocalTransform(parent, shear);
    graph.setLocalTransform(removed, translation(30, 0, 0));
    const auto revision = graph.revision();
    const auto statistics = physics.statistics();
    EXPECT_THROW((void)physics.detach(graph, removed, restore), std::invalid_argument);
    EXPECT_EQ(physics.bodyId(removed), removedId);
    EXPECT_EQ(physics.bodyCount(), 2u);
    ASSERT_TRUE(physics.jointState(joint));
    EXPECT_EQ(physics.jointState(joint)->state.bodyB, removedId);
    expectMatrixNear(graph.getNode(removed)->localTransform, translation(30, 0, 0));
    EXPECT_EQ(graph.revision(), revision);
    EXPECT_EQ(physics.statistics().transformPublications, statistics.transformPublications);
    graph.setLocalTransform(parent, glm::mat4{1});
    EXPECT_TRUE(physics.detach(graph, removed, restore));
    EXPECT_EQ(physics.bodyCount(), 1u);
    EXPECT_FALSE(physics.jointState(joint));
  }
}

TEST(PhysicsSceneTests, DetachCanRetireItsOwnInvalidEditedTransform) {
  for (bool restore : {false, true}) {
    SceneGraph graph;
    const auto kept = graph.createNode(glm::mat4{1}, 0u);
    const auto removed = graph.createNode(translation(20, 0, 0), 0u);
    PhysicsScene physics{noGravity()};
    (void)physics.attachBox(graph, kept, glm::vec3{-.5f}, glm::vec3{.5f});
    (void)physics.attachBox(graph, removed, glm::vec3{-.5f}, glm::vec3{.5f});
    glm::mat4 shear{1};
    shear[1][0] = .25f;
    graph.setLocalTransform(removed, shear);
    graph.setLocalTransform(kept, translation(5, 2, 0));
    EXPECT_TRUE(physics.detach(graph, removed, restore));
    EXPECT_FALSE(physics.hasBody(removed));
    expectMatrixNear(graph.getNode(kept)->worldTransform, translation(5, 2, 0));
    EXPECT_NO_THROW(physics.update(graph, 0));
    expectPositionNear(physics.bodyPose(kept)->position, {5, 2, 0});
  }
}

TEST(PhysicsSceneTests, RejectedRestoredTargetOverflowPreservesEndpointAndGraph) {
  SceneGraph graph;
  const float extent = std::numeric_limits<float>::max();
  const auto parent = graph.createNode(translation(-extent, 0, 0), 0u);
  const auto node = graph.createNode(translation(extent, 0, 0), 0u);
  ASSERT_TRUE(graph.setParent(node, parent));
  PhysicsScene physics{noGravity()};
  const auto body = physics.attachBox(graph, node, glm::vec3{-.5f}, glm::vec3{.5f});
  container::physics::PhysicsJointSettings settings;
  settings.type = container::physics::PhysicsJointType::Fixed;
  const auto joint = physics.createJoint(graph, std::nullopt, node, settings);
  // Both current and initial body poses are at the origin. Restoring the old
  // local under the edited parent alone would overflow the visual world.
  graph.setLocalTransform(parent, translation(extent, 0, 0));
  graph.setLocalTransform(node, translation(-extent, 0, 0));
  const auto revision = graph.revision();
  const auto statistics = physics.statistics();
  EXPECT_THROW((void)physics.detach(graph, node, true), std::invalid_argument);
  EXPECT_EQ(physics.bodyId(node), body);
  expectPositionNear(physics.bodyPose(node)->position, {0, 0, 0});
  ASSERT_TRUE(physics.jointState(joint));
  EXPECT_EQ(physics.jointState(joint)->state.bodyB, body);
  expectMatrixNear(graph.getNode(node)->localTransform, translation(-extent, 0, 0));
  EXPECT_EQ(graph.revision(), revision);
  EXPECT_EQ(physics.statistics().transformPublications, statistics.transformPublications);
  EXPECT_TRUE(physics.detach(graph, node, false));
  EXPECT_EQ(physics.bodyCount(), 0u);
  EXPECT_EQ(physics.jointCount(), 0u);
}

TEST(PhysicsSceneTests, RejectedJointRequestDoesNotCommitPendingBodyEdits) {
  for (bool incompatibleMotion : {false, true}) {
    SCOPED_TRACE(incompatibleMotion);
    SceneGraph graph;
    const auto edited = graph.createNode(glm::mat4{1}, 0u);
    const auto staticA = graph.createNode(translation(20, 0, 0), 0u);
    const auto staticB = graph.createNode(translation(30, 0, 0), 0u);
    PhysicsScene physics{noGravity()};
    const auto body = physics.attachBox(graph, edited, glm::vec3{-.5f}, glm::vec3{.5f});
    (void)physics.attachBox(graph, staticA, glm::vec3{-.5f}, glm::vec3{.5f},
        {.motionType = PhysicsMotionType::Static});
    (void)physics.attachBox(graph, staticB, glm::vec3{-.5f}, glm::vec3{.5f},
        {.motionType = PhysicsMotionType::Static});
    container::physics::PhysicsJointSettings existing;
    existing.type = container::physics::PhysicsJointType::Fixed;
    const auto joint = physics.createJoint(graph, std::nullopt, edited, existing);
    graph.setLocalTransform(edited, translation(10, 2, 0) * glm::scale(glm::mat4{1}, glm::vec3{2}));
    const auto revision = graph.revision();
    const auto statistics = physics.statistics();
    container::physics::PhysicsJointSettings rejected;
    if (!incompatibleMotion)
      rejected.axis = glm::vec3{0};
    EXPECT_THROW((void)physics.createJoint(graph,
        incompatibleMotion ? std::optional{staticA} : std::nullopt,
        incompatibleMotion ? staticB : edited, rejected), std::invalid_argument);
    EXPECT_EQ(physics.bodyId(edited), body);
    expectPositionNear(physics.bodyPose(edited)->position, {0, 0, 0});
    ASSERT_TRUE(physics.jointState(joint));
    EXPECT_EQ(physics.jointState(joint)->state.bodyB, body);
    EXPECT_EQ(physics.jointCount(), 1u);
    EXPECT_EQ(graph.revision(), revision);
    EXPECT_EQ(physics.statistics().authoredPoseUpdates, statistics.authoredPoseUpdates);
    EXPECT_EQ(physics.statistics().transformPublications, statistics.transformPublications);
  }
}

TEST(PhysicsSceneTests, CreatingUnrelatedJointPreservesPlayingKinematicContactSweep) {
  SceneGraph graph, referenceGraph;
  PhysicsScene physics{noGravity()}, reference{noGravity()};
  const auto populate = [](SceneGraph &targetGraph, PhysicsScene &targetPhysics) {
    const auto pusher = targetGraph.createNode(translation(-2, 0, 0), 0u);
    const auto pushed = targetGraph.createNode(glm::mat4{1}, 0u);
    const auto unrelated = targetGraph.createNode(translation(20, 0, 0), 0u);
    (void)targetPhysics.attachBox(targetGraph, pusher, glm::vec3{-.5f}, glm::vec3{.5f},
        {.motionType = PhysicsMotionType::Kinematic, .friction = 0});
    (void)targetPhysics.attachBox(targetGraph, pushed, glm::vec3{-.5f}, glm::vec3{.5f},
        {.friction = 0});
    (void)targetPhysics.attachBox(targetGraph, unrelated, glm::vec3{-.5f}, glm::vec3{.5f});
    targetPhysics.setPlaying(true);
    targetGraph.setLocalTransform(pusher, translation(2, 0, 0));
    return std::array{pusher, pushed, unrelated};
  };
  const auto nodes = populate(graph, physics);
  const auto referenceNodes = populate(referenceGraph, reference);
  container::physics::PhysicsJointSettings settings;
  settings.type = container::physics::PhysicsJointType::Fixed;
  settings.anchor = {20, 0, 0};
  const auto joint = physics.createJoint(graph, std::nullopt, nodes[2], settings);
  // The new joint is unrelated to the pending support movement. Its creation
  // must not consume that movement as a teleport before collision detection.
  expectPositionNear(physics.bodyPose(nodes[0])->position, {-2, 0, 0});
  physics.update(graph, 4 * PhysicsScene::kFixedStepSeconds);
  reference.update(referenceGraph, 4 * PhysicsScene::kFixedStepSeconds);
  expectPositionNear(physics.bodyPose(nodes[0])->position, {2, 0, 0}, 1e-4);
  EXPECT_GT(reference.bodyPose(referenceNodes[1])->position.x, 2.5);
  expectPositionNear(physics.bodyPose(nodes[1])->position,
      reference.bodyPose(referenceNodes[1])->position, 1e-3);
  EXPECT_EQ(physics.statistics().totalSteps, 4u);
  EXPECT_TRUE(physics.playing());
  EXPECT_TRUE(physics.jointState(joint));
}

TEST(PhysicsSceneTests, PausedJointCreationImmediatelySynchronizesKinematicEndpoint) {
  SceneGraph graph;
  const auto first = graph.createNode(translation(-2, 0, 0), 0u);
  const auto second = graph.createNode(translation(5, 0, 0), 0u);
  PhysicsScene physics{noGravity()};
  const auto firstBody = physics.attachBox(graph, first, glm::vec3{-.5f}, glm::vec3{.5f},
      {.motionType = PhysicsMotionType::Kinematic});
  const auto secondBody = physics.attachBox(graph, second, glm::vec3{-.5f}, glm::vec3{.5f});
  graph.setLocalTransform(first, translation(2, 0, 0));
  container::physics::PhysicsJointSettings settings;
  settings.type = container::physics::PhysicsJointType::Fixed;
  settings.anchor = {3.5, 0, 0};
  const auto joint = physics.createJoint(graph, first, second, settings);
  expectPositionNear(physics.bodyPose(first)->position, {2, 0, 0});
  const auto state = *physics.jointState(joint);
  EXPECT_EQ(state.state.bodyA, firstBody);
  EXPECT_EQ(state.state.bodyB, secondBody);
  expectPositionNear(state.state.frameA.anchor, {1.5, 0, 0});
  expectPositionNear(state.state.frameB.anchor, {-1.5, 0, 0});
  EXPECT_FALSE(physics.playing());
  EXPECT_EQ(physics.statistics().totalSteps, 0u);
  physics.singleStep(graph);
  expectPositionNear(physics.bodyPose(first)->position, {2, 0, 0}, .002);
  expectPositionNear(physics.bodyPose(second)->position, {5, 0, 0}, .002);
}

TEST(PhysicsSceneTests, PlayingJointUsesCurrentEndpointFramesUntilAnAdvancingTick) {
  SceneGraph graph;
  const auto first = graph.createNode(translation(-2, 0, 0), 0u);
  const auto second = graph.createNode(translation(4, 0, 0), 0u);
  PhysicsScene physics{noGravity()};
  (void)physics.attachBox(graph, first, glm::vec3{-.5f}, glm::vec3{.5f},
      {.motionType = PhysicsMotionType::Kinematic});
  (void)physics.attachBox(graph, second, glm::vec3{-.5f}, glm::vec3{.5f});
  physics.setPlaying(true);
  graph.setLocalTransform(first, translation(2, 0, 0));
  container::physics::PhysicsJointSettings settings;
  settings.type = container::physics::PhysicsJointType::Fixed;
  const auto joint = physics.createJoint(graph, first, second, settings);
  const auto frames = *physics.jointState(joint);
  expectPositionNear(frames.state.frameA.anchor, {2, 0, 0});
  expectPositionNear(frames.state.frameB.anchor, {-4, 0, 0});
  expectPositionNear(physics.bodyPose(first)->position, {-2, 0, 0});
  physics.update(graph, PhysicsScene::kFixedStepSeconds / 2);
  EXPECT_EQ(physics.statistics().totalSteps, 0u);
  expectPositionNear(physics.bodyPose(first)->position, {-2, 0, 0});
  physics.update(graph, 3.5 * PhysicsScene::kFixedStepSeconds);
  EXPECT_EQ(physics.statistics().totalSteps, 4u);
  expectPositionNear(physics.bodyPose(first)->position, {2, 0, 0}, 1e-4);
  for (int tick = 0; tick < 30; ++tick)
    physics.update(graph, PhysicsScene::kFixedStepSeconds);
  expectPositionNear(physics.bodyPose(second)->position, {8, 0, 0}, .01);
  const auto after = *physics.jointState(joint);
  expectPositionNear(after.state.frameA.anchor, frames.state.frameA.anchor);
  expectPositionNear(after.state.frameB.anchor, frames.state.frameB.anchor);
  EXPECT_TRUE(physics.playing());
}

TEST(PhysicsSceneTests, UnchangedPausedAndStaticScenesReuseTransformPublications) {
  SceneGraph graph;
  const auto parent = graph.createNode(translation(1.0f, 2.0f, 3.0f), 0u);
  const auto node = graph.createNode(glm::mat4{1.0f}, 0u);
  ASSERT_TRUE(graph.setParent(node, parent));
  PhysicsScene physics;
  PhysicsBodySettings settings;
  settings.motionType = PhysicsMotionType::Static;
  (void)physics.attachBox(graph, node, glm::vec3{-0.5f}, glm::vec3{0.5f}, settings);
  physics.update(graph, 0.0);
  EXPECT_EQ(physics.statistics().transformPublications, 1u);
  EXPECT_EQ(physics.statistics().authoredPoseUpdates, 0u);
  for (int frame = 0; frame < 100; ++frame)
    physics.update(graph, PhysicsScene::kFixedStepSeconds);
  EXPECT_EQ(physics.statistics().transformPublications, 1u);
  EXPECT_EQ(physics.statistics().authoredPoseUpdates, 0u);

  graph.setLocalTransform(parent, translation(2.0f, 3.0f, 4.0f));
  physics.update(graph, 0.0);
  EXPECT_EQ(physics.statistics().transformPublications, 2u);
  EXPECT_EQ(physics.statistics().authoredPoseUpdates, 1u);
  expectPositionNear(physics.bodyPose(node)->position, {2.0, 3.0, 4.0});

  physics.setPlaying(true);
  for (int frame = 0; frame < 10; ++frame)
    physics.update(graph, PhysicsScene::kFixedStepSeconds);
  EXPECT_EQ(physics.statistics().totalSteps, 10u);
  EXPECT_EQ(physics.statistics().transformPublications, 2u);
  EXPECT_EQ(physics.statistics().authoredPoseUpdates, 1u);
  physics.setPlaying(false);
  graph.setLocalTransform(parent, translation(3.0f, 4.0f, 5.0f));
  physics.update(graph, 0.0);
  EXPECT_EQ(physics.statistics().transformPublications, 3u);
  EXPECT_EQ(physics.statistics().authoredPoseUpdates, 2u);
}

TEST(PhysicsSceneTests, OffCenterBoundsUseTransformedParentAndSignedWorldScale) {
  SceneGraph graph;
  const auto parentTransform = translation(10.0f, 20.0f, -3.0f) *
      glm::rotate(glm::mat4{1.0f}, 0.6f, glm::vec3{0.0f, 0.0f, 1.0f}) *
      glm::scale(glm::mat4{1.0f}, glm::vec3{2.0f});
  const auto parent = graph.createNode(parentTransform, 0u);
  const auto local = translation(1.0f, 2.0f, 3.0f) *
      glm::rotate(glm::mat4{1.0f}, 0.35f, glm::vec3{0.0f, 1.0f, 0.0f}) *
      glm::scale(glm::mat4{1.0f}, glm::vec3{-1.0f, 3.0f, 2.0f});
  const auto node = graph.createNode(local, 0u);
  ASSERT_TRUE(graph.setParent(node, parent));
  graph.updateWorldTransforms();
  const auto originalWorld = graph.getNode(node)->worldTransform;
  const glm::vec3 boundsMin{1.0f, -2.0f, 0.0f};
  const glm::vec3 boundsMax{5.0f, 0.0f, 2.0f};
  const glm::vec3 center = (boundsMin + boundsMax) * 0.5f;
  const glm::dvec3 expectedCenter{originalWorld * glm::vec4{center, 1.0f}};
  PhysicsScene physics{noGravity()};
  PhysicsBodySettings settings;
  settings.linearVelocity = {0.0f, -2.0f, 0.0f};
  (void)physics.attachBox(graph, node, boundsMin, boundsMax, settings);
  ASSERT_TRUE(physics.bodyPose(node));
  expectPositionNear(physics.bodyPose(node)->position, expectedCenter);
  physics.singleStep(graph);
  const auto expectedWorld = translation(0.0f, -2.0f / 60.0f, 0.0f) * originalWorld;
  expectMatrixNear(graph.getNode(node)->worldTransform, expectedWorld);
  expectMatrixNear(graph.getNode(node)->localTransform, glm::inverse(parentTransform) * expectedWorld);
  const glm::dvec3 renderedCenter{graph.getNode(node)->worldTransform * glm::vec4{center, 1.0f}};
  expectPositionNear(renderedCenter, physics.bodyPose(node)->position);
  EXPECT_LT(glm::determinant(glm::mat3{graph.getNode(node)->worldTransform}), 0.0f);
}

TEST(PhysicsSceneTests, ResetRestoresInitialLinearAndAngularVelocity) {
  SceneGraph graph;
  const auto node = graph.createNode(translation(0.0f, 5.0f, 0.0f), 0u);
  PhysicsScene physics{noGravity()};
  PhysicsBodySettings settings;
  settings.linearVelocity = {2.0f, 0.0f, 0.0f};
  settings.angularVelocity = {0.0f, 1.0f, 0.0f};
  (void)physics.attachBox(graph, node, glm::vec3{-0.5f}, glm::vec3{0.5f}, settings);
  physics.singleStep(graph);
  const auto firstStep = graph.getNode(node)->worldTransform;
  for (int frame = 0; frame < 20; ++frame)
    physics.singleStep(graph);
  physics.reset(graph);
  EXPECT_FALSE(physics.playing());
  physics.singleStep(graph);
  expectMatrixNear(graph.getNode(node)->worldTransform, firstStep);
}

TEST(PhysicsSceneTests, StaticAndKinematicBodiesTrackAuthoredParentTransforms) {
  SceneGraph graph;
  const auto parent = graph.createNode(translation(1.0f, 2.0f, 3.0f), 0u);
  const auto staticNode = graph.createNode(translation(0.0f, 0.0f, 0.0f), 0u);
  const auto kinematicNode = graph.createNode(translation(5.0f, 0.0f, 0.0f), 0u);
  ASSERT_TRUE(graph.setParent(staticNode, parent));
  ASSERT_TRUE(graph.setParent(kinematicNode, parent));
  PhysicsScene physics;
  PhysicsBodySettings settings;
  settings.motionType = PhysicsMotionType::Static;
  (void)physics.attachBox(graph, staticNode, glm::vec3{-0.5f}, glm::vec3{0.5f}, settings);
  settings.motionType = PhysicsMotionType::Kinematic;
  (void)physics.attachBox(graph, kinematicNode, glm::vec3{-0.5f}, glm::vec3{0.5f}, settings);
  graph.setLocalTransform(parent, translation(10.0f, 20.0f, 30.0f));
  physics.update(graph, 0.0);
  ASSERT_TRUE(physics.bodyPose(staticNode));
  ASSERT_TRUE(physics.bodyPose(kinematicNode));
  expectPositionNear(physics.bodyPose(staticNode)->position, {10.0, 20.0, 30.0});
  expectPositionNear(physics.bodyPose(kinematicNode)->position, {15.0, 20.0, 30.0});
  physics.setPlaying(true);
  graph.setLocalTransform(parent, translation(11.0f, 21.0f, 31.0f));
  physics.update(graph, PhysicsScene::kFixedStepSeconds);
  expectPositionNear(physics.bodyPose(staticNode)->position, {11.0, 21.0, 31.0});
  expectPositionNear(physics.bodyPose(kinematicNode)->position, {16.0, 21.0, 31.0}, 1e-4);
}

TEST(PhysicsSceneTests, NestedDynamicBindingsResolveDesiredAncestorsBeforeWrites) {
  SceneGraph graph;
  const auto parent = graph.createNode(translation(0.0f, 20.0f, 0.0f), 0u);
  const auto middle = graph.createNode(translation(0.0f, 5.0f, 0.0f), 0u);
  const auto child = graph.createNode(translation(0.0f, 5.0f, 0.0f), 0u);
  ASSERT_TRUE(graph.setParent(middle, parent));
  ASSERT_TRUE(graph.setParent(child, middle));
  PhysicsScene physics;
  (void)physics.attachBox(graph, child, glm::vec3{-0.5f}, glm::vec3{0.5f});
  (void)physics.attachBox(graph, parent, glm::vec3{-0.5f}, glm::vec3{0.5f});
  physics.setPlaying(true);
  for (int frame = 0; frame < 30; ++frame)
    physics.update(graph, PhysicsScene::kFixedStepSeconds);
  EXPECT_NEAR(graph.getNode(child)->worldTransform[3].y -
              graph.getNode(parent)->worldTransform[3].y, 10.0f, 1e-4f);
  EXPECT_NEAR(graph.getNode(child)->localTransform[3].y, 5.0f, 1e-4f);
  expectPositionNear(glm::dvec3{graph.getNode(child)->worldTransform[3]},
                       physics.bodyPose(child)->position);
  expectPositionNear(glm::dvec3{graph.getNode(parent)->worldTransform[3]},
                       physics.bodyPose(parent)->position);
}

TEST(PhysicsSceneTests, MixedDynamicAndNonDynamicAncestorBindingsAreRejected) {
  SceneGraph graph;
  const auto parent = graph.createNode(translation(0.0f, 20.0f, 0.0f), 0u);
  const auto child = graph.createNode(translation(0.0f, 5.0f, 0.0f), 0u);
  ASSERT_TRUE(graph.setParent(child, parent));
  PhysicsScene physics;
  PhysicsBodySettings settings;
  settings.motionType = PhysicsMotionType::Static;
  (void)physics.attachBox(graph, parent, glm::vec3{-0.5f}, glm::vec3{0.5f}, settings);
  EXPECT_THROW((void)physics.attachBox(graph, child, glm::vec3{-0.5f}, glm::vec3{0.5f}),
               std::invalid_argument);
  EXPECT_EQ(physics.bodyCount(), 1u);
  physics.clear();
  (void)physics.attachBox(graph, parent, glm::vec3{-0.5f}, glm::vec3{0.5f});
  EXPECT_THROW((void)physics.attachBox(graph, child, glm::vec3{-0.5f}, glm::vec3{0.5f}, settings),
               std::invalid_argument);
  EXPECT_EQ(physics.bodyCount(), 1u);
}

TEST(PhysicsSceneTests, GraphReplacementDropsBodiesWithoutRestoringReusedIndices) {
  SceneGraph graph;
  const auto node = graph.createNode(translation(0.0f, 20.0f, 0.0f), 0u);
  PhysicsScene physics;
  (void)physics.attachBox(graph, node, glm::vec3{-0.5f}, glm::vec3{0.5f});
  physics.setPlaying(true);
  physics.update(graph, PhysicsScene::kFixedStepSeconds);
  const auto oldIdentity = graph.identity();
  graph = SceneGraph{};
  const auto reused = graph.createNode(translation(-100.0f, -200.0f, -300.0f), 0u);
  ASSERT_EQ(reused, node);
  ASSERT_NE(graph.identity(), oldIdentity);
  const auto replacement = graph.getNode(reused)->localTransform;
  physics.update(graph, PhysicsScene::kFixedStepSeconds);
  EXPECT_EQ(physics.bodyCount(), 0u);
  EXPECT_FALSE(physics.playing());
  expectMatrixNear(graph.getNode(reused)->localTransform, replacement);
  physics.clear(&graph);
  expectMatrixNear(graph.getNode(reused)->localTransform, replacement);
  (void)physics.attachBox(graph, reused, glm::vec3{-0.5f}, glm::vec3{0.5f});
  EXPECT_EQ(physics.bodyCount(), 1u);
}

TEST(PhysicsSceneTests, DetachAndClearHonorAuthoredRestorationChoice) {
  SceneGraph graph;
  const auto node = graph.createNode(translation(0.0f, 20.0f, 0.0f), 0u);
  const auto authored = graph.getNode(node)->localTransform;
  PhysicsScene physics;
  (void)physics.attachBox(graph, node, glm::vec3{-0.5f}, glm::vec3{0.5f});
  physics.singleStep(graph);
  const auto simulated = graph.getNode(node)->localTransform;
  ASSERT_TRUE(physics.detach(graph, node, false));
  expectMatrixNear(graph.getNode(node)->localTransform, simulated);
  EXPECT_FALSE(physics.detach(graph, node));
  EXPECT_FALSE(physics.bodyPose(node));
  graph.setLocalTransform(node, authored);
  (void)physics.attachBox(graph, node, glm::vec3{-0.5f}, glm::vec3{0.5f});
  physics.singleStep(graph);
  ASSERT_TRUE(physics.detach(graph, node));
  expectMatrixNear(graph.getNode(node)->localTransform, authored);
  (void)physics.attachBox(graph, node, glm::vec3{-0.5f}, glm::vec3{0.5f});
  physics.singleStep(graph);
  physics.clear(&graph);
  EXPECT_EQ(physics.bodyCount(), 0u);
  expectMatrixNear(graph.getNode(node)->localTransform, authored);
}

TEST(PhysicsSceneTests, ScaleEditsRebuildCollidersAndPreserveSimulatedVelocity) {
  SceneGraph graph;
  const auto node = graph.createNode(translation(0.0f, 5.0f, 0.0f), 0u);
  PhysicsScene physics{noGravity()};
  PhysicsBodySettings settings;
  settings.linearVelocity = {2.0f, 0.0f, 0.0f};
  (void)physics.attachBox(graph, node, glm::vec3{-0.5f}, glm::vec3{0.5f}, settings);
  physics.singleStep(graph);
  const auto firstBody = physics.bodyId(node);
  const auto before = graph.getNode(node)->localTransform;
  graph.setLocalTransform(node, before * glm::scale(glm::mat4{1.0f}, glm::vec3{2.0f}));
  physics.update(graph, 0.0);
  EXPECT_NE(physics.bodyId(node), firstBody);
  physics.singleStep(graph);
  EXPECT_NEAR(graph.getNode(node)->worldTransform[3].x,
              before[3].x + 2.0f / 60.0f, 1e-4f);
  EXPECT_NEAR(glm::length(glm::vec3{graph.getNode(node)->worldTransform[0]}), 2.0f, 1e-4f);
}

TEST(PhysicsSceneTests, PlayingKinematicRebuildPreservesContactSweep) {
  for (bool replace : {false, true}) {
    SCOPED_TRACE(replace);
    SceneGraph graph, referenceGraph;
    PhysicsScene physics{noGravity()}, reference{noGravity()};
    const auto populate = [](SceneGraph &targetGraph, PhysicsScene &targetPhysics) {
      const auto pusher = targetGraph.createNode(translation(-2, 0, 0), 0u);
      const auto pushed = targetGraph.createNode(glm::mat4{1}, 0u);
      (void)targetPhysics.attachBox(targetGraph, pusher, glm::vec3{-.5f}, glm::vec3{.5f},
          {.motionType = PhysicsMotionType::Kinematic, .friction = 0});
      (void)targetPhysics.attachBox(targetGraph, pushed, glm::vec3{-.5f}, glm::vec3{.5f},
          {.friction = 0});
      return std::array{pusher, pushed};
    };
    const auto nodes = populate(graph, physics);
    const auto referenceNodes = populate(referenceGraph, reference);
    const auto scaled = [](float x) {
      return glm::scale(translation(x, 0, 0), glm::vec3{1.2f});
    };
    PhysicsBodySettings settings;
    settings.motionType = PhysicsMotionType::Kinematic;
    settings.mass = 2;
    settings.friction = 0;
    // The reference receives the same shape/settings before motion begins.
    referenceGraph.setLocalTransform(referenceNodes[0], scaled(-2));
    if (replace)
      (void)reference.replaceBox(referenceGraph, referenceNodes[0], glm::vec3{-.5f}, glm::vec3{.5f}, settings);
    else
      reference.update(referenceGraph, 0);
    reference.setPlaying(true);
    referenceGraph.setLocalTransform(referenceNodes[0], scaled(2));
    physics.setPlaying(true);
    graph.setLocalTransform(nodes[0], scaled(2));
    if (replace)
      (void)physics.replaceBox(graph, nodes[0], glm::vec3{-.5f}, glm::vec3{.5f}, settings);
    physics.update(graph, 4 * PhysicsScene::kFixedStepSeconds);
    reference.update(referenceGraph, 4 * PhysicsScene::kFixedStepSeconds);
    EXPECT_GT(reference.bodyPose(referenceNodes[1])->position.x, 2.5);
    expectPositionNear(physics.bodyPose(nodes[1])->position,
        reference.bodyPose(referenceNodes[1])->position, 1e-3);
    expectPositionNear(physics.bodyPose(nodes[0])->position, {2, 0, 0}, 1e-4);
    EXPECT_TRUE(physics.playing());
    EXPECT_EQ(physics.statistics().totalSteps, 4u);
    EXPECT_EQ(physics.bodyCount(), 2u);
  }
}

TEST(PhysicsSceneTests, SingleStepKinematicResizePreservesContactMotion) {
  SceneGraph graph, referenceGraph;
  PhysicsScene physics{noGravity()}, reference{noGravity()};
  const auto populate = [](SceneGraph &targetGraph, PhysicsScene &targetPhysics) {
    const auto pusher = targetGraph.createNode(translation(-1, 0, 0), 0u);
    const auto pushed = targetGraph.createNode(glm::mat4{1}, 0u);
    (void)targetPhysics.attachBox(targetGraph, pusher, glm::vec3{-.5f}, glm::vec3{.5f},
        {.motionType = PhysicsMotionType::Kinematic, .friction = 0});
    (void)targetPhysics.attachBox(targetGraph, pushed, glm::vec3{-.5f}, glm::vec3{.5f},
        {.friction = 0});
    return std::array{pusher, pushed};
  };
  const auto nodes = populate(graph, physics);
  const auto referenceNodes = populate(referenceGraph, reference);
  referenceGraph.setLocalTransform(referenceNodes[0],
      glm::scale(translation(-1, 0, 0), glm::vec3{1.2f}));
  reference.update(referenceGraph, 0);
  const auto target = glm::scale(glm::mat4{1}, glm::vec3{1.2f});
  graph.setLocalTransform(nodes[0], target);
  referenceGraph.setLocalTransform(referenceNodes[0], target);
  // Single step is advancing even though playback is paused.
  physics.singleStep(graph);
  reference.singleStep(referenceGraph);
  EXPECT_GT(reference.bodyPose(referenceNodes[1])->position.x, .5);
  expectPositionNear(physics.bodyPose(nodes[1])->position,
      reference.bodyPose(referenceNodes[1])->position, 1e-3);
  expectPositionNear(physics.bodyPose(nodes[0])->position, {0, 0, 0}, 1e-4);
  EXPECT_FALSE(physics.playing());
  EXPECT_EQ(physics.statistics().totalSteps, 1u);
}

TEST(PhysicsSceneTests, PlayingKinematicRebuildPreservesNodeFrameUntilTick) {
  for (bool replace : {false, true}) {
    SCOPED_TRACE(replace);
    SceneGraph graph;
    const glm::dvec3 initialOrigin{-4, 1, 0}, targetOrigin{5, 3, 0};
    const auto initialRotation = glm::angleAxis(.3f, glm::vec3{0, 0, 1});
    const auto targetRotation = glm::angleAxis(.5f, glm::vec3{0, 1, 0});
    const glm::vec3 initialScale{2, 1, .75f}, targetScale{3, 1.5f, .5f};
    const auto authored = glm::translate(glm::mat4{1}, glm::vec3{initialOrigin}) *
        glm::mat4_cast(initialRotation) * glm::scale(glm::mat4{1}, initialScale);
    const auto node = graph.createNode(authored, 0u);
    const auto unrelated = graph.createNode(translation(40, 10, 0), 0u);
    PhysicsScene physics{noGravity()};
    (void)physics.attachBox(graph, node, {1, -.5f, -.5f}, {3, .5f, .5f},
        {.motionType = PhysicsMotionType::Kinematic});
    (void)physics.attachBox(graph, unrelated, glm::vec3{-.5f}, glm::vec3{.5f});
    physics.setPlaying(true);
    graph.setLocalTransform(node, glm::translate(glm::mat4{1}, glm::vec3{targetOrigin}) *
        glm::mat4_cast(targetRotation) * glm::scale(glm::mat4{1}, targetScale));
    if (replace) {
      // Changing the bounds also changes the collider center. Keep the old
      // physical node origin rather than keeping its old center of mass.
      (void)physics.replaceBox(graph, node, {2, -.5f, -.5f}, {4, .5f, .5f},
          {.motionType = PhysicsMotionType::Kinematic, .mass = 2});
    } else {
      container::physics::PhysicsJointSettings settings;
      settings.type = container::physics::PhysicsJointType::Fixed;
      settings.anchor = {40, 10, 0};
      (void)physics.createJoint(graph, std::nullopt, unrelated, settings);
    }
    const glm::dvec3 center{replace ? 3.0 : 2.0, 0, 0};
    const auto expectedCurrent = initialOrigin + glm::dquat{initialRotation} *
        (glm::dvec3{targetScale} * center);
    expectPositionNear(physics.bodyPose(node)->position, expectedCurrent, 1e-4);
    EXPECT_NEAR(std::abs(glm::dot(physics.bodyPose(node)->rotation, initialRotation)), 1, 1e-6);
    physics.update(graph, PhysicsScene::kFixedStepSeconds / 2);
    expectPositionNear(physics.bodyPose(node)->position, expectedCurrent, 1e-4);
    EXPECT_EQ(physics.statistics().totalSteps, 0u);
    physics.update(graph, 3.5 * PhysicsScene::kFixedStepSeconds);
    const auto expectedTarget = targetOrigin + glm::dquat{targetRotation} *
        (glm::dvec3{targetScale} * center);
    expectPositionNear(physics.bodyPose(node)->position, expectedTarget, 1e-4);
    EXPECT_NEAR(std::abs(glm::dot(physics.bodyPose(node)->rotation, targetRotation)), 1, 1e-6);
    physics.reset(graph);
    expectMatrixNear(graph.getNode(node)->localTransform, authored);
    expectPositionNear(physics.bodyPose(node)->position, initialOrigin +
        glm::dquat{initialRotation} * (glm::dvec3{initialScale} * center), 1e-4);
  }
}

TEST(PhysicsSceneTests, PausedKinematicRebuildImmediatelyUsesAuthoredPose) {
  for (bool replace : {false, true}) {
    SCOPED_TRACE(replace);
    SceneGraph graph;
    const auto node = graph.createNode(translation(-4, 0, 0), 0u);
    PhysicsScene physics{noGravity()};
    (void)physics.attachBox(graph, node, {1, -.5f, -.5f}, {3, .5f, .5f},
        {.motionType = PhysicsMotionType::Kinematic});
    const auto rotation = glm::angleAxis(.5f, glm::vec3{0, 1, 0});
    const glm::vec3 scale{3, 1.5f, .5f};
    const auto target = translation(5, 3, 0) * glm::mat4_cast(rotation) *
        glm::scale(glm::mat4{1}, scale);
    graph.setLocalTransform(node, target);
    if (replace)
      (void)physics.replaceBox(graph, node, {2, -.5f, -.5f}, {4, .5f, .5f},
          {.motionType = PhysicsMotionType::Kinematic});
    else
      physics.update(graph, 0);
    const glm::dvec3 center{replace ? 3.0 : 2.0, 0, 0};
    expectPositionNear(physics.bodyPose(node)->position,
        glm::dvec3{5, 3, 0} + glm::dquat{rotation} * (glm::dvec3{scale} * center), 1e-4);
    EXPECT_NEAR(std::abs(glm::dot(physics.bodyPose(node)->rotation, rotation)), 1, 1e-6);
    EXPECT_EQ(physics.statistics().totalSteps, 0u);
    EXPECT_FALSE(physics.playing());
    expectMatrixNear(graph.getNode(node)->localTransform, target);
  }
}

TEST(PhysicsSceneTests, ImportedUnitScaleEditsAreNotLostDuringSimulation) {
  SceneGraph graph;
  const auto node = graph.createNode(glm::scale(glm::mat4{1}, glm::vec3{.001f}), 0u);
  PhysicsScene physics{noGravity()};
  const auto original = physics.attachBox(graph, node, glm::vec3{-10000}, glm::vec3{10000});
  graph.setLocalTransform(node, glm::scale(glm::mat4{1}, glm::vec3{.001005f}));
  physics.update(graph, 0);
  EXPECT_NE(physics.bodyId(node), original);
  physics.singleStep(graph);
  for (int axis = 0; axis < 3; ++axis)
    EXPECT_NEAR(glm::length(glm::vec3{graph.getNode(node)->worldTransform[axis]}), .001005f, 1e-9f);
}

TEST(PhysicsSceneTests, SparseSubtreeQueriesShareDeepAncestryAndPreservePreflightPolicy) {
  SceneGraph graph;
  const auto group = graph.createNode(glm::mat4{1}, 0u);
  const auto visualOnly = graph.createNode(glm::mat4{1}, 0u);
  for (uint32_t index = 0; index < 128; ++index) {
    const auto visual = graph.createNode(glm::mat4{1}, 0u);
    ASSERT_TRUE(graph.setParent(visual, index % 2 == 0 ? group : visualOnly));
  }
  auto common = group;
  for (uint32_t depth = 0; depth < 64; ++depth) {
    const auto next = graph.createNode(glm::mat4{1}, 0u);
    ASSERT_TRUE(graph.setParent(next, common));
    common = next;
  }
  const auto nested = graph.createNode(translation(0, 5, 0), 0u);
  ASSERT_TRUE(graph.setParent(nested, common));
  const auto sibling = graph.createNode(translation(10, 5, 0), 0u);
  ASSERT_TRUE(graph.setParent(sibling, common));
  const auto outside = graph.createNode(translation(20, 5, 0), 0u);
  PhysicsScene physics{noGravity()};
  for (const auto node : {common, nested, sibling, outside})
    (void)physics.attachBox(graph, node, glm::vec3{-.5f}, glm::vec3{.5f});
  physics.setPlaying(true);
  const auto revision = graph.revision();
  EXPECT_TRUE(physics.hasDynamicBodyInSubtree(graph, group));
  EXPECT_TRUE(physics.hasDynamicBodyInSubtree(graph, common));
  EXPECT_TRUE(physics.hasDynamicBodyInSubtree(graph, nested));
  EXPECT_FALSE(physics.hasDynamicBodyInSubtree(graph, visualOnly));
  EXPECT_FALSE(physics.hasDynamicBodyInSubtree(graph, SceneGraph::kInvalidNode));
  EXPECT_NO_THROW(physics.validateTransform(graph, group, translation(1, 2, 3)));
  auto shear = glm::mat4{1};
  shear[1][0] = .5f;
  EXPECT_THROW(physics.validateTransform(graph, group, shear), std::invalid_argument);
  EXPECT_NO_THROW(physics.validateTransform(graph, visualOnly, shear));
  EXPECT_NO_THROW(physics.validateReparent(graph, group, visualOnly));
  EXPECT_EQ(graph.revision(), revision);
  EXPECT_EQ(physics.bodyCount(), 4u);
  EXPECT_TRUE(physics.playing());
  EXPECT_EQ(physics.statistics().totalSteps, 0u);
}

TEST(PhysicsSceneTests, SubtreeQueriesFollowMotionReplacementMembershipAndGraphIdentity) {
  SceneGraph graph;
  const auto group = graph.createNode(glm::mat4{1}, 0u);
  const auto dynamic = graph.createNode(translation(0, 5, 0), 0u);
  const auto support = graph.createNode(translation(10, 5, 0), 0u);
  ASSERT_TRUE(graph.setParent(dynamic, group));
  ASSERT_TRUE(graph.setParent(support, group));
  PhysicsScene physics{noGravity()};
  (void)physics.attachBox(graph, dynamic, glm::vec3{-.5f}, glm::vec3{.5f});
  (void)physics.attachBox(graph, support, glm::vec3{-.5f}, glm::vec3{.5f},
      {.motionType = PhysicsMotionType::Static});
  const auto revision = graph.revision();
  EXPECT_TRUE(physics.hasDynamicBodyInSubtree(graph, group));
  EXPECT_FALSE(physics.hasDynamicBodyInSubtree(graph, support));
  SceneGraph replacement = graph;
  EXPECT_FALSE(physics.hasDynamicBodyInSubtree(replacement, group));
  EXPECT_TRUE(physics.hasDynamicBodyInSubtree(graph, group));
  (void)physics.replaceBox(graph, dynamic, glm::vec3{-.5f}, glm::vec3{.5f},
      {.motionType = PhysicsMotionType::Kinematic});
  EXPECT_FALSE(physics.hasDynamicBodyInSubtree(graph, dynamic));
  EXPECT_FALSE(physics.hasDynamicBodyInSubtree(graph, group));
  (void)physics.replaceBox(graph, dynamic, glm::vec3{-.5f}, glm::vec3{.5f});
  EXPECT_TRUE(physics.hasDynamicBodyInSubtree(graph, group));
  ASSERT_TRUE(physics.detach(graph, dynamic, false));
  EXPECT_FALSE(physics.hasDynamicBodyInSubtree(graph, group));
  (void)physics.replaceBox(graph, support, glm::vec3{-.5f}, glm::vec3{.5f});
  EXPECT_TRUE(physics.hasDynamicBodyInSubtree(graph, group));
  EXPECT_EQ(graph.revision(), revision);
  physics.clear();
  EXPECT_FALSE(physics.hasDynamicBodyInSubtree(graph, group));
}

TEST(PhysicsSceneTests, TransformPreflightRejectsShearedEditsWithoutChangingLiveState) {
  for (const auto motion : {PhysicsMotionType::Static, PhysicsMotionType::Dynamic}) {
    SCOPED_TRACE(static_cast<int>(motion));
    SceneGraph graph;
    const auto parent = graph.createNode(glm::scale(glm::mat4{1}, {2, 1, 1}), 0u);
    const auto node = graph.createNode(translation(0, 5, 0), 0u);
    ASSERT_TRUE(graph.setParent(node, parent));
    const auto other = graph.createNode(translation(10, 5, 0), 0u);
    PhysicsScene physics{noGravity()};
    const auto body = physics.attachBox(graph, node, glm::vec3{-.5f}, glm::vec3{.5f},
        {.motionType = motion});
    const auto otherBody = physics.attachBox(graph, other, glm::vec3{-.5f}, glm::vec3{.5f});
    container::physics::PhysicsJointSettings settings;
    settings.type = container::physics::PhysicsJointType::Fixed;
    settings.anchor = {0, 5, 0};
    const auto joint = physics.createJoint(graph, node, other, settings);
    physics.setPlaying(true);
    const auto before = physics.statistics();
    const auto revision = graph.revision();
    const auto original = graph.getNode(node)->localTransform;
    const auto pose = *physics.bodyPose(node);
    const auto otherPose = *physics.bodyPose(other);
    const auto frames = *physics.jointState(joint);
    const auto proposed = glm::rotate(original, glm::radians(45.0f), {0, 0, 1});
    EXPECT_THROW(physics.validateTransform(graph, node, proposed), std::invalid_argument);
    EXPECT_EQ(graph.revision(), revision);
    EXPECT_EQ(graph.getNode(node)->localTransform, original);
    EXPECT_EQ(physics.bodyId(node), body);
    EXPECT_EQ(physics.bodyId(other), otherBody);
    expectPositionNear(physics.bodyPose(node)->position, pose.position);
    EXPECT_EQ(physics.bodyPose(node)->rotation, pose.rotation);
    expectPositionNear(physics.bodyPose(other)->position, otherPose.position);
    ASSERT_TRUE(physics.jointState(joint));
    EXPECT_EQ(physics.jointState(joint)->state.frameA.anchor, frames.state.frameA.anchor);
    EXPECT_EQ(physics.jointState(joint)->state.frameB.anchor, frames.state.frameB.anchor);
    EXPECT_EQ(physics.statistics().authoredPoseUpdates, before.authoredPoseUpdates);
    EXPECT_EQ(physics.statistics().totalSteps, before.totalSteps);
    EXPECT_TRUE(physics.playing());
    EXPECT_NO_THROW(physics.update(graph, PhysicsScene::kFixedStepSeconds));
  }
}

TEST(PhysicsSceneTests, TransformPreflightAcceptsSupportedLocalAndAncestorEdits) {
  SceneGraph graph;
  const auto parent = graph.createNode(glm::scale(glm::mat4{1}, {2, 1, 1}), 0u);
  const auto node = graph.createNode(translation(0, 5, 0), 0u);
  ASSERT_TRUE(graph.setParent(node, parent));
  PhysicsScene physics{noGravity()};
  const auto body = physics.attachBox(graph, node, glm::vec3{-.5f}, glm::vec3{.5f});
  const auto revision = graph.revision();
  const auto proposed = glm::rotate(translation(3, 6, 7), .4f, {1, 0, 0});
  EXPECT_NO_THROW(physics.validateTransform(graph, node, proposed));
  const auto parentEdit = glm::rotate(translation(10, 0, 0), .3f, {0, 0, 1}) *
      glm::scale(glm::mat4{1}, {2, 1, 1});
  EXPECT_NO_THROW(physics.validateTransform(graph, parent, parentEdit));
  EXPECT_EQ(graph.revision(), revision);
  EXPECT_EQ(physics.bodyId(node), body);
  graph.setLocalTransform(node, proposed);
  graph.setLocalTransform(parent, parentEdit);
  EXPECT_NO_THROW(physics.update(graph, 0));
  const auto expected = parentEdit * proposed;
  expectPositionNear(physics.bodyPose(node)->position, glm::dvec3{expected[3]}, 1e-4);
  EXPECT_NO_THROW(physics.reset(graph));
}

TEST(PhysicsSceneTests, ExactParentPreflightUsesStoredLocalInsteadOfPreservingWorld) {
  for (const auto motion : {PhysicsMotionType::Static, PhysicsMotionType::Dynamic}) {
    SCOPED_TRACE(static_cast<int>(motion));
    SceneGraph graph;
    const auto parent = graph.createNode(glm::scale(glm::mat4{1}, {2, 1, 1}), 0u);
    const auto node = graph.createNode(translation(0, 5, 0), 0u);
    PhysicsScene physics{noGravity()};
    const auto body = physics.attachBox(graph, node, glm::vec3{-.5f}, glm::vec3{.5f},
        {.motionType = motion});
    physics.setPlaying(true);
    const auto original = graph.getNode(node)->localTransform;
    const auto proposed = glm::rotate(original, .7f, {0, 0, 1});
    const auto revision = graph.revision();
    const auto hierarchy = graph.hierarchyRevision();
    const auto pose = *physics.bodyPose(node);
    const auto statistics = physics.statistics();
    // A rigid local rotation is supported at the current root. The existing
    // world-preserving reparent is also supported, but this exact local under
    // the new nonuniform parent would shear the body.
    EXPECT_NO_THROW(physics.validateTransform(graph, node, proposed));
    EXPECT_NO_THROW(physics.validateReparent(graph, node, parent));
    EXPECT_THROW(physics.validateTransform(graph, node, proposed, parent),
                 std::invalid_argument);
    EXPECT_EQ(graph.revision(), revision);
    EXPECT_EQ(graph.hierarchyRevision(), hierarchy);
    EXPECT_EQ(graph.getNode(node)->parent, SceneGraph::kInvalidNode);
    EXPECT_EQ(graph.getNode(node)->localTransform, original);
    EXPECT_EQ(physics.bodyId(node), body);
    expectPositionNear(physics.bodyPose(node)->position, pose.position);
    EXPECT_EQ(physics.bodyPose(node)->rotation, pose.rotation);
    EXPECT_EQ(physics.statistics().totalSteps, statistics.totalSteps);
    EXPECT_EQ(physics.statistics().authoredPoseUpdates, statistics.authoredPoseUpdates);
    EXPECT_TRUE(physics.playing());
  }
}

TEST(PhysicsSceneTests, ExactParentPreflightSeedsDescendantWorldsWithoutGraphMutation) {
  SceneGraph graph;
  const auto parent = graph.createNode(glm::scale(glm::mat4{1}, {2, 1, 1}), 0u);
  const auto group = graph.createNode(glm::mat4{1}, 0u);
  const auto child = graph.createNode(glm::rotate(translation(0, 5, 0), .7f, {0, 0, 1}), 0u);
  ASSERT_TRUE(graph.setParent(child, group));
  PhysicsScene physics{noGravity()};
  const auto body = physics.attachBox(graph, child, glm::vec3{-.5f}, glm::vec3{.5f});
  const auto revision = graph.revision();
  // Compensation at the group makes the descendant rigid under its proposed
  // parent. Retaining the group's old local instead would shear that child.
  const auto compensation = glm::scale(glm::mat4{1}, {.5f, 1, 1});
  EXPECT_NO_THROW(physics.validateTransform(graph, group, compensation, parent));
  EXPECT_THROW(physics.validateTransform(graph, group, glm::mat4{1}, parent),
               std::invalid_argument);
  EXPECT_EQ(graph.revision(), revision);
  EXPECT_EQ(graph.getNode(group)->parent, SceneGraph::kInvalidNode);
  EXPECT_EQ(graph.getNode(group)->localTransform, glm::mat4{1});
  EXPECT_EQ(physics.bodyId(child), body);
}

TEST(PhysicsSceneTests, ExactParentPreflightAlsoProtectsBoundAuthoredResetLocal) {
  SceneGraph graph;
  const auto parent = graph.createNode(glm::scale(glm::mat4{1}, {2, 1, 1}), 0u);
  const auto node = graph.createNode(glm::rotate(translation(0, 5, 0), .7f, {0, 0, 1}), 0u);
  PhysicsScene physics{noGravity()};
  (void)physics.attachBox(graph, node, glm::vec3{-.5f}, glm::vec3{.5f});
  // The exact live local would be supported under the parent, but Reset would
  // restore the attached rotated local and produce an unsupported shear.
  const auto proposed = translation(3, 4, 5);
  const auto revision = graph.revision();
  EXPECT_THROW(physics.validateTransform(graph, node, proposed, parent),
               std::invalid_argument);
  EXPECT_EQ(graph.revision(), revision);
  EXPECT_EQ(graph.getNode(node)->parent, SceneGraph::kInvalidNode);
}

TEST(PhysicsSceneTests, ExactParentPreflightSupportsRootAndRejectsUnsupportedParent) {
  SceneGraph graph;
  const auto parent = graph.createNode(glm::scale(glm::mat4{1}, {2, 1, 1}), 0u);
  auto shear = glm::mat4{1};
  shear[1][0] = .5f;
  const auto shearedParent = graph.createNode(shear, 0u);
  const auto node = graph.createNode(translation(0, 5, 0), 0u);
  const auto visual = graph.createNode(glm::mat4{1}, 0u);
  ASSERT_TRUE(graph.setParent(node, parent));
  PhysicsScene physics{noGravity()};
  (void)physics.attachBox(graph, node, glm::vec3{-.5f}, glm::vec3{.5f});
  const auto proposed = glm::rotate(translation(3, 4, 5), .7f, {0, 0, 1});
  EXPECT_THROW(physics.validateTransform(graph, node, proposed), std::invalid_argument);
  EXPECT_NO_THROW(physics.validateTransform(graph, node, proposed, SceneGraph::kInvalidNode));
  // Even a local inverse that cancels a sheared parent's world basis cannot
  // make that unsupported physics ancestry legal.
  EXPECT_THROW(physics.validateTransform(graph, node, glm::inverse(shear), shearedParent),
               std::invalid_argument);
  EXPECT_NO_THROW(physics.validateTransform(graph, visual, glm::mat4{1}, shearedParent));
  EXPECT_EQ(graph.getNode(node)->parent, parent);
}

TEST(PhysicsSceneTests, ExactParentPreflightRejectsCyclesMissingParentsAndMixedBodies) {
  SceneGraph graph;
  const auto support = graph.createNode(glm::mat4{1}, 0u);
  const auto group = graph.createNode(glm::mat4{1}, 0u);
  const auto node = graph.createNode(translation(0, 5, 0), 0u);
  ASSERT_TRUE(graph.setParent(node, group));
  PhysicsScene physics{noGravity()};
  (void)physics.attachBox(graph, support, glm::vec3{-.5f}, glm::vec3{.5f},
      {.motionType = PhysicsMotionType::Static});
  (void)physics.attachBox(graph, node, glm::vec3{-.5f}, glm::vec3{.5f});
  physics.setPlaying(true);
  const auto revision = graph.revision();
  EXPECT_THROW(physics.validateTransform(graph, group, glm::mat4{1}, node),
               std::invalid_argument);
  EXPECT_THROW(physics.validateTransform(graph, group, glm::mat4{1}, 1000u),
               std::invalid_argument);
  EXPECT_THROW(physics.validateTransform(graph, group, glm::mat4{1}, support),
               std::invalid_argument);
  EXPECT_EQ(graph.revision(), revision);
  EXPECT_EQ(graph.getNode(group)->parent, SceneGraph::kInvalidNode);
  EXPECT_TRUE(physics.playing());
}

TEST(PhysicsSceneTests, TransformPreflightRestrictsOnlyMatchingPhysicsSubtrees) {
  SceneGraph graph;
  const auto bound = graph.createNode(glm::mat4{1}, 0u);
  const auto visual = graph.createNode(glm::mat4{1}, 0u);
  PhysicsScene physics{noGravity()};
  const auto body = physics.attachBox(graph, bound, glm::vec3{-.5f}, glm::vec3{.5f});
  auto shear = glm::mat4{1};
  shear[1][0] = .5f;
  const auto revision = graph.revision();
  EXPECT_NO_THROW(physics.validateTransform(graph, visual, shear));
  EXPECT_THROW(physics.validateTransform(graph, bound, shear), std::invalid_argument);
  EXPECT_THROW(physics.validateTransform(graph, bound, glm::scale(glm::mat4{1}, {0, 1, 1})),
      std::invalid_argument);
  auto nonfinite = glm::mat4{1};
  nonfinite[3][0] = std::numeric_limits<float>::infinity();
  EXPECT_THROW(physics.validateTransform(graph, bound, nonfinite), std::invalid_argument);
  SceneGraph replacement;
  const auto reused = replacement.createNode(glm::mat4{1}, 0u);
  ASSERT_EQ(reused, bound);
  EXPECT_NO_THROW(physics.validateTransform(replacement, reused, shear));
  EXPECT_EQ(graph.revision(), revision);
  EXPECT_EQ(physics.bodyId(bound), body);
  EXPECT_EQ(physics.graphIdentity(), graph.identity());
}

TEST(PhysicsSceneTests, TransformPreflightProtectsResetAndCanonicalJointFrames) {
  SceneGraph graph;
  const auto parent = graph.createNode(glm::mat4{1}, 0u);
  const auto node = graph.createNode(glm::rotate(translation(0, 5, 0), .7f, {0, 0, 1}), 0u);
  ASSERT_TRUE(graph.setParent(node, parent));
  const auto anchored = graph.createNode(glm::mat4{1}, 0u);
  PhysicsScene physics{noGravity()};
  const auto body = physics.attachBox(graph, node, glm::vec3{-.5f}, glm::vec3{.5f});
  const auto anchoredBody = physics.attachBox(graph, anchored, glm::vec3{-.5f}, glm::vec3{.5f});
  container::physics::PhysicsJointSettings settings;
  settings.type = container::physics::PhysicsJointType::Fixed;
  settings.anchor = {1e31, 0, 0};
  const auto joint = physics.createJoint(graph, std::nullopt, anchored, settings);
  graph.setLocalTransform(node, translation(0, 5, 0));
  physics.update(graph, 0);
  const auto revision = graph.revision();
  // The current child would remain rigid; its rotated attached local would
  // instead become sheared when Reset resolves the proposed unbound parent.
  EXPECT_THROW(physics.validateTransform(graph, parent, glm::scale(glm::mat4{1}, {2, 1, 1})),
      std::invalid_argument);
  // This scale has finite body pose/extents but overflows the scaled canonical
  // local anchor. Reject before a collider rebuild can retire the old body.
  EXPECT_THROW(physics.validateTransform(graph, anchored, glm::scale(glm::mat4{1}, glm::vec3{1e8f})),
      std::invalid_argument);
  EXPECT_EQ(graph.revision(), revision);
  EXPECT_EQ(physics.bodyId(node), body);
  EXPECT_EQ(physics.bodyId(anchored), anchoredBody);
  ASSERT_TRUE(physics.jointState(joint));
  EXPECT_EQ(physics.jointState(joint)->state.frameB.anchor, settings.anchor);
  EXPECT_NO_THROW(physics.reset(graph));
}

TEST(PhysicsSceneTests, ReparentPreflightRejectsMixedSubtreesWithoutChangingLiveState) {
  for (const auto supportMotion : {PhysicsMotionType::Static, PhysicsMotionType::Kinematic,
                                   PhysicsMotionType::Dynamic}) {
    SceneGraph graph;
    const auto group = graph.createNode(glm::mat4{1}, 0u);
    const auto child = graph.createNode(translation(0, 5, 0), 0u);
    ASSERT_TRUE(graph.setParent(child, group));
    const auto support = graph.createNode(translation(10, 5, 0), 0u);
    PhysicsScene physics{noGravity()};
    const auto childMotion = supportMotion == PhysicsMotionType::Dynamic
        ? PhysicsMotionType::Static : PhysicsMotionType::Dynamic;
    const auto childId = physics.attachBox(graph, child, glm::vec3{-.5f}, glm::vec3{.5f},
        {.motionType = childMotion});
    const auto supportId = physics.attachBox(graph, support, glm::vec3{-.5f}, glm::vec3{.5f},
        {.motionType = supportMotion});
    const auto dynamic = childMotion == PhysicsMotionType::Dynamic ? child : support;
    const auto joint = physics.createJoint(graph, std::nullopt, dynamic, {});
    physics.setPlaying(true);
    const auto revision = graph.revision();
    const auto local = graph.getNode(child)->localTransform;
    EXPECT_THROW(physics.validateReparent(graph, group, support), std::invalid_argument);
    EXPECT_EQ(graph.revision(), revision);
    EXPECT_EQ(graph.getNode(group)->parent, SceneGraph::kInvalidNode);
    EXPECT_EQ(graph.getNode(child)->localTransform, local);
    EXPECT_EQ(physics.bodyId(child), childId);
    EXPECT_EQ(physics.bodyId(support), supportId);
    ASSERT_TRUE(physics.jointState(joint));
    EXPECT_TRUE(physics.playing());
    EXPECT_NO_THROW(physics.update(graph, PhysicsScene::kFixedStepSeconds));
    EXPECT_NO_THROW(physics.reset(graph));
  }
}

TEST(PhysicsSceneTests, ReparentPreflightAllowsCompatibleWorldPreservingEdits) {
  SceneGraph graph;
  const auto parent = graph.createNode(translation(10, 0, 0), 0u);
  const auto node = graph.createNode(translation(0, 5, 0), 0u);
  PhysicsScene physics{noGravity()};
  const auto body = physics.attachBox(graph, node, glm::vec3{-.5f}, glm::vec3{.5f});
  const auto joint = physics.createJoint(graph, std::nullopt, node, {});
  const auto revision = graph.revision();
  EXPECT_NO_THROW(physics.validateReparent(graph, node, parent));
  EXPECT_EQ(graph.revision(), revision);
  ASSERT_TRUE(graph.setParentPreserveWorldTransform(node, parent));
  EXPECT_NO_THROW(physics.update(graph, 0));
  EXPECT_EQ(physics.bodyId(node), body);
  expectPositionNear(physics.bodyPose(node)->position, {0, 5, 0});
  EXPECT_TRUE(physics.jointState(joint));
  EXPECT_NO_THROW(physics.reset(graph));
}

TEST(PhysicsSceneTests, ReparentPreflightRejectsShearedPhysicsParentsOnly) {
  SceneGraph graph;
  const auto scaled = graph.createNode(glm::scale(glm::mat4{1}, glm::vec3{2, 1, 1}), 0u);
  const auto sheared = graph.createNode(glm::rotate(glm::mat4{1}, .7f, glm::vec3{0, 0, 1}), 0u);
  ASSERT_TRUE(graph.setParent(sheared, scaled));
  const auto bodyNode = graph.createNode(translation(0, 5, 0), 0u);
  const auto visualOnly = graph.createNode(glm::mat4{1}, 0u);
  PhysicsScene physics{noGravity()};
  (void)physics.attachBox(graph, bodyNode, glm::vec3{-.5f}, glm::vec3{.5f});
  const auto revision = graph.revision();
  EXPECT_THROW(physics.validateReparent(graph, bodyNode, sheared), std::invalid_argument);
  EXPECT_EQ(graph.revision(), revision);
  EXPECT_NO_THROW(physics.validateReparent(graph, visualOnly, sheared));
}

TEST(PhysicsSceneTests, ReparentPreflightProtectsAuthoredResetTransforms) {
  SceneGraph graph;
  const auto parent = graph.createNode(glm::scale(glm::mat4{1}, glm::vec3{2, 1, 1}), 0u);
  const auto authored = glm::rotate(translation(0, 5, 0), .7f, glm::vec3{0, 0, 1});
  const auto boundRoot = graph.createNode(authored, 0u);
  const auto group = graph.createNode(authored, 0u);
  const auto child = graph.createNode(glm::mat4{1}, 0u);
  ASSERT_TRUE(graph.setParent(child, group));
  PhysicsScene physics{noGravity()};
  const auto body = physics.attachBox(graph, boundRoot, glm::vec3{-.5f}, glm::vec3{.5f});
  (void)physics.attachBox(graph, child, glm::vec3{-.5f}, glm::vec3{.5f});
  const auto revision = graph.revision();
  // The live world transform would remain rigid, but Reset would restore a
  // rotated attached local under the new nonuniform parent, introducing shear.
  EXPECT_THROW(physics.validateReparent(graph, boundRoot, parent), std::invalid_argument);
  EXPECT_EQ(graph.revision(), revision);
  EXPECT_EQ(physics.bodyId(boundRoot), body);
  // An unbound moved group retains its compensated local on Reset, so it is
  // valid even though that new local alone contains shear.
  EXPECT_NO_THROW(physics.validateReparent(graph, group, parent));
  ASSERT_TRUE(graph.setParentPreserveWorldTransform(group, parent));
  EXPECT_NO_THROW(physics.update(graph, 0));
  EXPECT_NO_THROW(physics.reset(graph));
  expectMatrixNear(graph.getNode(child)->worldTransform, authored);
}

TEST(PhysicsSceneTests, InvalidBoundsTransformsAndDuplicateBindingsAreRejected) {
  SceneGraph graph;
  PhysicsScene physics;
  EXPECT_THROW((void)physics.attachBox(graph, 0u, glm::vec3{-0.5f}, glm::vec3{0.5f}),
               std::invalid_argument);
  const auto node = graph.createNode(glm::mat4{1.0f}, 0u);
  EXPECT_THROW((void)physics.attachBox(graph, node, {1.0f, 0.0f, 0.0f},
                                       {-1.0f, 1.0f, 1.0f}), std::invalid_argument);
  EXPECT_THROW((void)physics.attachBox(graph, node,
                  {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f},
                  glm::vec3{1.0f}), std::invalid_argument);
  graph.setLocalTransform(node, glm::scale(glm::mat4{1.0f}, {1.0f, 0.0f, 1.0f}));
  EXPECT_THROW((void)physics.attachBox(graph, node, glm::vec3{-0.5f}, glm::vec3{0.5f}),
               std::invalid_argument);
  glm::mat4 shear{1.0f};
  shear[1][0] = 0.2f;
  graph.setLocalTransform(node, shear);
  EXPECT_THROW((void)physics.attachBox(graph, node, glm::vec3{-0.5f}, glm::vec3{0.5f}),
               std::invalid_argument);
  glm::mat4 nonFinite{1.0f};
  nonFinite[3].x = std::numeric_limits<float>::infinity();
  graph.setLocalTransform(node, nonFinite);
  EXPECT_THROW((void)physics.attachBox(graph, node, glm::vec3{-0.5f}, glm::vec3{0.5f}),
               std::invalid_argument);
  EXPECT_EQ(physics.bodyCount(), 0u);
  graph.setLocalTransform(node, glm::mat4{1.0f});
  // Flat/point bounds produce a minimum box thickness rather than zero axes.
  (void)physics.attachBox(graph, node, glm::vec3{0.0f}, glm::vec3{0.0f});
  EXPECT_THROW((void)physics.attachBox(graph, node, glm::vec3{-0.5f}, glm::vec3{0.5f}),
               std::invalid_argument);
  EXPECT_EQ(physics.bodyCount(), 1u);
}

TEST(PhysicsSceneTests, ReparentingMixedBindingsIsDetectedBeforeSimulation) {
  SceneGraph graph;
  const auto staticNode = graph.createNode(translation(0.0f, 0.0f, 0.0f), 0u);
  const auto dynamicNode = graph.createNode(translation(0.0f, 5.0f, 0.0f), 0u);
  PhysicsScene physics;
  PhysicsBodySettings settings;
  settings.motionType = PhysicsMotionType::Static;
  (void)physics.attachBox(graph, staticNode, glm::vec3{-0.5f}, glm::vec3{0.5f}, settings);
  (void)physics.attachBox(graph, dynamicNode, glm::vec3{-0.5f}, glm::vec3{0.5f});
  ASSERT_TRUE(graph.setParent(dynamicNode, staticNode));
  EXPECT_THROW(physics.singleStep(graph), std::invalid_argument);
  EXPECT_EQ(physics.statistics().totalSteps, 0u);
}

TEST(PhysicsSceneTests, ReplacementChangesMotionAtLivePoseAndPreservesAuthoredReset) {
  SceneGraph graph;
  const auto node = graph.createNode(translation(0.0f, 5.0f, 0.0f), 0u);
  const auto authored = graph.getNode(node)->localTransform;
  PhysicsScene physics{noGravity()};
  PhysicsBodySettings settings;
  settings.linearVelocity = {2.0f, 0.0f, 0.0f};
  const auto original = physics.attachBox(graph, node, glm::vec3{-0.5f},
                                          glm::vec3{0.5f}, settings);
  physics.singleStep(graph);
  const auto liveVisual = graph.getNode(node)->localTransform;
  settings.motionType = PhysicsMotionType::Static;
  settings.mass = 12.0f;
  settings.friction = 0.8f;
  settings.restitution = 0.2f;
  const auto replacement = physics.replaceBox(graph, node, {0.0f, -0.5f, -0.5f},
                                               {2.0f, 0.5f, 0.5f}, settings);
  EXPECT_NE(replacement, original);
  EXPECT_EQ(physics.bodyCount(), 1u);
  EXPECT_EQ(physics.motionType(node), PhysicsMotionType::Static);
  expectMatrixNear(graph.getNode(node)->localTransform, liveVisual);
  expectPositionNear(physics.bodyPose(node)->position,
                       glm::dvec3{liveVisual * glm::vec4{1.0f, 0.0f, 0.0f, 1.0f}});
  physics.update(graph, PhysicsScene::kFixedStepSeconds);
  expectMatrixNear(graph.getNode(node)->localTransform, liveVisual);
  physics.reset(graph);
  expectMatrixNear(graph.getNode(node)->localTransform, authored);
  expectPositionNear(physics.bodyPose(node)->position, {1.0, 5.0, 0.0});
  EXPECT_EQ(physics.motionType(node), PhysicsMotionType::Static);
}

TEST(PhysicsSceneTests, InvalidReplacementRetainsBodyPoseMotionAndVelocity) {
  SceneGraph graph;
  const auto node = graph.createNode(translation(0.0f, 5.0f, 0.0f), 0u);
  PhysicsScene physics{noGravity()};
  PhysicsBodySettings originalSettings;
  originalSettings.linearVelocity = {2.0f, 0.0f, 0.0f};
  const auto original = physics.attachBox(graph, node, glm::vec3{-0.5f},
                                          glm::vec3{0.5f}, originalSettings);
  const auto originalPose = *physics.bodyPose(node);
  PhysicsBodySettings invalid;
  invalid.motionType = PhysicsMotionType::Static;
  invalid.mass = 0.0f;
  EXPECT_THROW((void)physics.replaceBox(graph, node, glm::vec3{-1.0f},
                                         glm::vec3{1.0f}, invalid), std::invalid_argument);
  EXPECT_THROW((void)physics.replaceBox(graph, node, glm::vec3{1.0f},
                                         glm::vec3{-1.0f}), std::invalid_argument);
  EXPECT_EQ(physics.bodyId(node), original);
  EXPECT_EQ(physics.bodyCount(), 1u);
  EXPECT_EQ(physics.motionType(node), PhysicsMotionType::Dynamic);
  expectPositionNear(physics.bodyPose(node)->position, originalPose.position);
  physics.singleStep(graph);
  EXPECT_GT(graph.getNode(node)->worldTransform[3].x, 0.03f);
  EXPECT_NEAR(graph.getNode(node)->worldTransform[3].y, 5.0f, 1e-5f);
}

TEST(PhysicsSceneTests, ReplacementCapacityFailureRetainsOriginalBinding) {
  SceneGraph graph;
  const auto node = graph.createNode(translation(0.0f, 5.0f, 0.0f), 0u);
  auto worldSettings = noGravity();
  worldSettings.maxBodies = 1u;
  PhysicsScene physics{worldSettings};
  const auto original = physics.attachBox(graph, node, glm::vec3{-0.5f}, glm::vec3{0.5f});
  PhysicsBodySettings settings;
  settings.motionType = PhysicsMotionType::Static;
  EXPECT_THROW((void)physics.replaceBox(graph, node, glm::vec3{-1.0f},
                                         glm::vec3{1.0f}, settings), std::runtime_error);
  EXPECT_EQ(physics.bodyId(node), original);
  EXPECT_EQ(physics.bodyCount(), 1u);
  EXPECT_EQ(physics.motionType(node), PhysicsMotionType::Dynamic);
  expectPositionNear(physics.bodyPose(node)->position, {0.0, 5.0, 0.0});
  physics.singleStep(graph);
  EXPECT_TRUE(physics.bodyPose(node));
}

TEST(PhysicsSceneTests, ReplacementRejectsMixedAncestorsBeforeRetiringBodies) {
  SceneGraph graph;
  const auto parent = graph.createNode(translation(0.0f, 10.0f, 0.0f), 0u);
  const auto child = graph.createNode(translation(0.0f, 5.0f, 0.0f), 0u);
  ASSERT_TRUE(graph.setParent(child, parent));
  PhysicsScene physics;
  const auto parentBody = physics.attachBox(graph, parent, glm::vec3{-0.5f}, glm::vec3{0.5f});
  const auto childBody = physics.attachBox(graph, child, glm::vec3{-0.5f}, glm::vec3{0.5f});
  PhysicsBodySettings settings;
  settings.motionType = PhysicsMotionType::Static;
  EXPECT_THROW((void)physics.replaceBox(graph, parent, glm::vec3{-0.5f},
                                         glm::vec3{0.5f}, settings), std::invalid_argument);
  EXPECT_EQ(physics.bodyId(parent), parentBody);
  EXPECT_EQ(physics.bodyId(child), childBody);
  EXPECT_EQ(physics.bodyCount(), 2u);
  EXPECT_EQ(physics.motionType(parent), PhysicsMotionType::Dynamic);
  EXPECT_EQ(physics.motionType(child), PhysicsMotionType::Dynamic);
  EXPECT_NO_THROW(physics.singleStep(graph));
}

TEST(PhysicsSceneTests, PausedDynamicMoveAndRotationResumeWithoutLosingVelocity) {
  SceneGraph editedGraph, controlGraph;
  const auto editedNode = editedGraph.createNode(translation(0.0f, 5.0f, 0.0f), 0u);
  const auto controlNode = controlGraph.createNode(translation(0.0f, 5.0f, 0.0f), 0u);
  PhysicsScene edited{noGravity()}, control{noGravity()};
  PhysicsBodySettings settings;
  settings.linearVelocity = {2.0f, 0.0f, 0.0f};
  settings.angularVelocity = {0.0f, 0.5f, 0.0f};
  (void)edited.attachBox(editedGraph, editedNode, glm::vec3{-0.5f}, glm::vec3{0.5f}, settings);
  (void)control.attachBox(controlGraph, controlNode, glm::vec3{-0.5f}, glm::vec3{0.5f}, settings);
  edited.singleStep(editedGraph);
  control.singleStep(controlGraph);
  edited.update(editedGraph, 0.0);
  const auto rotation = glm::angleAxis(0.7f, glm::vec3{0.0f, 0.0f, 1.0f});
  const auto editedTransform = translation(7.0f, 8.0f, 9.0f) * glm::mat4_cast(rotation);
  editedGraph.setLocalTransform(editedNode, editedTransform);
  edited.update(editedGraph, 0.0);
  expectMatrixNear(editedGraph.getNode(editedNode)->worldTransform, editedTransform);
  expectPositionNear(edited.bodyPose(editedNode)->position, {7.0, 8.0, 9.0});
  EXPECT_NEAR(std::abs(glm::dot(edited.bodyPose(editedNode)->rotation, rotation)), 1.0f, 1e-6f);
  EXPECT_EQ(edited.statistics().authoredPoseUpdates, 1u);
  const auto steps = edited.statistics().totalSteps;
  editedGraph.setName(editedNode, "Name edit must not teleport a body");
  edited.update(editedGraph, 10.0);
  EXPECT_EQ(edited.statistics().authoredPoseUpdates, 1u);
  EXPECT_EQ(edited.statistics().totalSteps, steps);

  const auto controlBefore = *control.bodyPose(controlNode);
  control.singleStep(controlGraph);
  const auto controlAfter = *control.bodyPose(controlNode);
  const auto displacement = controlAfter.position - controlBefore.position;
  const auto rotationDelta = controlAfter.rotation * glm::inverse(controlBefore.rotation);
  edited.setPlaying(true);
  edited.update(editedGraph, PhysicsScene::kFixedStepSeconds);
  expectPositionNear(edited.bodyPose(editedNode)->position,
                       glm::dvec3{7.0, 8.0, 9.0} + displacement);
  EXPECT_NEAR(std::abs(glm::dot(edited.bodyPose(editedNode)->rotation,
                               rotationDelta * rotation)), 1.0f, 1e-5f);
  EXPECT_GT(editedGraph.getNode(editedNode)->worldTransform[3].x, 7.03f);
  EXPECT_NEAR(editedGraph.getNode(editedNode)->worldTransform[3].y, 8.0f, 1e-5f);
  edited.setPlaying(false);
  edited.update(editedGraph, 0.0);
  EXPECT_EQ(edited.statistics().authoredPoseUpdates, 1u);
}

TEST(PhysicsSceneTests, SmallAuthoredRotationsSynchronizeStaticAndPausedDynamicColliders) {
  for (const auto motion : {PhysicsMotionType::Static, PhysicsMotionType::Dynamic}) {
    for (const float degrees : {.01f, .02f}) {
      SCOPED_TRACE(int(motion));
      SCOPED_TRACE(degrees);
      SceneGraph graph;
      const auto node = graph.createNode(glm::mat4{1}, 0);
      PhysicsScene physics{noGravity()};
      (void)physics.attachBox(graph, node, {-50, -.5f, -5}, {50, .5f, 5},
                             {.motionType = motion});
      physics.update(graph, 0);
      const auto rotation = glm::angleAxis(glm::radians(degrees), glm::vec3{0, 0, 1});
      const auto target = glm::mat4_cast(rotation);
      graph.setLocalTransform(node, target);
      physics.update(graph, 0);
      // A float dot product rounds to one for these distinct orientations.
      // Check the actual endpoint of a 100 m collider instead of another dot.
      const auto endpoint = glm::dquat{rotation} * glm::dvec3{50, 0, 0};
      expectPositionNear(glm::dquat{physics.bodyPose(node)->rotation} * glm::dvec3{50, 0, 0},
                         endpoint, 1e-6);
      expectMatrixNear(graph.getNode(node)->worldTransform, target, 1e-7f);
      EXPECT_EQ(physics.statistics().authoredPoseUpdates, 1u);
      const auto updates = physics.statistics().authoredPoseUpdates;
      // Quaternion signs describe the same matrix. Neither that edit nor an
      // unrelated label change should request another physics teleport.
      graph.setLocalTransform(node, glm::mat4_cast(-rotation));
      graph.setName(node, "Equivalent quaternion and unchanged pose");
      physics.update(graph, 0);
      EXPECT_EQ(physics.statistics().authoredPoseUpdates, updates);
      physics.setPlaying(true);
      physics.update(graph, PhysicsScene::kFixedStepSeconds);
      expectPositionNear(glm::dquat{physics.bodyPose(node)->rotation} * glm::dvec3{50, 0, 0},
                         endpoint, 1e-6);
      expectPositionNear(glm::dvec3{graph.getNode(node)->worldTransform[0]} * 50.0,
                         endpoint, 1e-5);
    }
  }
}

TEST(PhysicsSceneTests, SmallPendingDynamicRotationsSurviveDirectPlayAndSingleStep) {
  for (const bool singleStep : {false, true}) {
    for (const float degrees : {.01f, .02f}) {
      SCOPED_TRACE(singleStep);
      SCOPED_TRACE(degrees);
      SceneGraph graph;
      const auto node = graph.createNode(glm::mat4{1}, 0);
      PhysicsScene physics{noGravity()};
      (void)physics.attachBox(graph, node, {-50, -.5f, -5}, {50, .5f, 5},
                             {.linearVelocity = {1, 0, 0}});
      const auto rotation = glm::angleAxis(glm::radians(degrees), glm::vec3{0, 0, 1});
      graph.setLocalTransform(node, glm::mat4_cast(rotation));
      if (singleStep) {
        physics.singleStep(graph);
      } else {
        physics.setPlaying(true);
        physics.update(graph, PhysicsScene::kFixedStepSeconds);
      }
      const auto endpoint = glm::dquat{rotation} * glm::dvec3{50, 0, 0};
      expectPositionNear(glm::dquat{physics.bodyPose(node)->rotation} * glm::dvec3{50, 0, 0},
                         endpoint, 1e-6);
      expectPositionNear(glm::dvec3{graph.getNode(node)->worldTransform[0]} * 50.0,
                         endpoint, 1e-5);
      EXPECT_GT(physics.bodyPose(node)->position.x, .01);
      EXPECT_EQ(physics.statistics().totalSteps, 1u);
      EXPECT_EQ(physics.playing(), !singleStep);
    }
  }
}

TEST(PhysicsSceneTests, PausedMoveWakesSleepingDynamicBeforePlaybackResumes) {
  SceneGraph graph;
  const auto ground = graph.createNode(translation(0.0f, -0.5f, 0.0f), 0u);
  const auto box = graph.createNode(translation(0.0f, 3.0f, 0.0f), 0u);
  PhysicsScene physics;
  PhysicsBodySettings settings;
  settings.motionType = PhysicsMotionType::Static;
  (void)physics.attachBox(graph, ground, {-10.0f, -0.5f, -10.0f},
                          {10.0f, 0.5f, 10.0f}, settings);
  (void)physics.attachBox(graph, box, glm::vec3{-0.5f}, glm::vec3{0.5f});
  physics.setPlaying(true);
  for (int frame = 0; frame < 300; ++frame)
    physics.update(graph, PhysicsScene::kFixedStepSeconds);
  ASSERT_EQ(physics.statistics().activeBodyCount, 0u);
  ASSERT_NEAR(physics.bodyPose(box)->position.y, 0.5, 0.04);

  physics.setPlaying(false);
  graph.setName(box, "A label edit must retain sleep");
  physics.update(graph, 0.0);
  EXPECT_EQ(physics.statistics().activeBodyCount, 0u);
  EXPECT_EQ(physics.statistics().authoredPoseUpdates, 0u);
  const auto steps = physics.statistics().totalSteps;
  graph.setLocalTransform(box, translation(0.0f, 5.0f, 0.0f));
  physics.update(graph, 0.0);
  expectPositionNear(physics.bodyPose(box)->position, {0.0, 5.0, 0.0});
  EXPECT_EQ(physics.statistics().activeBodyCount, 1u);
  EXPECT_EQ(physics.statistics().totalSteps, steps);

  physics.setPlaying(true);
  for (int frame = 0; frame < 30; ++frame)
    physics.update(graph, PhysicsScene::kFixedStepSeconds);
  EXPECT_LT(physics.bodyPose(box)->position.y, 4.0);
  EXPECT_EQ(physics.statistics().totalSteps, steps + 30u);
}

TEST(PhysicsSceneTests, MovingSleepingDynamicSupportReleasesItsRiderOnPlayAndSingleStep) {
  for (const bool singleStep : {false, true}) {
    SCOPED_TRACE(singleStep);
    SceneGraph graph;
    const auto floor = graph.createNode(translation(0, -.5f, 0), 0);
    const auto lower = graph.createNode(translation(0, .5f, 0), 0);
    const auto upper = graph.createNode(translation(0, 1.5f, 0), 0);
    const auto remoteLower = graph.createNode(translation(30, .5f, 0), 0);
    const auto remoteUpper = graph.createNode(translation(30, 1.5f, 0), 0);
    PhysicsScene physics{{.maxBodies = 32, .workerThreads = 1}};
    (void)physics.attachBox(graph, floor, {-50, -.5f, -5}, {50, .5f, 5},
                           {.motionType = PhysicsMotionType::Static});
    for (const auto node : {lower, upper, remoteLower, remoteUpper})
      (void)physics.attachBox(graph, node, glm::vec3{-.5f}, glm::vec3{.5f});
    physics.setPlaying(true);
    for (int tick = 0; tick < 600; ++tick)
      physics.update(graph, PhysicsScene::kFixedStepSeconds);
    ASSERT_EQ(physics.statistics().activeBodyCount, 0);
    const auto upperBefore = physics.bodyPose(upper)->position;
    const auto remoteBefore = physics.bodyPose(remoteUpper)->position;
    ASSERT_NEAR(upperBefore.y, 1.5, .03);
    physics.setPlaying(false);
    graph.setName(lower, "An unchanged paused stack must stay asleep");
    physics.update(graph, 0);
    EXPECT_EQ(physics.statistics().activeBodyCount, 0);
    const auto steps = physics.statistics().totalSteps;
    auto moved = graph.getNode(lower)->localTransform;
    moved[3].x += 10;
    graph.setLocalTransform(lower, moved);
    if (!singleStep) {
      physics.update(graph, 0);
      EXPECT_EQ(physics.statistics().totalSteps, steps);
      EXPECT_EQ(physics.statistics().activeBodyCount, 2);
      physics.setPlaying(true);
    }
    for (int tick = 0; tick < 30; ++tick) {
      if (singleStep) physics.singleStep(graph);
      else physics.update(graph, PhysicsScene::kFixedStepSeconds);
      if (tick == 0)
        EXPECT_LT(physics.bodyPose(upper)->position.y, upperBefore.y - .001);
    }
    EXPECT_LT(physics.bodyPose(upper)->position.y, upperBefore.y - .5);
    expectPositionNear(physics.bodyPose(remoteUpper)->position, remoteBefore);
    EXPECT_NEAR(physics.bodyPose(lower)->position.x, 10, .001);
    EXPECT_NEAR(graph.getNode(upper)->worldTransform[3].y,
                physics.bodyPose(upper)->position.y, 1e-5);
    EXPECT_EQ(physics.statistics().totalSteps, steps + 30);
    EXPECT_EQ(physics.playing(), !singleStep);
  }
}

TEST(PhysicsSceneTests, PausedKinematicSupportMoveWakesItsSleepingDynamic) {
  SceneGraph graph;
  const auto ground = graph.createNode(translation(0.0f, -0.5f, 0.0f), 0u);
  const auto box = graph.createNode(translation(0.0f, 3.0f, 0.0f), 0u);
  PhysicsScene physics;
  PhysicsBodySettings settings;
  settings.motionType = PhysicsMotionType::Kinematic;
  (void)physics.attachBox(graph, ground, {-2.0f, -0.5f, -2.0f},
                          {2.0f, 0.5f, 2.0f}, settings);
  (void)physics.attachBox(graph, box, glm::vec3{-0.5f}, glm::vec3{0.5f});
  physics.setPlaying(true);
  for (int frame = 0; frame < 300; ++frame)
    physics.update(graph, PhysicsScene::kFixedStepSeconds);
  ASSERT_EQ(physics.statistics().activeBodyCount, 0u);
  ASSERT_NEAR(physics.bodyPose(box)->position.y, 0.5, 0.04);

  physics.setPlaying(false);
  graph.setName(ground, "An unchanged support must retain sleep");
  physics.update(graph, 0.0);
  EXPECT_EQ(physics.statistics().activeBodyCount, 0u);
  const auto steps = physics.statistics().totalSteps;
  graph.setLocalTransform(ground, translation(20.0f, -0.5f, 0.0f));
  physics.update(graph, 0.0);
  expectPositionNear(physics.bodyPose(ground)->position, {20.0, -0.5, 0.0});
  EXPECT_EQ(physics.statistics().totalSteps, steps);

  physics.setPlaying(true);
  for (int frame = 0; frame < 30; ++frame)
    physics.update(graph, PhysicsScene::kFixedStepSeconds);
  EXPECT_LT(physics.bodyPose(box)->position.y, 0.0);
}

TEST(PhysicsSceneTests, ResetUsesAuthoredLocalsUnderEditedUnboundParent) {
  SceneGraph graph;
  const auto parent = graph.createNode(translation(1.0f, 0.0f, 0.0f), 0u);
  const auto authoredLocal = translation(0.0f, 5.0f, 0.0f);
  const auto node = graph.createNode(authoredLocal, 0u);
  ASSERT_TRUE(graph.setParent(node, parent));
  PhysicsScene physics{noGravity()};
  PhysicsBodySettings settings;
  settings.linearVelocity = {2.0f, 0.0f, 0.0f};
  (void)physics.attachBox(graph, node, {1.0f, -0.5f, -0.5f},
                           {3.0f, 0.5f, 0.5f}, settings);
  physics.singleStep(graph);
  const auto editedParent = translation(10.0f, 2.0f, 3.0f) *
      glm::scale(glm::mat4{1.0f}, glm::vec3{2.0f});
  graph.setLocalTransform(parent, editedParent);
  physics.update(graph, 0.0);
  physics.reset(graph);
  expectMatrixNear(graph.getNode(node)->localTransform, authoredLocal);
  expectMatrixNear(graph.getNode(parent)->localTransform, editedParent);
  expectPositionNear(physics.bodyPose(node)->position, {14.0, 12.0, 3.0});
  const glm::dvec3 visualCenter{graph.getNode(node)->worldTransform *
                                glm::vec4{2.0f, 0.0f, 0.0f, 1.0f}};
  expectPositionNear(visualCenter, physics.bodyPose(node)->position);
  physics.setPlaying(true);
  physics.update(graph, PhysicsScene::kFixedStepSeconds);
  EXPECT_GT(graph.getNode(node)->worldTransform[3].x, 10.03f);
  EXPECT_NEAR(graph.getNode(node)->worldTransform[3].y, 12.0f, 1e-5f);
}

TEST(PhysicsSceneTests, InvalidResetParentDoesNotRetireExistingBodies) {
  SceneGraph graph;
  const auto parent = graph.createNode(glm::mat4{1.0f}, 0u);
  const auto node = graph.createNode(translation(0.0f, 5.0f, 0.0f), 0u);
  ASSERT_TRUE(graph.setParent(node, parent));
  PhysicsScene physics;
  const auto body = physics.attachBox(graph, node, glm::vec3{-0.5f}, glm::vec3{0.5f});
  physics.singleStep(graph);
  const auto pose = *physics.bodyPose(node);
  graph.setLocalTransform(parent, glm::scale(glm::mat4{1.0f}, glm::vec3{1.0f, 0.0f, 1.0f}));
  EXPECT_THROW(physics.reset(graph), std::invalid_argument);
  EXPECT_EQ(physics.bodyId(node), body);
  EXPECT_EQ(physics.bodyCount(), 1u);
  expectPositionNear(physics.bodyPose(node)->position, pose.position);
}

TEST(PhysicsSceneTests, RejectedResetColliderRetainsLiveBodiesJointsAndSimulation) {
  SceneGraph graph;
  const auto parent = graph.createNode(glm::mat4{1.0f}, 0u);
  const auto node = graph.createNode(translation(0, 5, 0), 0u);
  ASSERT_TRUE(graph.setParent(node, parent));
  const auto other = graph.createNode(translation(10, 5, 0), 0u);
  auto worldSettings = noGravity();
  worldSettings.maxBodies = 2;
  PhysicsScene physics{worldSettings};
  const auto firstId = physics.attachBox(graph, node, glm::vec3{-.5f}, glm::vec3{.5f},
      {.linearVelocity = {0, 1, 0}});
  const auto otherId = physics.attachBox(graph, other, glm::vec3{-.5f}, glm::vec3{.5f});
  container::physics::PhysicsJointSettings jointSettings;
  jointSettings.type = container::physics::PhysicsJointType::Slider;
  jointSettings.anchor = {0, 5, 0};
  jointSettings.axis = {0, 1, 0};
  const auto joint = physics.createJoint(graph, std::nullopt, node, jointSettings);
  physics.setPlaying(true);
  physics.update(graph, PhysicsScene::kFixedStepSeconds);
  const auto pose = *physics.bodyPose(node);
  const auto otherPose = *physics.bodyPose(other);
  const auto liveLocal = graph.getNode(node)->localTransform;
  const auto statistics = physics.statistics();
  // A finite, nonsingular parent can still make Jolt's shape inertia overflow.
  graph.setLocalTransform(parent, glm::scale(glm::mat4{1}, glm::vec3{1e10f}));
  EXPECT_THROW(physics.reset(graph), std::invalid_argument);
  EXPECT_EQ(physics.bodyId(node), firstId);
  EXPECT_EQ(physics.bodyId(other), otherId);
  EXPECT_EQ(physics.bodyCount(), 2u);
  EXPECT_EQ(physics.jointCount(), 1u);
  ASSERT_TRUE(physics.bodyPose(node));
  ASSERT_TRUE(physics.bodyPose(other));
  ASSERT_TRUE(physics.jointState(joint));
  expectPositionNear(physics.bodyPose(node)->position, pose.position);
  expectPositionNear(physics.bodyPose(other)->position, otherPose.position);
  expectMatrixNear(graph.getNode(node)->localTransform, liveLocal);
  EXPECT_TRUE(physics.playing());
  EXPECT_EQ(physics.statistics().totalSteps, statistics.totalSteps);
  graph.setLocalTransform(parent, glm::mat4{1});
  physics.update(graph, PhysicsScene::kFixedStepSeconds);
  EXPECT_GT(physics.bodyPose(node)->position.y, pose.position.y);
  // Reset must also succeed when every configured body slot is occupied.
  EXPECT_NO_THROW(physics.reset(graph));
  EXPECT_EQ(physics.bodyCount(), 2u);
  ASSERT_TRUE(physics.jointState(joint));
  expectPositionNear(physics.bodyPose(node)->position, {0, 5, 0});
}

TEST(PhysicsSceneTests, RejectedResetJointFrameRetainsLiveMechanism) {
  SceneGraph graph;
  const auto parent = graph.createNode(glm::mat4{1}, 0u);
  const auto node = graph.createNode(translation(2, 0, 0), 0u);
  ASSERT_TRUE(graph.setParent(node, parent));
  PhysicsScene physics{noGravity()};
  const auto body = physics.attachBox(graph, node, glm::vec3{-.5f}, glm::vec3{.5f});
  container::physics::PhysicsJointSettings settings;
  settings.type = container::physics::PhysicsJointType::Fixed;
  settings.anchor = {4e37, 0, 0};
  const auto joint = physics.createJoint(graph, std::nullopt, node, settings);
  const auto pose = *physics.bodyPose(node);
  const auto local = graph.getNode(node)->localTransform;
  // The resized box is valid, but its geometry-local anchor exceeds the
  // supported canonical joint-frame range. Failure occurs after body creation.
  graph.setLocalTransform(parent, glm::scale(glm::mat4{1}, glm::vec3{3, 1, 1}));
  EXPECT_THROW(physics.reset(graph), std::invalid_argument);
  EXPECT_EQ(physics.bodyId(node), body);
  ASSERT_TRUE(physics.bodyPose(node));
  ASSERT_TRUE(physics.jointState(joint));
  expectPositionNear(physics.bodyPose(node)->position, pose.position);
  expectMatrixNear(graph.getNode(node)->localTransform, local);
  graph.setLocalTransform(parent, glm::mat4{1});
  EXPECT_NO_THROW(physics.reset(graph));
  EXPECT_EQ(physics.bodyCount(), 1u);
  ASSERT_TRUE(physics.jointState(joint));
}

TEST(PhysicsSceneTests, FixedWorldJointSurvivesResetWithoutChangingItsSceneIdentity) {
  SceneGraph graph;
  const auto node = graph.createNode(translation(2, 4, -3), 0u);
  const auto authored = graph.getNode(node)->localTransform;
  PhysicsScene physics;
  const auto body = physics.attachBox(graph, node, glm::vec3{-.5f}, glm::vec3{.5f});
  container::physics::PhysicsJointSettings settings;
  settings.type = container::physics::PhysicsJointType::Fixed;
  settings.anchor = {2, 4, -3};
  const auto joint = physics.createJoint(graph, std::nullopt, node, settings);
  physics.setPlaying(true);
  for (int frame = 0; frame < 120; ++frame) physics.update(graph, PhysicsScene::kFixedStepSeconds);
  expectMatrixNear(graph.getNode(node)->worldTransform, authored, .002f);
  physics.reset(graph);
  EXPECT_NE(physics.bodyId(node), body);
  ASSERT_TRUE(physics.jointState(joint));
  EXPECT_EQ(physics.jointState(joint)->id, joint);
  EXPECT_EQ(physics.jointCount(), 1u);
  EXPECT_EQ(physics.statistics().jointCount, 1u);
  EXPECT_FALSE(physics.playing());
  expectMatrixNear(graph.getNode(node)->worldTransform, authored);
  physics.singleStep(graph);
  expectMatrixNear(graph.getNode(node)->worldTransform, authored, .002f);
}

TEST(PhysicsSceneTests, BodySettingsSnapshotTracksReplacementWithoutMutatingAuthoredSettings) {
  SceneGraph graph;
  const auto node = graph.createNode(translation(0, 4, 0), 0u);
  PhysicsScene physics{noGravity()};
  PhysicsBodySettings settings;
  settings.mass = 2.5f;
  settings.friction = .25f;
  settings.restitution = .75f;
  settings.gravityFactor = -.5f;
  settings.useCCD = true;
  settings.linearVelocity = {1, 0, 0};
  (void)physics.attachBox(graph, node, glm::vec3{-.5f}, glm::vec3{.5f}, settings);
  auto snapshot = physics.bodySettings(node);
  ASSERT_TRUE(snapshot);
  EXPECT_FLOAT_EQ(snapshot->mass, 2.5f);
  EXPECT_FLOAT_EQ(snapshot->gravityFactor, -.5f);
  EXPECT_TRUE(snapshot->useCCD);
  snapshot->mass = 100;
  EXPECT_FLOAT_EQ(physics.bodySettings(node)->mass, 2.5f);
  settings.motionType = PhysicsMotionType::Static;
  settings.friction = .1f;
  settings.useCCD = false;
  (void)physics.replaceBox(graph, node, glm::vec3{-.5f}, glm::vec3{.5f}, settings);
  physics.reset(graph);
  ASSERT_TRUE(physics.bodySettings(node));
  EXPECT_EQ(physics.bodySettings(node)->motionType, PhysicsMotionType::Static);
  EXPECT_FLOAT_EQ(physics.bodySettings(node)->friction, .1f);
  EXPECT_FALSE(physics.bodySettings(node)->useCCD);
  EXPECT_TRUE(physics.detach(graph, node));
  EXPECT_FALSE(physics.bodySettings(node));
}

TEST(PhysicsSceneTests, HingeZeroFrameAndMotorLimitsSurviveResetUnderRotatedParent) {
  SceneGraph graph;
  const auto parent = graph.createNode(glm::rotate(translation(3, 0, 0), .6f, glm::vec3{0, 0, 1}), 0u);
  const auto node = graph.createNode(translation(0, 3, 0), 0u, true, parent);
  const auto authored = graph.getNode(node)->localTransform;
  PhysicsScene physics{noGravity()};
  (void)physics.attachBox(graph, node, glm::vec3{-.5f}, glm::vec3{.5f});
  container::physics::PhysicsJointSettings settings;
  settings.anchor = physics.bodyPose(node)->position;
  settings.axis = {0, 0, 1};
  settings.normal = {1, 0, 0};
  settings.limitsEnabled = true;
  settings.lowerLimit = -.5f;
  settings.upperLimit = .5f;
  settings.motor.mode = container::physics::PhysicsJointMotorMode::Velocity;
  settings.motor.targetVelocity = 1;
  const auto joint = physics.createJoint(graph, std::nullopt, node, settings);
  physics.setPlaying(true);
  for (int frame = 0; frame < 100; ++frame) physics.update(graph, PhysicsScene::kFixedStepSeconds);
  ASSERT_TRUE(physics.jointState(joint));
  EXPECT_NEAR(physics.jointState(joint)->state.coordinate, .5f, .03f);
  physics.reset(graph);
  ASSERT_TRUE(physics.jointState(joint));
  EXPECT_NEAR(physics.jointState(joint)->state.coordinate, 0, .001f);
  expectMatrixNear(graph.getNode(node)->localTransform, authored);
  auto motor = settings.motor;
  motor.targetVelocity = -1;
  ASSERT_TRUE(physics.setJointMotor(joint, motor));
  physics.setPlaying(true);
  for (int frame = 0; frame < 100; ++frame) physics.update(graph, PhysicsScene::kFixedStepSeconds);
  EXPECT_NEAR(physics.jointState(joint)->state.coordinate, -.5f, .03f);
}

TEST(PhysicsSceneTests, SliderRetainsZeroFrameAcrossReplacementScaleEditAndReset) {
  SceneGraph graph;
  const auto node = graph.createNode(translation(0, 4, 0), 0u);
  PhysicsScene physics{noGravity()};
  (void)physics.attachBox(graph, node, glm::vec3{-.5f}, glm::vec3{.5f});
  container::physics::PhysicsJointSettings settings;
  settings.type = container::physics::PhysicsJointType::Slider;
  settings.anchor = {0, 4, 0};
  settings.axis = {1, 0, 0};
  settings.normal = {0, 1, 0};
  settings.limitsEnabled = true;
  settings.lowerLimit = -.75f;
  settings.upperLimit = .75f;
  settings.motor.mode = container::physics::PhysicsJointMotorMode::Velocity;
  settings.motor.targetVelocity = 1;
  const auto joint = physics.createJoint(graph, std::nullopt, node, settings);
  physics.setPlaying(true);
  for (int frame = 0; frame < 20; ++frame) physics.update(graph, PhysicsScene::kFixedStepSeconds);
  const auto before = *physics.jointState(joint);
  EXPECT_GT(before.state.coordinate, .2f);
  PhysicsBodySettings body;
  body.mass = 2;
  (void)physics.replaceBox(graph, node, glm::vec3{-.5f}, glm::vec3{.5f}, body);
  ASSERT_TRUE(physics.jointState(joint));
  EXPECT_NEAR(physics.jointState(joint)->state.coordinate, before.state.coordinate, .001f);
  physics.setPlaying(false);
  graph.setLocalTransform(node, glm::scale(graph.getNode(node)->localTransform, {2, 1, 1}));
  physics.update(graph, 0);
  EXPECT_NEAR(physics.jointState(joint)->state.coordinate, before.state.coordinate, .001f);
  physics.setPlaying(true);
  for (int frame = 0; frame < 100; ++frame) physics.update(graph, PhysicsScene::kFixedStepSeconds);
  EXPECT_NEAR(physics.jointState(joint)->state.coordinate, .75f, .03f);
  physics.reset(graph);
  EXPECT_NEAR(physics.jointState(joint)->state.coordinate, 0, .001f);
  EXPECT_EQ(physics.jointCount(), 1u);
}

TEST(PhysicsSceneTests, IncompatibleOrCapacityFailedReplacementKeepsBodyAndJoint) {
  for (const bool bounded : {false, true}) {
    SceneGraph graph;
    const auto node = graph.createNode(translation(0, 3, 0), 0u);
    auto world = noGravity();
    if (bounded) world.maxBodies = 1;
    PhysicsScene physics{world};
    const auto body = physics.attachBox(graph, node, glm::vec3{-.5f}, glm::vec3{.5f});
    container::physics::PhysicsJointSettings settings;
    settings.type = container::physics::PhysicsJointType::Fixed;
    settings.anchor = {0, 3, 0};
    const auto joint = physics.createJoint(graph, std::nullopt, node, settings);
    PhysicsBodySettings replacement;
    if (!bounded) replacement.motionType = PhysicsMotionType::Static;
    EXPECT_THROW((void)physics.replaceBox(graph, node, glm::vec3{-.5f}, glm::vec3{.5f}, replacement), std::exception);
    EXPECT_EQ(physics.bodyId(node), body);
    EXPECT_EQ(physics.motionType(node), PhysicsMotionType::Dynamic);
    EXPECT_TRUE(physics.jointState(joint));
    EXPECT_EQ(physics.jointCount(), 1u);
    physics.singleStep(graph);
    expectPositionNear(physics.bodyPose(node)->position, {0, 3, 0}, .002);
  }
}

TEST(PhysicsSceneTests, OffCenterGeometryJointAnchorFollowsScaleAndColliderCenterChanges) {
  SceneGraph graph;
  const auto node = graph.createNode(translation(0, 3, 0), 0u);
  PhysicsScene physics{noGravity()};
  (void)physics.attachBox(graph, node, {1, -.5f, -.5f}, {3, .5f, .5f});
  container::physics::PhysicsJointSettings settings;
  settings.type = container::physics::PhysicsJointType::Fixed;
  settings.anchor = {0, 3, 0};
  const auto joint = physics.createJoint(graph, std::nullopt, node, settings);
  graph.setLocalTransform(node, glm::scale(translation(0, 3, 0), {2, 1, 1}));
  physics.update(graph, 0);
  ASSERT_TRUE(physics.jointState(joint));
  EXPECT_NEAR(physics.jointState(joint)->state.frameB.anchor.x, -4, .001);
  physics.singleStep(graph);
  expectMatrixNear(graph.getNode(node)->worldTransform,
                    glm::scale(translation(0, 3, 0), {2, 1, 1}), .002f);
  (void)physics.replaceBox(graph, node, {3, -.5f, -.5f}, {5, .5f, .5f});
  EXPECT_NEAR(physics.jointState(joint)->state.frameB.anchor.x, -8, .001);
  physics.singleStep(graph);
  EXPECT_NEAR(graph.getNode(node)->worldTransform[3].x, 0, .002f);
  physics.reset(graph);
  EXPECT_NEAR(physics.jointState(joint)->state.frameB.anchor.x, -4, .001);
  physics.singleStep(graph);
  expectMatrixNear(graph.getNode(node)->worldTransform, translation(0, 3, 0), .002f);
}

TEST(PhysicsSceneTests, DetachRetiresOnlyConnectedJointsAndGraphReplacementRetiresAll) {
  SceneGraph graph;
  const auto first = graph.createNode(translation(0, 3, 0), 0u);
  const auto second = graph.createNode(translation(3, 3, 0), 0u);
  PhysicsScene physics{noGravity()};
  (void)physics.attachBox(graph, first, glm::vec3{-.5f}, glm::vec3{.5f});
  (void)physics.attachBox(graph, second, glm::vec3{-.5f}, glm::vec3{.5f});
  container::physics::PhysicsJointSettings settings;
  settings.type = container::physics::PhysicsJointType::Fixed;
  settings.anchor = {0, 3, 0};
  const auto between = physics.createJoint(graph, first, second, settings);
  const auto retained = physics.createJoint(graph, std::nullopt, second, settings);
  ASSERT_TRUE(physics.detach(graph, first));
  EXPECT_FALSE(physics.jointState(between));
  EXPECT_TRUE(physics.jointState(retained));
  EXPECT_FALSE(physics.destroyJoint(between));
  graph = SceneGraph{};
  (void)graph.createNode(translation(100, 200, 300), 0u);
  (void)graph.createNode(translation(400, 500, 600), 0u);
  physics.update(graph, 0);
  EXPECT_EQ(physics.jointCount(), 0u);
  EXPECT_EQ(physics.bodyCount(), 0u);
  EXPECT_FALSE(physics.jointState(retained));
  expectMatrixNear(graph.getNode(1)->localTransform, translation(400, 500, 600));
}

TEST(PhysicsSceneTests, InvalidJointEndpointsAndMotorUpdatesDoNotChangeExistingJoint) {
  SceneGraph graph;
  const auto node = graph.createNode(translation(0, 3, 0), 0u);
  PhysicsScene physics{noGravity()};
  (void)physics.attachBox(graph, node, glm::vec3{-.5f}, glm::vec3{.5f});
  container::physics::PhysicsJointSettings settings;
  settings.anchor = {0, 3, 0};
  const auto joint = physics.createJoint(graph, std::nullopt, node, settings);
  EXPECT_THROW((void)physics.createJoint(graph, node, node, settings), std::invalid_argument);
  EXPECT_THROW((void)physics.createJoint(graph, std::nullopt, 999, settings), std::invalid_argument);
  auto motor = settings.motor;
  motor.targetVelocity = std::numeric_limits<float>::quiet_NaN();
  EXPECT_THROW((void)physics.setJointMotor(joint, motor), std::invalid_argument);
  EXPECT_THROW((void)physics.setJointLimits(joint, true, 1, 2), std::invalid_argument);
  EXPECT_EQ(physics.jointCount(), 1u);
  EXPECT_FALSE(physics.setJointMotor(joint + 1000, settings.motor));
  EXPECT_EQ(physics.jointState(joint)->state.settings.motor.mode, container::physics::PhysicsJointMotorMode::Off);
}

} // namespace
