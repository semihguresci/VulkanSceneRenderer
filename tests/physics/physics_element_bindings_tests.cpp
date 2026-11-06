#include "Container/physics/PhysicsElementBindings.h"
#include "Container/utility/SceneGraph.h"

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

using namespace container::physics;
using container::gpu::ObjectTransformUpdate;
using container::scene::SceneGraph;

glm::mat4 translation(glm::vec3 value) {
  return glm::translate(glm::mat4{1.0f}, value);
}

PhysicsWorldSettings noGravity() {
  return {.gravity = glm::vec3{0.0f}, .maxBodies = 64u, .workerThreads = 1u};
}

PhysicsElementFragment fragment(uint32_t object, glm::vec3 center,
                                 glm::vec3 half = glm::vec3{0.5f}) {
  return {.objectIndex = object, .model = translation(center),
          .worldMin = center - half, .worldMax = center + half};
}

void expectMatrix(const glm::mat4 &actual, const glm::mat4 &expected,
                    float tolerance = 1e-4f) {
  for (int column = 0; column < 4; ++column)
    for (int row = 0; row < 4; ++row)
      EXPECT_NEAR(actual[column][row], expected[column][row], tolerance);
}

void expectModel(std::span<const ObjectTransformUpdate> updates,
                 uint32_t object, const glm::mat4 &expected,
                 float tolerance = 1e-4f) {
  const auto found = std::find_if(updates.begin(), updates.end(),
                                [object](const auto &update) {
                                  return update.objectIndex == object;
                                });
  ASSERT_NE(found, updates.end());
  expectMatrix(found->model, expected, tolerance);
}

TEST(PhysicsElementBindingsTests, ProductOwnsAllFragmentsAndPreservesTheirRelativeTransforms) {
  SceneGraph graph;
  PhysicsScene physics{noGravity()};
  PhysicsElementBindings bindings;
  auto first = fragment(4u, {1.0f, 5.0f, 0.0f});
  auto second = fragment(9u, {3.0f, 5.0f, 0.0f});
  first.model *= glm::scale(glm::mat4{1.0f}, {-1.0f, 2.0f, 0.5f});
  second.model *= glm::rotate(glm::mat4{1.0f}, 0.7f, {0.0f, 0.0f, 1.0f});
  const std::array originals{first, second};
  PhysicsBodySettings settings;
  settings.linearVelocity = {1.0f, 0.0f, 0.0f};
  settings.angularVelocity = {0.0f, 1.0f, 0.0f};
  {
    std::vector inputs{first, second};
    (void)bindings.attachOrReplace(graph, physics, 7u, 3u, inputs, settings,
                                    "Fragmented product");
    inputs.front().model = glm::mat4{0.0f};
  }
  ASSERT_EQ(bindings.productCount(), 1u);
  ASSERT_EQ(physics.bodyCount(), 1u);
  ASSERT_TRUE(bindings.bodyNodeForObject(4u));
  const auto node = *bindings.bodyNodeForObject(4u);
  EXPECT_EQ(bindings.bodyNodeForObject(9u), node);
  EXPECT_EQ(bindings.productForObject(9u), 3u);
  EXPECT_EQ(bindings.bodyNodeForProduct(3u), node);
  EXPECT_EQ(bindings.objectsForNode(node).size(), 2u);
  EXPECT_FALSE(graph.getNode(node)->renderable);
  EXPECT_EQ(graph.getNode(node)->name, "Fragmented product");
  graph.updateWorldTransforms();
  const auto anchor = graph.getNode(node)->worldTransform;
  const auto initial = bindings.collectTransforms(graph);
  for (const auto &entry : originals)
    expectModel(initial, entry.objectIndex, entry.model);

  physics.singleStep(graph);
  const auto delta = graph.getNode(node)->worldTransform * glm::inverse(anchor);
  const auto moved = bindings.collectTransforms(graph);
  for (const auto &entry : originals)
    expectModel(moved, entry.objectIndex, delta * entry.model);
  const auto relative = glm::inverse(originals[0].model) * originals[1].model;
  expectMatrix(glm::inverse(moved[0].model) * moved[1].model, relative);
}

TEST(PhysicsElementBindingsTests, AuxiliaryDynamicCollidesWithPrimaryStaticFloor) {
  SceneGraph graph;
  PhysicsScene physics{{.maxBodies = 64u, .workerThreads = 1u}};
  PhysicsElementBindings bindings;
  const auto floor = graph.createNode(translation({0.0f, -0.5f, 0.0f}), 0u);
  PhysicsBodySettings settings;
  settings.motionType = PhysicsMotionType::Static;
  (void)physics.attachBox(graph, floor, {-10.0f, -0.5f, -10.0f},
                           {10.0f, 0.5f, 10.0f}, settings);
  const std::array fragments{fragment(1u, {-0.75f, 3.0f, 0.0f}),
                             fragment(2u, {0.75f, 3.0f, 0.0f})};
  (void)bindings.attachOrReplace(graph, physics, 1u, 10u, fragments);
  physics.setPlaying(true);
  for (int frame = 0; frame < 240; ++frame)
    physics.update(graph, PhysicsScene::kFixedStepSeconds);
  ASSERT_EQ(physics.bodyCount(), 2u);
  const auto node = *bindings.bodyNodeForObject(1u);
  EXPECT_NEAR(physics.bodyPose(node)->position.y, 0.5, 0.04);
  EXPECT_EQ(physics.statistics().activeBodyCount, 0u);
  const auto updates = bindings.collectTransforms(graph);
  ASSERT_EQ(updates.size(), 2u);
  for (const auto &update : updates)
    EXPECT_NEAR(update.model[3].y, 0.5f, 0.04f);
  EXPECT_FLOAT_EQ(graph.getNode(floor)->worldTransform[3].y, -0.5f);
}

TEST(PhysicsElementBindingsTests, AuxiliaryStaticSupportsPrimaryDynamicBody) {
  SceneGraph graph;
  PhysicsScene physics{{.maxBodies = 64u, .workerThreads = 1u}};
  PhysicsElementBindings bindings;
  const auto box = graph.createNode(translation({0.0f, 3.0f, 0.0f}), 0u);
  (void)physics.attachBox(graph, box, glm::vec3{-0.5f}, glm::vec3{0.5f});
  const std::array fragments{fragment(8u, {0.0f, -0.5f, 0.0f},
                                       {5.0f, 0.5f, 5.0f})};
  PhysicsBodySettings settings;
  settings.motionType = PhysicsMotionType::Static;
  (void)bindings.attachOrReplace(graph, physics, 1u, 2u, fragments, settings);
  physics.setPlaying(true);
  for (int frame = 0; frame < 240; ++frame)
    physics.update(graph, PhysicsScene::kFixedStepSeconds);
  EXPECT_NEAR(physics.bodyPose(box)->position.y, 0.5, 0.04);
  const auto originalBody = physics.bodyId(box);
  bindings.synchronizePublication(graph, physics, 2u);
  EXPECT_EQ(physics.bodyId(box), originalBody);
  EXPECT_EQ(physics.bodyCount(), 1u);
  for (int frame = 0; frame < 30; ++frame)
    physics.update(graph, PhysicsScene::kFixedStepSeconds);
  EXPECT_LT(physics.bodyPose(box)->position.y, 0.0);
}

TEST(PhysicsElementBindingsTests, ProxyScaleAndRotationTransformEveryFragmentTogether) {
  SceneGraph graph;
  PhysicsScene physics{noGravity()};
  PhysicsElementBindings bindings;
  const std::array fragments{fragment(2u, {2.0f, 5.0f, 0.0f}),
                             fragment(3u, {4.0f, 5.0f, 0.0f})};
  (void)bindings.attachOrReplace(graph, physics, 1u, 1u, fragments);
  const auto node = *bindings.bodyNodeForObject(2u);
  graph.updateWorldTransforms();
  const auto anchor = graph.getNode(node)->worldTransform;
  const auto edited = translation({7.0f, 8.0f, 9.0f}) *
      glm::rotate(glm::mat4{1.0f}, 0.6f, {0.0f, 1.0f, 0.0f}) *
      glm::scale(glm::mat4{1.0f}, {2.0f, 3.0f, 0.5f});
  graph.setLocalTransform(node, edited);
  physics.update(graph, 0.0);
  const auto updates = bindings.collectTransforms(graph);
  for (const auto &entry : fragments)
    expectModel(updates, entry.objectIndex, edited * glm::inverse(anchor) * entry.model);
}

TEST(PhysicsElementBindingsTests, TransformPreviewScopesProductsWithoutChangingTheSimulation) {
  SceneGraph graph;
  PhysicsScene physics{noGravity()};
  PhysicsElementBindings bindings;
  const std::array first{fragment(4u, {1, 5, 0}), fragment(9u, {3, 5, 0})};
  const std::array second{fragment(12u, {20, 5, 0})};
  const auto body = bindings.attachOrReplace(graph, physics, 1, 1, first);
  (void)bindings.attachOrReplace(graph, physics, 1, 2, second);
  const auto proxy = *bindings.bodyNodeForObject(4);
  const auto root = graph.getNode(proxy)->parent;
  const auto unrelated = graph.createNode(glm::mat4{1}, 0);
  physics.setPlaying(true);
  physics.update(graph, PhysicsScene::kFixedStepSeconds / 2);
  const auto revision = graph.revision();
  const auto pose = *physics.bodyPose(proxy);
  const auto accumulated = physics.statistics().accumulatedSeconds;
  const auto local = graph.getNode(proxy)->localTransform;
  const auto proposed = translation({7, 8, 9}) *
      glm::rotate(glm::mat4{1}, .4f, glm::vec3{0, 1, 0}) *
      glm::scale(glm::mat4{1}, glm::vec3{2, 3, .5f});
  const auto preview = bindings.previewTransforms(graph, proxy, proposed);
  ASSERT_EQ(preview.size(), 2);
  for (const auto &entry : first)
    expectModel(preview, entry.objectIndex,
                proposed * translation({-2, -5, 0}) * entry.model);
  const auto rootEdit = translation({4, 2, -3});
  const auto all = bindings.previewTransforms(graph, root, rootEdit);
  ASSERT_EQ(all.size(), 3);
  for (const auto &entry : first) expectModel(all, entry.objectIndex, rootEdit * entry.model);
  expectModel(all, second[0].objectIndex, rootEdit * second[0].model);
  EXPECT_TRUE(bindings.previewTransforms(graph, unrelated, proposed).empty());
  EXPECT_TRUE(bindings.previewTransforms(graph, SceneGraph::kInvalidNode, proposed).empty());
  SceneGraph foreign;
  EXPECT_TRUE(bindings.previewTransforms(foreign, proxy, proposed).empty());
  EXPECT_EQ(graph.revision(), revision);
  EXPECT_EQ(graph.getNode(proxy)->localTransform, local);
  EXPECT_EQ(physics.bodyId(proxy), body);
  EXPECT_EQ(physics.bodyPose(proxy)->position, pose.position);
  EXPECT_EQ(physics.bodyPose(proxy)->rotation, pose.rotation);
  EXPECT_TRUE(physics.playing());
  EXPECT_EQ(physics.statistics().accumulatedSeconds, accumulated);
  const auto actual = bindings.collectTransforms(graph);
  for (const auto &entry : first) expectModel(actual, entry.objectIndex, entry.model);
  expectModel(actual, second[0].objectIndex, second[0].model);
}

TEST(PhysicsElementBindingsTests, TransformPreviewMatchesPublicationUnderACompensatedRoot) {
  SceneGraph graph;
  PhysicsScene physics{noGravity()};
  PhysicsElementBindings bindings;
  const PhysicsBodySettings settings{.motionType = PhysicsMotionType::Static};
  const std::array first{fragment(0, {0, 5, 0})};
  (void)bindings.attachOrReplace(graph, physics, 1, 1, first, settings);
  const auto root = graph.getNode(*bindings.bodyNodeForObject(0))->parent;
  const auto rootWorld = glm::rotate(glm::mat4{1}, glm::radians(45.f), glm::vec3{0, 0, 1}) *
      glm::scale(glm::mat4{1}, glm::vec3{2, 1, 1});
  graph.setLocalTransform(root, rootWorld);
  physics.update(graph, 0);
  const std::array second{fragment(1, {5, 5, 0}), fragment(2, {7, 5, 0})};
  (void)bindings.attachOrReplace(graph, physics, 1, 2, second, settings);
  const auto proxy = *bindings.bodyNodeForObject(1);
  const auto before = graph.getNode(proxy)->localTransform;
  auto proposed = before;
  proposed[3] += glm::vec4{1, -.5f, 2, 0};
  const auto preview = bindings.previewTransforms(graph, proxy, proposed);
  ASSERT_EQ(preview.size(), 2);
  graph.setLocalTransform(proxy, proposed);
  graph.updateWorldTransforms();
  const auto published = bindings.collectTransforms(graph);
  for (const auto &entry : preview) expectModel(published, entry.objectIndex, entry.model);
  const auto movedRoot = translation({3, 2, 1}) * rootWorld;
  const auto rootPreview = bindings.previewTransforms(graph, root, movedRoot);
  ASSERT_EQ(rootPreview.size(), 3);
  graph.setLocalTransform(root, movedRoot);
  graph.updateWorldTransforms();
  const auto all = bindings.collectTransforms(graph);
  for (const auto &entry : rootPreview) expectModel(all, entry.objectIndex, entry.model);
}

TEST(PhysicsElementBindingsTests, RootTranslationPreservesVelocitiesAndRebasesResetAndDetach) {
  SceneGraph graph;
  PhysicsScene physics{noGravity()};
  PhysicsElementBindings bindings;
  const std::array fragments{fragment(1u, {2.0f, 5.0f, 0.0f}),
                             fragment(2u, {4.0f, 5.0f, 0.0f})};
  PhysicsBodySettings settings;
  settings.linearVelocity = {2.0f, 0.0f, 0.0f};
  (void)bindings.attachOrReplace(graph, physics, 1u, 1u, fragments, settings);
  const auto node = *bindings.bodyNodeForObject(1u);
  physics.setPlaying(true);
  physics.update(graph, PhysicsScene::kFixedStepSeconds);
  const auto before = bindings.collectTransforms(graph);
  const glm::vec3 offset{10.0f, 2.0f, 3.0f};
  bindings.translate(graph, physics, offset);
  EXPECT_FALSE(physics.playing());
  physics.update(graph, 0.0);
  const auto rebased = bindings.collectTransforms(graph);
  for (const auto &entry : before)
    expectModel(rebased, entry.objectIndex, translation(offset) * entry.model);
  const auto prior = physics.bodyPose(node)->position;
  physics.singleStep(graph);
  EXPECT_GT(physics.bodyPose(node)->position.x, prior.x + 0.03);

  physics.reset(graph);
  const auto reset = bindings.collectTransforms(graph);
  for (const auto &entry : fragments)
    expectModel(reset, entry.objectIndex, translation(offset) * entry.model);
  physics.singleStep(graph);
  const auto restored = bindings.detach(graph, physics, 2u);
  ASSERT_EQ(restored.size(), 2u);
  for (const auto &entry : fragments)
    expectModel(restored, entry.objectIndex, translation(offset) * entry.model);
  EXPECT_EQ(bindings.productCount(), 0u);
  EXPECT_EQ(physics.bodyCount(), 0u);
  EXPECT_FALSE(bindings.bodyNodeForObject(1u));
  EXPECT_TRUE(bindings.collectTransforms(graph).empty());
}

TEST(PhysicsElementBindingsTests, ProductsAttachedAfterRootMoveHaveIndependentResetAnchors) {
  SceneGraph graph;
  PhysicsScene physics{noGravity()};
  PhysicsElementBindings bindings;
  const std::array first{fragment(1u, {0.0f, 5.0f, 0.0f})};
  const std::array second{fragment(2u, {20.0f, 5.0f, 0.0f})};
  (void)bindings.attachOrReplace(graph, physics, 1u, 1u, first);
  bindings.translate(graph, physics, {10.0f, 0.0f, 0.0f});
  physics.update(graph, 0.0);
  (void)bindings.attachOrReplace(graph, physics, 1u, 2u, second);
  expectModel(bindings.collectTransforms(graph), 2u, second[0].model);
  bindings.translate(graph, physics, {5.0f, 0.0f, 0.0f});
  physics.update(graph, 0.0);
  physics.reset(graph);
  const auto restored = bindings.clear(&graph, physics);
  expectModel(restored, 1u, translation({15.0f, 0.0f, 0.0f}) * first[0].model);
  expectModel(restored, 2u, translation({5.0f, 0.0f, 0.0f}) * second[0].model);
  EXPECT_EQ(physics.bodyCount(), 0u);
}

TEST(PhysicsElementBindingsTests, ReplacementPreservesLivePoseAndOriginalResetAnchors) {
  SceneGraph graph;
  PhysicsScene physics{noGravity()};
  PhysicsElementBindings bindings;
  std::array fragments{fragment(1u, {2.0f, 5.0f, 0.0f}),
                        fragment(2u, {4.0f, 5.0f, 0.0f})};
  const auto originals = fragments;
  PhysicsBodySettings settings;
  settings.angularVelocity = {0.0f, 1.0f, 0.0f};
  const auto originalBody = bindings.attachOrReplace(graph, physics, 1u, 3u, fragments, settings);
  for (int frame = 0; frame < 20; ++frame)
    physics.singleStep(graph);
  const auto node = *bindings.bodyNodeForObject(1u);
  const auto live = bindings.collectTransforms(graph);
  for (auto &entry : fragments)
    for (const auto &update : live)
      if (entry.objectIndex == update.objectIndex)
        entry.model = update.model;
  settings.motionType = PhysicsMotionType::Static;
  const auto replacement = bindings.attachOrReplace(graph, physics, 1u, 3u, fragments, settings);
  EXPECT_NE(replacement, originalBody);
  EXPECT_EQ(bindings.bodyNodeForObject(1u), node);
  EXPECT_EQ(physics.bodyCount(), 1u);
  EXPECT_EQ(physics.motionType(node), PhysicsMotionType::Static);
  for (const auto &entry : live)
    expectModel(bindings.collectTransforms(graph), entry.objectIndex, entry.model);
  physics.reset(graph);
  for (const auto &entry : originals)
    expectModel(bindings.collectTransforms(graph), entry.objectIndex, entry.model);
}

TEST(PhysicsElementBindingsTests, PlayingKinematicReplacementPreservesPendingFragmentedSupportContactSweep) {
  SceneGraph graph, referenceGraph;
  PhysicsScene physics{noGravity()}, reference{noGravity()};
  PhysicsElementBindings bindings, referenceBindings;
  const std::array fragments{
      fragment(4u, {-2.25f, 5.0f, 0.0f}, {.25f, .5f, .5f}),
      fragment(7u, {-1.75f, 5.0f, 0.0f}, {.25f, .5f, .5f})};
  PhysicsBodySettings supportSettings;
  supportSettings.motionType = PhysicsMotionType::Kinematic;
  supportSettings.friction = 0;
  const auto populate = [&](SceneGraph &targetGraph, PhysicsScene &targetPhysics,
                            PhysicsElementBindings &targetBindings) {
    const auto pushed = targetGraph.createNode(translation({0, 5, 0}), 0u);
    (void)targetPhysics.attachBox(targetGraph, pushed, glm::vec3{-.5f},
                                  glm::vec3{.5f}, {.friction = 0});
    (void)targetBindings.attachOrReplace(targetGraph, targetPhysics, 1u, 3u,
                                          fragments, supportSettings);
    const auto support = *targetBindings.bodyNodeForObject(4u);
    targetPhysics.setPlaying(true);
    targetGraph.setLocalTransform(support, translation({2, 5, 0}));
    return std::array{pushed, support};
  };
  const auto nodes = populate(graph, physics, bindings);
  const auto referenceNodes = populate(referenceGraph, reference, referenceBindings);
  const auto originalBody = *physics.bodyId(nodes[1]);
  const auto replacement = bindings.attachOrReplace(graph, physics, 1u, 3u,
                                                     fragments, supportSettings);
  EXPECT_NE(replacement, originalBody);
  EXPECT_EQ(bindings.bodyNodeForObject(4u), nodes[1]);
  EXPECT_EQ(bindings.bodyNodeForObject(7u), nodes[1]);
  EXPECT_EQ(physics.bodyCount(), 2u);
  EXPECT_TRUE(physics.playing());
  // The authored target remains visible in the graph, but replacing settings
  // must retain the old physical pose until collision ticks traverse the gap.
  EXPECT_NEAR(physics.bodyPose(nodes[1])->position.x, -2.0, 1e-5);
  expectMatrix(graph.getNode(nodes[1])->localTransform, translation({2, 5, 0}));
  physics.update(graph, 4 * PhysicsScene::kFixedStepSeconds);
  reference.update(referenceGraph, 4 * PhysicsScene::kFixedStepSeconds);
  const auto pushed = physics.bodyPose(nodes[0])->position;
  const auto referencePushed = reference.bodyPose(referenceNodes[0])->position;
  EXPECT_GT(referencePushed.x, 2.5);
  EXPECT_NEAR(glm::length(pushed - referencePushed), 0.0, 1e-3);
  EXPECT_NEAR(physics.bodyPose(nodes[1])->position.x, 2.0, 1e-4);
  EXPECT_EQ(physics.statistics().totalSteps, 4u);
  const auto updates = bindings.collectTransforms(graph);
  ASSERT_EQ(updates.size(), fragments.size());
  for (const auto &entry : fragments)
    expectModel(updates, entry.objectIndex,
                  translation({4, 0, 0}) * entry.model);
}

TEST(PhysicsElementBindingsTests, FailedReplacementRetainsBodyMembershipAndMotion) {
  SceneGraph graph;
  auto worldSettings = noGravity();
  worldSettings.maxBodies = 1u;
  PhysicsScene physics{worldSettings};
  PhysicsElementBindings bindings;
  const std::array fragments{fragment(1u, {0.0f, 5.0f, 0.0f})};
  PhysicsBodySettings settings;
  settings.linearVelocity = {2.0f, 0.0f, 0.0f};
  const auto original = bindings.attachOrReplace(graph, physics, 1u, 1u, fragments, settings);
  const auto node = *bindings.bodyNodeForObject(1u);
  settings.motionType = PhysicsMotionType::Static;
  EXPECT_THROW((void)bindings.attachOrReplace(graph, physics, 1u, 1u, fragments, settings),
               std::runtime_error);
  EXPECT_EQ(physics.bodyId(node), original);
  EXPECT_EQ(physics.motionType(node), PhysicsMotionType::Dynamic);
  EXPECT_EQ(bindings.productForObject(1u), 1u);
  EXPECT_EQ(bindings.productCount(), 1u);
  physics.singleStep(graph);
  EXPECT_GT(physics.bodyPose(node)->position.x, 0.03);
}

TEST(PhysicsElementBindingsTests, PublicationChangeRetiresOnlyAuxiliaryAndReusesProxyNodes) {
  SceneGraph graph;
  PhysicsScene physics{noGravity()};
  PhysicsElementBindings bindings;
  const auto primary = graph.createNode(translation({-10.0f, 5.0f, 0.0f}), 0u);
  const auto primaryBody = physics.attachBox(graph, primary, glm::vec3{-0.5f}, glm::vec3{0.5f});
  const std::array fragments{fragment(1u, {0.0f, 5.0f, 0.0f})};
  const auto original = bindings.attachOrReplace(graph, physics, 10u, 2u, fragments);
  const auto proxy = *bindings.bodyNodeForObject(1u);
  const auto count = graph.nodeCount();
  physics.reset(graph);
  ASSERT_NE(physics.bodyId(proxy), original);
  const auto resetPrimaryBody = physics.bodyId(primary);
  bindings.synchronizePublication(graph, physics, 11u);
  EXPECT_TRUE(bindings.collectTransforms(graph).empty());
  EXPECT_EQ(bindings.productCount(), 0u);
  EXPECT_EQ(physics.bodyId(primary), resetPrimaryBody);
  EXPECT_EQ(physics.bodyCount(), 1u);
  const std::array replacementFragments{fragment(1u, {100.0f, 5.0f, 0.0f})};
  const auto replacement = bindings.attachOrReplace(graph, physics, 11u, 2u, replacementFragments);
  EXPECT_NE(replacement, original);
  EXPECT_EQ(bindings.bodyNodeForObject(1u), proxy);
  EXPECT_EQ(graph.nodeCount(), count);
  expectModel(bindings.collectTransforms(graph), 1u, replacementFragments[0].model);
}

TEST(PhysicsElementBindingsTests, GraphReplacementCannotDetachNewPrimaryAtReusedProxyIndex) {
  SceneGraph graph;
  PhysicsScene physics{noGravity()};
  PhysicsElementBindings bindings;
  const std::array fragments{fragment(1u, {0.0f, 5.0f, 0.0f})};
  (void)bindings.attachOrReplace(graph, physics, 1u, 1u, fragments);
  const auto oldProxy = *bindings.bodyNodeForObject(1u);
  graph = SceneGraph{};
  for (uint32_t index = 0; index <= oldProxy; ++index)
    (void)graph.createNode(translation({float(index), 20.0f, 0.0f}), 0u);
  const auto primary = physics.attachBox(graph, oldProxy, glm::vec3{-0.5f}, glm::vec3{0.5f});
  EXPECT_TRUE(bindings.collectTransforms(graph).empty());
  bindings.synchronizePublication(graph, physics, 2u);
  EXPECT_EQ(physics.bodyId(oldProxy), primary);
  EXPECT_EQ(physics.bodyCount(), 1u);
  EXPECT_EQ(bindings.productCount(), 0u);
}

TEST(PhysicsElementBindingsTests, GraphReplacementRetiresResetAndResizedWorldsBeforeNextUpdate) {
  for (const bool resize : {false, true}) {
    for (const bool clear : {false, true}) {
      SCOPED_TRACE(testing::Message() << "resize=" << resize << " clear=" << clear);
      SceneGraph graph;
      PhysicsScene physics{noGravity()};
      PhysicsElementBindings bindings;
      const auto anchor = graph.createNode(translation({-10.0f, 5.0f, 0.0f}), 0u);
      PhysicsBodySettings support;
      support.motionType = PhysicsMotionType::Static;
      (void)physics.attachBox(graph, anchor, glm::vec3{-.5f}, glm::vec3{.5f}, support);
      const std::array fragments{fragment(1u, {0.0f, 5.0f, 0.0f})};
      const auto original = bindings.attachOrReplace(graph, physics, 1u, 1u, fragments);
      const auto proxy = *bindings.bodyNodeForObject(1u);
      PhysicsJointSettings settings;
      settings.anchor = {0.0, 5.0, 0.0};
      const auto joint = physics.createJoint(graph, anchor, proxy, settings);
      if (resize) {
        graph.setLocalTransform(proxy, translation({0.0f, 5.0f, 0.0f}) *
            glm::scale(glm::mat4{1.0f}, {2.0f, 1.0f, 1.0f}));
        physics.update(graph, 0.0);
      } else {
        physics.reset(graph);
      }
      ASSERT_NE(physics.bodyId(proxy), original);
      ASSERT_TRUE(physics.jointState(joint));

      graph = SceneGraph{};
      const auto newNode = graph.createNode(translation({10.0f, 20.0f, 30.0f}), 0u);
      const auto newLocal = graph.getNode(newNode)->localTransform;
      ASSERT_EQ(physics.bodyCount(), 2u);
      ASSERT_EQ(graph.getNode(proxy), nullptr);
      if (clear)
        EXPECT_TRUE(bindings.clear(&graph, physics).empty());
      else
        bindings.synchronizePublication(graph, physics, 2u);
      EXPECT_EQ(physics.bodyCount(), 0u);
      EXPECT_EQ(physics.jointCount(), 0u);
      EXPECT_FALSE(physics.jointState(joint));
      EXPECT_EQ(bindings.productCount(), 0u);
      EXPECT_TRUE(bindings.collectTransforms(graph).empty());
      EXPECT_EQ(graph.getNode(newNode)->localTransform, newLocal);

      const auto primary = physics.attachBox(graph, newNode, glm::vec3{-.5f}, glm::vec3{.5f});
      (void)bindings.attachOrReplace(graph, physics, 2u, 1u, fragments);
      EXPECT_EQ(physics.bodyId(newNode), primary);
      EXPECT_EQ(physics.bodyCount(), 2u);
      expectModel(bindings.collectTransforms(graph), 1u, fragments[0].model);
    }
  }
}

TEST(PhysicsElementBindingsTests, GraphReplacementPreservesNewWorldAfterOldBodiesWereRecreated) {
  for (const bool resize : {false, true}) {
    for (const bool clear : {false, true}) {
      SCOPED_TRACE(testing::Message() << "resize=" << resize << " clear=" << clear);
      SceneGraph graph;
      PhysicsScene physics{noGravity()};
      PhysicsElementBindings bindings;
      const std::array fragments{fragment(1u, {0.0f, 5.0f, 0.0f})};
      const auto original = bindings.attachOrReplace(graph, physics, 1u, 1u, fragments);
      const auto proxy = *bindings.bodyNodeForObject(1u);
      if (resize) {
        graph.setLocalTransform(proxy, translation({0.0f, 5.0f, 0.0f}) *
            glm::scale(glm::mat4{1.0f}, {2.0f, 1.0f, 1.0f}));
        physics.update(graph, 0.0);
      } else {
        physics.reset(graph);
      }
      ASSERT_NE(physics.bodyId(proxy), original);

      graph = SceneGraph{};
      for (uint32_t index = 0; index <= proxy; ++index)
        (void)graph.createNode(translation({float(index), 20.0f, 0.0f}), 0u);
      const auto primary = physics.attachBox(graph, proxy, glm::vec3{-.5f}, glm::vec3{.5f});
      PhysicsJointSettings settings;
      settings.anchor = physics.bodyPose(proxy)->position;
      const auto joint = physics.createJoint(graph, std::nullopt, proxy, settings);
      physics.setPlaying(true);
      const auto newLocal = graph.getNode(proxy)->localTransform;
      if (clear)
        EXPECT_TRUE(bindings.clear(&graph, physics).empty());
      else
        bindings.synchronizePublication(graph, physics, 2u);
      EXPECT_EQ(physics.bodyId(proxy), primary);
      EXPECT_EQ(physics.bodyCount(), 1u);
      EXPECT_EQ(physics.jointCount(), 1u);
      EXPECT_TRUE(physics.jointState(joint));
      EXPECT_TRUE(physics.playing());
      EXPECT_EQ(bindings.productCount(), 0u);
      EXPECT_EQ(graph.getNode(proxy)->localTransform, newLocal);
    }
  }
}

TEST(PhysicsElementBindingsTests, InvalidFragmentsAndOverlappingProductsRetainExistingBinding) {
  SceneGraph graph;
  PhysicsScene physics{noGravity()};
  PhysicsElementBindings bindings;
  const std::array fragments{fragment(1u, {0.0f, 5.0f, 0.0f})};
  const auto original = bindings.attachOrReplace(graph, physics, 1u, 1u, fragments);
  const auto node = *bindings.bodyNodeForObject(1u);
  const auto count = graph.nodeCount();
  EXPECT_THROW((void)bindings.attachOrReplace(graph, physics, 1u, 2u, fragments),
               std::invalid_argument);
  EXPECT_THROW((void)bindings.attachOrReplace(graph, physics, 1u, 0u, fragments),
               std::invalid_argument);
  EXPECT_THROW((void)bindings.attachOrReplace(graph, physics, 1u, 2u, {}),
               std::invalid_argument);
  const std::array duplicate{fragments[0], fragments[0]};
  EXPECT_THROW((void)bindings.attachOrReplace(graph, physics, 1u, 1u, duplicate),
               std::invalid_argument);
  const std::array changedMembership{fragment(2u, {0.0f, 5.0f, 0.0f})};
  EXPECT_THROW((void)bindings.attachOrReplace(graph, physics, 1u, 1u, changedMembership),
               std::invalid_argument);
  std::array invalid{fragments[0]};
  invalid[0].worldMax.y = -10.0f;
  EXPECT_THROW((void)bindings.attachOrReplace(graph, physics, 1u, 1u, invalid),
               std::invalid_argument);
  invalid = fragments;
  invalid[0].model[0][0] = std::numeric_limits<float>::quiet_NaN();
  EXPECT_THROW((void)bindings.attachOrReplace(graph, physics, 1u, 1u, invalid),
               std::invalid_argument);
  EXPECT_EQ(physics.bodyId(node), original);
  EXPECT_EQ(bindings.productCount(), 1u);
  EXPECT_EQ(graph.nodeCount(), count);
  EXPECT_FALSE(bindings.bodyNodeForObject(2u));
}

TEST(PhysicsElementBindingsTests, FailedNewAttachmentAndRepeatedDetachReuseBoundedProxyPool) {
  SceneGraph graph;
  PhysicsScene physics{noGravity()};
  PhysicsElementBindings bindings;
  const std::array fragments{fragment(1u, {0.0f, 5.0f, 0.0f})};
  PhysicsBodySettings invalid;
  invalid.mass = 0.0f;
  for (int attempt = 0; attempt < 10; ++attempt) {
    EXPECT_THROW((void)bindings.attachOrReplace(graph, physics, 1u, 1u, fragments, invalid),
                 std::invalid_argument);
    EXPECT_EQ(bindings.productCount(), 0u);
    EXPECT_EQ(physics.bodyCount(), 0u);
  }
  const auto count = graph.nodeCount();
  EXPECT_EQ(count, 2u);
  for (uint32_t product = 1u; product <= 100u; ++product) {
    (void)bindings.attachOrReplace(graph, physics, 1u, product, fragments);
    EXPECT_EQ(bindings.detach(graph, physics, 1u).size(), 1u);
  }
  EXPECT_EQ(graph.nodeCount(), count);
  EXPECT_EQ(bindings.productCount(), 0u);
  EXPECT_EQ(physics.bodyCount(), 0u);
  EXPECT_TRUE(bindings.clear(&graph, physics).empty());
}

TEST(PhysicsElementBindingsTests, UnpublishableFragmentModelsCannotCreateOrReplaceBodies) {
  SceneGraph graph;
  PhysicsScene physics{noGravity()};
  PhysicsElementBindings bindings;
  const std::array fragments{fragment(1u, {0.0f, 5.0f, 0.0f})};
  PhysicsBodySettings settings;
  settings.linearVelocity = {2.0f, 0.0f, 0.0f};
  const auto body = bindings.attachOrReplace(graph, physics, 1u, 1u, fragments, settings);
  const auto node = *bindings.bodyNodeForObject(1u);
  const auto count = graph.nodeCount();
  std::vector<glm::mat4> invalidModels;
  auto invalid = fragments[0].model;
  invalid[0] = glm::vec4{0.0f};
  invalidModels.push_back(invalid);
  invalid = fragments[0].model;
  invalid[1] = invalid[0];
  invalidModels.push_back(invalid);
  invalid = fragments[0].model;
  invalid[0][3] = 1e-6f;
  invalidModels.push_back(invalid);
  invalid = fragments[0].model;
  invalid[3][3] += 1e-6f;
  invalidModels.push_back(invalid);
  invalid = fragments[0].model;
  invalid[0][0] = 1e-40f; // An invertible double matrix whose float normal overflows.
  invalidModels.push_back(invalid);
  for (const auto &model : invalidModels) {
    const std::array replacement{PhysicsElementFragment{
        .objectIndex = 1u, .model = model,
        .worldMin = fragments[0].worldMin, .worldMax = fragments[0].worldMax}};
    const std::array addition{PhysicsElementFragment{
        .objectIndex = 2u, .model = model,
        .worldMin = fragments[0].worldMin, .worldMax = fragments[0].worldMax}};
    EXPECT_THROW((void)bindings.attachOrReplace(graph, physics, 1u, 1u, replacement),
                 std::invalid_argument);
    EXPECT_THROW((void)bindings.attachOrReplace(graph, physics, 1u, 2u, addition),
                 std::invalid_argument);
    EXPECT_EQ(physics.bodyId(node), body);
    EXPECT_EQ(bindings.productCount(), 1u);
    EXPECT_EQ(bindings.productForObject(1u), 1u);
    EXPECT_FALSE(bindings.bodyNodeForObject(2u));
    EXPECT_EQ(graph.nodeCount(), count);
    expectModel(bindings.collectTransforms(graph), 1u, fragments[0].model);
  }
  physics.singleStep(graph);
  EXPECT_GT(physics.bodyPose(node)->position.x, .03);
}

TEST(PhysicsElementBindingsTests, SmallMirroredFragmentScalesRemainPublishable) {
  SceneGraph graph;
  PhysicsScene physics{noGravity()};
  PhysicsElementBindings bindings;
  auto tiny = fragment(1u, {0.0f, 5.0f, 0.0f});
  tiny.model *= glm::scale(glm::mat4{1.0f}, {-1e-12f, 2e-12f, 3e-12f});
  const std::array fragments{tiny};
  PhysicsBodySettings settings;
  settings.linearVelocity = {2.0f, 0.0f, 0.0f};
  EXPECT_NO_THROW((void)bindings.attachOrReplace(graph, physics, 1u, 1u, fragments, settings));
  physics.singleStep(graph);
  const auto updates = bindings.collectTransforms(graph);
  ASSERT_EQ(updates.size(), 1u);
  EXPECT_GT(updates[0].model[3].x, .03f);
  for (int column = 0; column < 3; ++column)
    EXPECT_EQ(updates[0].model[column], tiny.model[column]);
}

TEST(PhysicsElementBindingsTests, NullClearRequiresRetiredBodiesAndNeverClearsPrimaryWorld) {
  SceneGraph graph;
  PhysicsScene physics{noGravity()};
  PhysicsElementBindings bindings;
  const auto primary = graph.createNode(translation({-10.0f, 5.0f, 0.0f}), 0u);
  const auto primaryBody = physics.attachBox(graph, primary, glm::vec3{-0.5f}, glm::vec3{0.5f});
  const std::array fragments{fragment(1u, {0.0f, 5.0f, 0.0f})};
  (void)bindings.attachOrReplace(graph, physics, 1u, 1u, fragments);
  EXPECT_THROW((void)bindings.clear(nullptr, physics), std::invalid_argument);
  EXPECT_EQ(physics.bodyId(primary), primaryBody);
  EXPECT_EQ(physics.bodyCount(), 2u);
  EXPECT_TRUE(bindings.clear(&graph, physics, false).empty());
  EXPECT_EQ(physics.bodyId(primary), primaryBody);
  EXPECT_EQ(physics.bodyCount(), 1u);
  EXPECT_TRUE(bindings.clear(nullptr, physics).empty());
}

TEST(PhysicsElementBindingsTests, NullClearRejectsResetLiveBodiesButAcceptsReusedIndicesInNewGraph) {
  SceneGraph graph;
  PhysicsScene physics{noGravity()};
  PhysicsElementBindings bindings;
  const std::array fragments{fragment(1u, {0.0f, 5.0f, 0.0f})};
  const auto original = bindings.attachOrReplace(graph, physics, 1u, 1u, fragments);
  const auto proxy = *bindings.bodyNodeForObject(1u);
  physics.reset(graph);
  ASSERT_NE(physics.bodyId(proxy), original);
  EXPECT_THROW((void)bindings.clear(nullptr, physics), std::invalid_argument);
  EXPECT_EQ(bindings.productCount(), 1u);
  EXPECT_EQ(physics.bodyCount(), 1u);

  graph = SceneGraph{};
  for (uint32_t index = 0; index <= proxy; ++index)
    (void)graph.createNode(translation({float(index), 20.0f, 0.0f}), 0u);
  const auto primary = physics.attachBox(graph, proxy, glm::vec3{-.5f}, glm::vec3{.5f});
  EXPECT_NO_THROW((void)bindings.clear(nullptr, physics));
  EXPECT_EQ(bindings.productCount(), 0u);
  EXPECT_EQ(physics.bodyId(proxy), primary);
  EXPECT_EQ(physics.bodyCount(), 1u);
}

TEST(PhysicsElementBindingsTests, KinematicTranslationKeepsPlaybackAndInvalidTranslationIsAtomic) {
  SceneGraph graph;
  PhysicsScene physics{noGravity()};
  PhysicsElementBindings bindings;
  const std::array fragments{fragment(1u, {0.0f, 5.0f, 0.0f})};
  PhysicsBodySettings settings;
  settings.motionType = PhysicsMotionType::Kinematic;
  (void)bindings.attachOrReplace(graph, physics, 1u, 1u, fragments, settings);
  physics.setPlaying(true);
  bindings.translate(graph, physics, {7.0f, 0.0f, 0.0f});
  EXPECT_TRUE(physics.playing());
  physics.update(graph, PhysicsScene::kFixedStepSeconds);
  expectModel(bindings.collectTransforms(graph), 1u,
                translation({7.0f, 0.0f, 0.0f}) * fragments[0].model);
  const auto before = bindings.collectTransforms(graph);
  EXPECT_THROW(bindings.translate(graph, physics,
                                  {std::numeric_limits<float>::infinity(), 0.0f, 0.0f}),
               std::invalid_argument);
  expectModel(bindings.collectTransforms(graph), 1u, before[0].model);
  EXPECT_TRUE(physics.playing());
}

TEST(PhysicsElementBindingsTests, OwnershipIncludesRootAndRecycledProxiesOnlyInTheirGraphPublication) {
  SceneGraph graph;
  PhysicsScene physics{noGravity()};
  PhysicsElementBindings bindings;
  const auto primary = graph.createNode(translation({-10.0f, 5.0f, 0.0f}), 0u);
  const auto primaryBody = physics.attachBox(graph, primary, glm::vec3{-0.5f}, glm::vec3{0.5f});
  const std::array first{fragment(1u, {0.0f, 5.0f, 0.0f})};
  const std::array second{fragment(2u, {2.0f, 5.0f, 0.0f})};
  (void)bindings.attachOrReplace(graph, physics, 1u, 1u, first);
  (void)bindings.attachOrReplace(graph, physics, 1u, 2u, second);
  const auto firstProxy = *bindings.bodyNodeForObject(1u);
  const auto secondProxy = *bindings.bodyNodeForObject(2u);
  const auto root = graph.getNode(firstProxy)->parent;
  EXPECT_FALSE(bindings.ownsNode(graph, primary));
  EXPECT_FALSE(bindings.ownsNode(graph, SceneGraph::kInvalidNode));
  EXPECT_TRUE(bindings.ownsNode(graph, root));
  EXPECT_TRUE(bindings.ownsNode(graph, firstProxy));
  EXPECT_TRUE(bindings.ownsNode(graph, secondProxy));

  (void)bindings.detach(graph, physics, 1u);
  EXPECT_TRUE(bindings.ownsNode(graph, firstProxy));
  (void)bindings.clear(&graph, physics);
  EXPECT_TRUE(bindings.ownsNode(graph, root));
  EXPECT_TRUE(bindings.ownsNode(graph, firstProxy));
  EXPECT_TRUE(bindings.ownsNode(graph, secondProxy));
  EXPECT_EQ(physics.bodyId(primary), primaryBody);
  const auto count = graph.nodeCount();
  (void)bindings.attachOrReplace(graph, physics, 1u, 1u, first);
  EXPECT_EQ(graph.nodeCount(), count);
  EXPECT_TRUE(bindings.ownsNode(graph, *bindings.bodyNodeForObject(1u)));

  const auto oldCount = graph.nodeCount();
  graph = SceneGraph{};
  for (size_t node = 0; node < oldCount; ++node)
    (void)graph.createNode(glm::mat4{1.0f}, 0u);
  EXPECT_FALSE(bindings.ownsNode(graph, root));
  EXPECT_FALSE(bindings.ownsNode(graph, firstProxy));
  EXPECT_FALSE(bindings.ownsNode(graph, secondProxy));
  bindings.synchronizePublication(graph, physics, 2u);
  EXPECT_FALSE(bindings.ownsNode(graph, root));
  EXPECT_FALSE(bindings.ownsNode(graph, firstProxy));
}

TEST(PhysicsElementBindingsTests, ArticulatedProductPreservesFragmentsAndRetiresCrossProviderJointOnReload) {
  SceneGraph graph;
  PhysicsScene physics{noGravity()};
  PhysicsElementBindings bindings;
  const auto anchor = graph.createNode(translation({0, 4, 0}), 0u);
  PhysicsBodySettings support;
  support.motionType = PhysicsMotionType::Static;
  (void)physics.attachBox(graph, anchor, glm::vec3{-.5f}, glm::vec3{.5f}, support);
  const std::array fragments{fragment(0, {-1, 3, 0}), fragment(1, {1, 3, 0})};
  (void)bindings.attachOrReplace(graph, physics, 1, 5, fragments);
  const auto productNode = *bindings.bodyNodeForObject(0);
  PhysicsJointSettings settings;
  settings.anchor = {0, 3, 0};
  settings.axis = {0, 0, 1};
  settings.normal = {1, 0, 0};
  settings.limitsEnabled = true;
  settings.lowerLimit = -.4f;
  settings.upperLimit = .4f;
  settings.motor.mode = PhysicsJointMotorMode::Velocity;
  settings.motor.targetVelocity = 1;
  const auto joint = physics.createJoint(graph, anchor, productNode, settings);
  physics.setPlaying(true);
  for (int frame = 0; frame < 100; ++frame) physics.update(graph, PhysicsScene::kFixedStepSeconds);
  ASSERT_TRUE(physics.jointState(joint));
  const auto angle = physics.jointState(joint)->state.coordinate;
  EXPECT_NEAR(angle, .4f, .03f);
  const auto delta = translation({0, 3, 0}) * glm::rotate(glm::mat4{1}, angle, glm::vec3{0, 0, 1}) * translation({0, -3, 0});
  const auto updates = bindings.collectTransforms(graph);
  expectModel(updates, 0, delta * fragments[0].model, .003f);
  expectModel(updates, 1, delta * fragments[1].model, .003f);
  physics.reset(graph);
  EXPECT_TRUE(physics.jointState(joint));
  expectModel(bindings.collectTransforms(graph), 0, fragments[0].model);
  bindings.synchronizePublication(graph, physics, 2);
  EXPECT_EQ(physics.jointCount(), 0u);
  EXPECT_FALSE(physics.jointState(joint));
  EXPECT_EQ(physics.bodyCount(), 1u);
  EXPECT_TRUE(physics.hasBody(anchor));
}

} // namespace
