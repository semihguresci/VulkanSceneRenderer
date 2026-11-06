#include "Container/physics/PhysicsWorld.h"

// Derive the budget boundary from the pinned backend's actual ABI/layout.
#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <future>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

using namespace container::physics;
constexpr float kStep = 1.0f / 60.0f;

PhysicsWorldSettings testWorldSettings() {
  return {.gravity = {0.0f, -9.81f, 0.0f},
          .maxBodies = 64u, .workerThreads = 1u};
}

void simulate(PhysicsWorld &world, size_t count, float delta = kStep) {
  for (size_t step = 0; step < count; ++step)
    world.step(delta);
}

PhysicsBodyId addFloor(PhysicsWorld &world) {
  return world.createBox({10.0f, 0.5f, 10.0f},
                         {.motionType = PhysicsMotionType::Static,
                          .position = {0.0, -0.5, 0.0}});
}

void expectPosition(const PhysicsWorld &world, PhysicsBodyId id,
                     glm::dvec3 expected, double tolerance = 1e-5) {
  const auto pose = world.pose(id);
  ASSERT_TRUE(pose);
  EXPECT_NEAR(pose->position.x, expected.x, tolerance);
  EXPECT_NEAR(pose->position.y, expected.y, tolerance);
  EXPECT_NEAR(pose->position.z, expected.z, tolerance);
}

TEST(PhysicsWorldTests, FallingBoxSettlesOnStaticFloorAndSleeps) {
  PhysicsWorld world(testWorldSettings());
  const auto floor = addFloor(world);
  const auto box = world.createBox({0.5f, 0.5f, 0.5f},
                                   {.position = {0.0, 4.0, 0.0}});
  ASSERT_NE(box, kInvalidPhysicsBodyId);
  EXPECT_EQ(world.bodyCount(), 2u);
  EXPECT_GE(world.activeBodyCount(), 1u);
  simulate(world, 240u);
  expectPosition(world, floor, {0.0, -0.5, 0.0});
  expectPosition(world, box, {0.0, 0.5, 0.0}, 0.04);
  const auto velocity = world.linearVelocity(box);
  ASSERT_TRUE(velocity);
  EXPECT_NEAR(glm::length(*velocity), 0.0f, 0.02f);
  EXPECT_EQ(world.activeBodyCount(), 0u);
}

TEST(PhysicsWorldTests, StaticBodiesIgnoreGravityAndVelocityMutation) {
  PhysicsWorld world(testWorldSettings());
  const auto body = world.createSphere(0.5f,
      {.motionType = PhysicsMotionType::Static, .position = {1.0, 5.0, 3.0},
       .linearVelocity = {10.0f, 2.0f, 0.0f}});
  EXPECT_FALSE(world.setLinearVelocity(body, {3.0f, 0.0f, 0.0f}));
  EXPECT_FALSE(world.setAngularVelocity(body, {0.0f, 1.0f, 0.0f}));
  simulate(world, 60u);
  expectPosition(world, body, {1.0, 5.0, 3.0});
  EXPECT_EQ(*world.linearVelocity(body), glm::vec3(0.0f));
  EXPECT_EQ(*world.angularVelocity(body), glm::vec3(0.0f));
}

TEST(PhysicsWorldTests, KinematicBodyReachesTargetWithoutGravity) {
  PhysicsWorld world(testWorldSettings());
  const auto body = world.createBox({0.5f, 0.5f, 0.5f},
      {.motionType = PhysicsMotionType::Kinematic, .position = {0.0, 4.0, 0.0}});
  const PhysicsBodyPose target{{1.0, 4.0, -0.5},
      glm::angleAxis(0.2f, glm::vec3(0.0f, 1.0f, 0.0f))};
  EXPECT_TRUE(world.moveKinematic(body, target, kStep));
  world.step(kStep);
  expectPosition(world, body, target.position, 1e-4);
  ASSERT_TRUE(world.pose(body));
  EXPECT_NEAR(std::abs(glm::dot(world.pose(body)->rotation, target.rotation)),
              1.0f, 1e-5f);
  EXPECT_TRUE(world.setLinearVelocity(body, glm::vec3(0.0f)));
  EXPECT_TRUE(world.setAngularVelocity(body, glm::vec3(0.0f)));
  simulate(world, 60u);
  expectPosition(world, body, target.position, 1e-4);

  const auto dynamic = world.createSphere(0.5f, {.position = {10.0, 5.0, 0.0}});
  EXPECT_FALSE(world.moveKinematic(dynamic, target, kStep));
}

TEST(PhysicsWorldTests, RejectedKinematicTargetsRetainPoseVelocitiesAndSleepState) {
  auto settings = testWorldSettings();
  settings.gravity = glm::vec3(0.0f);
  PhysicsWorld world(settings);
  const glm::vec3 linear{0.5f, -0.25f, 0.75f};
  const glm::vec3 angular{0.1f, 0.2f, -0.3f};
  const auto rotation = glm::angleAxis(0.35f, glm::normalize(glm::vec3(1, 2, 3)));
  const auto body = world.createBox(glm::vec3(0.5f),
      {.motionType = PhysicsMotionType::Kinematic, .position = {3.0, 4.0, 5.0},
       .rotation = rotation, .linearVelocity = linear, .angularVelocity = angular});
  const auto before = world.pose(body).value();
  const float tiny = std::numeric_limits<float>::min() * 0.3f;
  ASSERT_TRUE(std::isfinite(1.0f / tiny));
  struct RejectedTarget {
    PhysicsBodyPose pose;
    float duration;
  };
  const std::array<RejectedTarget, 4> rejected{{
      {{{1e37, 4.0, 5.0}, rotation}, kStep}, // Velocity component overflows.
      {{{4.0, 4.0, 5.0}, rotation}, 1e-20f}, // Finite velocity, squared speed overflows.
      {before, tiny}, // Jolt's (2 / dt) * zero angular delta produces NaNs.
      {{{3.0, 4.0, 5.0}, glm::angleAxis(1.0f, glm::vec3(0, 1, 0)) * rotation},
        std::numeric_limits<float>::min()}}};
  for (size_t index = 0; index < rejected.size(); ++index) {
    SCOPED_TRACE(index);
    EXPECT_THROW((void)world.moveKinematic(body, rejected[index].pose,
                                          rejected[index].duration), std::invalid_argument);
    expectPosition(world, body, before.position);
    EXPECT_EQ(world.pose(body)->rotation, before.rotation);
    EXPECT_EQ(world.linearVelocity(body).value(), linear);
    EXPECT_EQ(world.angularVelocity(body).value(), angular);
  }
  world.step(kStep);
  expectPosition(world, body, before.position + glm::dvec3{linear} * double(kStep), 1e-6);
  ASSERT_TRUE(world.setLinearVelocity(body, glm::vec3(0.0f)));
  ASSERT_TRUE(world.setAngularVelocity(body, glm::vec3(0.0f)));
  simulate(world, 120u);
  ASSERT_EQ(world.activeBodyCount(), 0u);
  const auto sleeping = world.pose(body).value();
  EXPECT_THROW((void)world.moveKinematic(body, {{1e37, 0.0, 0.0}}, kStep),
               std::invalid_argument);
  EXPECT_EQ(world.activeBodyCount(), 0u);
  expectPosition(world, body, sleeping.position);
  EXPECT_EQ(world.linearVelocity(body).value(), glm::vec3(0.0f));
  EXPECT_EQ(world.angularVelocity(body).value(), glm::vec3(0.0f));
}

TEST(PhysicsWorldTests, FastRotatedKinematicTargetsRetainUncappedMotionAndQuaternionEquivalence) {
  PhysicsWorld world(testWorldSettings());
  const auto initial = glm::angleAxis(0.6f, glm::vec3(1, 0, 0));
  const auto body = world.createBox({0.25f, 0.5f, 0.75f},
      {.motionType = PhysicsMotionType::Kinematic, .position = {1.0, 4.0, 2.0},
       .rotation = initial});
  const auto desired = glm::angleAxis(1.2f, glm::vec3(0, 0, 1)) * initial;
  const PhysicsBodyPose target{{21.0, 4.0, 2.0}, desired * -2.5f};
  ASSERT_TRUE(world.moveKinematic(body, target, kStep));
  EXPECT_NEAR(world.linearVelocity(body)->x, 1200.0f, 0.001f);
  EXPECT_NEAR(glm::length(world.angularVelocity(body).value()), 72.0f, 0.001f);
  world.step(kStep);
  expectPosition(world, body, target.position, 1e-4);
  EXPECT_NEAR(std::abs(glm::dot(world.pose(body)->rotation, desired)), 1.0f, 1e-5f);
  const auto reached = world.pose(body).value();
  ASSERT_TRUE(world.moveKinematic(body, {reached.position, -reached.rotation}, kStep));
  EXPECT_NEAR(glm::length(world.linearVelocity(body).value()), 0.0f, 1e-5f);
  EXPECT_NEAR(glm::length(world.angularVelocity(body).value()), 0.0f, 1e-5f);
  world.step(kStep);
  expectPosition(world, body, reached.position);
  EXPECT_NEAR(std::abs(glm::dot(world.pose(body)->rotation, desired)), 1.0f, 1e-5f);
}

TEST(PhysicsWorldTests, SphereAndCapsuleRestOnOwnedIndexedTriangleMesh) {
  PhysicsWorld world(testWorldSettings());
  PhysicsBodyId floor;
  {
    const std::array<glm::vec3, 4> vertices{{
        {-10.0f, 0.0f, -10.0f}, {10.0f, 0.0f, -10.0f},
        {10.0f, 0.0f, 10.0f}, {-10.0f, 0.0f, 10.0f}}};
    const std::array<uint32_t, 6> indices{0u, 2u, 1u, 0u, 3u, 2u};
    floor = world.createStaticMesh(vertices, indices);
  }
  const auto sphere = world.createSphere(0.5f,
      {.position = {-2.0, 4.0, 0.0}});
  const auto capsule = world.createCapsule(0.5f, 0.5f,
      {.position = {2.0, 4.0, 0.0}});
  simulate(world, 240u);
  EXPECT_TRUE(world.contains(floor));
  expectPosition(world, sphere, {-2.0, 0.5, 0.0}, 0.04);
  const auto capsulePose = world.pose(capsule);
  ASSERT_TRUE(capsulePose);
  const auto capsuleAxis = capsulePose->rotation * glm::vec3(0, 1, 0);
  const double supportedHeight = 0.5 + 0.5 * std::abs(capsuleAxis.y);
  EXPECT_NEAR(capsulePose->position.y, supportedHeight, 0.04);
  EXPECT_LT(std::abs(capsulePose->position.x - 2.0), 1.5);
}

TEST(PhysicsWorldTests, ZeroHeightCapsuleBecomesSphereAndSmallBoxesAreValid) {
  PhysicsWorld world(testWorldSettings());
  const auto capsule = world.createCapsule(0.0f, 0.5f,
      {.motionType = PhysicsMotionType::Static});
  const auto small = world.createBox({0.001f, 0.002f, 0.003f},
      {.position = {10.0, 3.0, 0.0}});
  EXPECT_TRUE(world.contains(capsule));
  EXPECT_TRUE(world.contains(small));
  world.step(kStep);
  EXPECT_LT(world.pose(small)->position.y, 3.0);
}

TEST(PhysicsWorldTests, PoseAndVelocityUpdatesPreserveQuaternionConvention) {
  PhysicsWorld world(testWorldSettings());
  const auto body = world.createBox({0.5f, 0.5f, 0.5f},
                                    {.gravityFactor = 0.0f});
  const auto rotation = glm::angleAxis(0.7f, glm::normalize(glm::vec3(1, 2, 3)));
  EXPECT_TRUE(world.setPose(body, {{2.0, 3.0, 4.0}, rotation * 2.0f}));
  expectPosition(world, body, {2.0, 3.0, 4.0});
  EXPECT_NEAR(std::abs(glm::dot(world.pose(body)->rotation, rotation)), 1.0f, 1e-6f);
  EXPECT_TRUE(world.setLinearVelocity(body, {2.0f, 0.0f, 0.0f}));
  EXPECT_TRUE(world.setAngularVelocity(body, {0.0f, 0.5f, 0.0f}));
  EXPECT_EQ(*world.linearVelocity(body), glm::vec3(2.0f, 0.0f, 0.0f));
  EXPECT_EQ(*world.angularVelocity(body), glm::vec3(0.0f, 0.5f, 0.0f));
  world.step(kStep);
  EXPECT_GT(world.pose(body)->position.x, 2.0);
  EXPECT_NEAR(world.pose(body)->position.y, 3.0, 1e-6);
}

TEST(PhysicsWorldTests, LargeWorldPositionsRetainSubmeterPrecision) {
  PhysicsWorld world(testWorldSettings());
  const glm::dvec3 position{1'000'000'000.125, 2'000'000'000.25,
                            -3'000'000'000.375};
  const auto body = world.createSphere(0.5f,
      {.motionType = PhysicsMotionType::Static, .position = position});
  expectPosition(world, body, position, 1e-6);
  EXPECT_TRUE(world.setPose(body, {position + glm::dvec3(0.125, 0.25, 0.5)}));
  expectPosition(world, body, position + glm::dvec3(0.125, 0.25, 0.5), 1e-6);
}

TEST(PhysicsWorldTests, InitialAndEditedVelocitiesShareSafeBackendCapsForCcdBodies) {
  auto settings = testWorldSettings();
  settings.gravity = glm::vec3(0.0f);
  PhysicsWorld world(settings);
  constexpr float maxLinearSpeed = 500.0f;
  constexpr float maxAngularSpeed = 0.25f * 3.14159265358979323846f * 60.0f;
  const float largest = std::numeric_limits<float>::max();
  const std::array<glm::vec3, 3> velocities{{
      {600.0f, 0.0f, 0.0f}, {400.0f, -400.0f, 400.0f},
      {largest, largest * 0.5f, -largest * 0.25f}}};
  size_t pair = 0u;
  for (const auto motion : {PhysicsMotionType::Dynamic, PhysicsMotionType::Kinematic}) {
    for (const auto velocity : velocities) {
      SCOPED_TRACE(::testing::Message() << "motion=" << int(motion) << " pair=" << pair);
      const glm::dvec3 start{0.0, 0.0, 20.0 * double(pair++)};
      const auto initial = world.createSphere(0.5f,
          {.motionType = motion, .position = start,
           .linearVelocity = velocity, .angularVelocity = velocity, .useCCD = true});
      const auto edited = world.createSphere(0.5f,
          {.motionType = motion, .position = start + glm::dvec3(100.0, 0.0, 0.0),
           .useCCD = true});
      ASSERT_TRUE(world.setLinearVelocity(edited, velocity));
      ASSERT_TRUE(world.setAngularVelocity(edited, velocity));
      const auto linear = world.linearVelocity(initial).value();
      const auto angular = world.angularVelocity(initial).value();
      EXPECT_NEAR(glm::length(linear), maxLinearSpeed, 0.001f);
      EXPECT_NEAR(glm::length(angular), maxAngularSpeed, 0.0001f);
      const auto direction = glm::normalize(glm::dvec3{velocity});
      EXPECT_NEAR(glm::dot(glm::dvec3{linear} / double(glm::length(linear)), direction), 1.0, 1e-6);
      EXPECT_NEAR(glm::dot(glm::dvec3{angular} / double(glm::length(angular)), direction), 1.0, 1e-6);
      EXPECT_EQ(linear, world.linearVelocity(edited).value());
      EXPECT_EQ(angular, world.angularVelocity(edited).value());
      world.step(kStep);
      const auto pose = world.pose(initial).value();
      EXPECT_TRUE(std::isfinite(pose.position.x));
      EXPECT_TRUE(std::isfinite(pose.position.y));
      EXPECT_TRUE(std::isfinite(pose.position.z));
      EXPECT_NEAR(glm::length(pose.rotation), 1.0f, 1e-5f);
      EXPECT_GT(glm::dot(pose.position - start, direction), 8.0);
    }
  }

  const glm::vec3 ordinaryLinear{1.0f, -2.0f, 3.0f};
  const glm::vec3 ordinaryAngular{-0.1f, 0.2f, -0.3f};
  const auto ordinary = world.createSphere(0.5f,
      {.position = {0.0, 0.0, 200.0}, .linearVelocity = ordinaryLinear,
       .angularVelocity = ordinaryAngular});
  EXPECT_EQ(world.linearVelocity(ordinary).value(), ordinaryLinear);
  EXPECT_EQ(world.angularVelocity(ordinary).value(), ordinaryAngular);
}

TEST(PhysicsWorldTests, HandlesDoNotAliasAfterDestroyClearOrDifferentWorld) {
  PhysicsWorld first(testWorldSettings()), second(testWorldSettings());
  const auto old = first.createSphere(0.5f);
  EXPECT_FALSE(second.contains(old));
  EXPECT_FALSE(second.pose(old));
  EXPECT_TRUE(first.destroyBody(old));
  EXPECT_FALSE(first.destroyBody(old));
  const auto replacement = first.createSphere(0.5f);
  EXPECT_NE(replacement, old);
  first.clear();
  const auto reloaded = first.createSphere(0.5f);
  const auto independent = second.createSphere(0.5f);
  EXPECT_NE(reloaded, replacement);
  EXPECT_NE(reloaded, independent);
  for (const auto stale : {kInvalidPhysicsBodyId, old, replacement, independent}) {
    EXPECT_FALSE(first.contains(stale));
    EXPECT_FALSE(first.pose(stale));
    EXPECT_FALSE(first.linearVelocity(stale));
    EXPECT_FALSE(first.angularVelocity(stale));
    EXPECT_FALSE(first.setPose(stale, {}));
    EXPECT_FALSE(first.setLinearVelocity(stale, {}));
    EXPECT_FALSE(first.setAngularVelocity(stale, {}));
    EXPECT_FALSE(first.moveKinematic(stale, {}, kStep));
    EXPECT_FALSE(first.destroyBody(stale));
  }
  EXPECT_EQ(first.bodyCount(), 1u);
  EXPECT_TRUE(first.contains(reloaded));
}

TEST(PhysicsWorldTests, CapacityFailureLeavesExistingBodyAndAllowsReuse) {
  auto settings = testWorldSettings();
  settings.maxBodies = 1u;
  PhysicsWorld world(settings);
  const auto first = world.createSphere(0.5f);
  EXPECT_THROW((void)world.createSphere(0.5f), std::runtime_error);
  EXPECT_EQ(world.bodyCount(), 1u);
  EXPECT_TRUE(world.contains(first));
  EXPECT_TRUE(world.destroyBody(first));
  const auto replacement = world.createSphere(0.5f);
  EXPECT_NE(first, replacement);
  world.clear();
  EXPECT_EQ(world.bodyCount(), 0u);
  EXPECT_EQ(world.activeBodyCount(), 0u);
  world.clear();
  world.step(kStep);
}

TEST(PhysicsWorldTests, RepeatedWorldLifecyclePreservesOtherLiveWorld) {
  PhysicsWorld survivor(testWorldSettings());
  const auto body = survivor.createSphere(0.5f,
      {.position = {0.0, 5.0, 0.0}});
  for (size_t cycle = 0; cycle < 8u; ++cycle) {
    PhysicsWorld temporary(testWorldSettings());
    (void)addFloor(temporary);
    (void)temporary.createSphere(0.5f,
        {.position = {0.0, 3.0, 0.0}});
    temporary.step(kStep);
    survivor.step(kStep);
  }
  EXPECT_TRUE(survivor.contains(body));
  EXPECT_LT(survivor.pose(body)->position.y, 5.0);
}

TEST(PhysicsWorldTests, UnsupportedCollisionBudgetIsRejectedBeforeWorldAllocation) {
  PhysicsWorld survivor(testWorldSettings());
  const auto body = survivor.createSphere(0.5f, {.position = {0.0, 5.0, 0.0}});
  auto settings = testWorldSettings();
  // Contact limits depend on sizeof backend solver types. Derive the first
  // rejected capacity for this build instead of risking a huge allocation
  // when another compiler/layout supports a different number of contacts.
  constexpr uint32_t supportedCapacity = std::min({
      JPH::PhysicsSystem::cMaxBodiesLimit,
      JPH::ContactConstraintManager::cMaxContactConstraintsLimit / 4u,
      JPH::ContactConstraintManager::cMaxBodyPairsLimit / 4u});
  static_assert(supportedCapacity < std::numeric_limits<uint32_t>::max());
  for (const auto capacity : {supportedCapacity + 1u,
                              std::numeric_limits<uint32_t>::max()}) {
    SCOPED_TRACE(capacity);
    settings.maxBodies = capacity;
    EXPECT_THROW(PhysicsWorld{settings}, std::invalid_argument);
    ASSERT_EQ(survivor.bodyCount(), 1u);
    ASSERT_TRUE(survivor.contains(body));
    survivor.step(kStep);
  }
  EXPECT_LT(survivor.pose(body)->position.y, 5.0);
  PhysicsWorld usableAfterFailures(testWorldSettings());
  EXPECT_EQ(usableAfterFailures.bodyCount(), 0u);
}

TEST(PhysicsWorldTests, ConcurrentIndependentWorldLifetimesShareRuntimeSafely) {
  std::vector<std::future<void>> work;
  for (size_t worker = 0; worker < 3u; ++worker)
    work.push_back(std::async(std::launch::async, [] {
      for (size_t cycle = 0; cycle < 6u; ++cycle) {
        PhysicsWorld world(testWorldSettings());
        const auto body = world.createSphere(0.5f,
            {.position = {0.0, 5.0, 0.0}});
        simulate(world, 3u);
        if (!world.pose(body) || world.pose(body)->position.y >= 5.0)
          throw std::runtime_error("Independent physics world did not advance");
      }
    }));
  for (auto &task : work)
    EXPECT_NO_THROW(task.get());
  PhysicsWorld after(testWorldSettings());
  EXPECT_EQ(after.bodyCount(), 0u);
}

TEST(PhysicsWorldTests, EqualFixedStepsMatchAcrossIndependentWorlds) {
  PhysicsWorld first(testWorldSettings()), second(testWorldSettings());
  const auto one = first.createSphere(0.5f,
      {.position = {0.0, 5.0, 0.0}, .linearVelocity = {2.0f, 0.0f, 0.0f}});
  const auto two = second.createSphere(0.5f,
      {.position = {0.0, 5.0, 0.0}, .linearVelocity = {2.0f, 0.0f, 0.0f}});
  simulate(first, 60u);
  for (size_t frame = 0; frame < 20u; ++frame)
    simulate(second, 3u);
  ASSERT_TRUE(first.pose(one));
  expectPosition(second, two, first.pose(one)->position, 1e-6);
  EXPECT_NEAR(glm::length(*first.linearVelocity(one) - *second.linearVelocity(two)),
              0.0f, 1e-6f);
}

TEST(PhysicsWorldTests, HalfSizeTimestepsProduceConsistentGravityMotion) {
  PhysicsWorld first(testWorldSettings()), second(testWorldSettings());
  const auto one = first.createSphere(0.5f,
      {.position = {0.0, 10.0, 0.0}});
  const auto two = second.createSphere(0.5f,
      {.position = {0.0, 10.0, 0.0}});
  simulate(first, 60u);
  simulate(second, 120u, kStep * 0.5f);
  EXPECT_NEAR(first.pose(one)->position.y, second.pose(two)->position.y, 0.06);
  EXPECT_LT(first.pose(one)->position.y, 6.0);
  EXPECT_GT(first.pose(one)->position.y, 4.0);
}

TEST(PhysicsWorldTests, RejectsInvalidShapeAndMeshDataWithoutInstallingBodies) {
  PhysicsWorld world(testWorldSettings());
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float infinity = std::numeric_limits<float>::infinity();
  for (const auto dimensions : {glm::vec3(0), glm::vec3(-1),
                                 glm::vec3(nan, 1, 1), glm::vec3(infinity, 1, 1)})
    EXPECT_THROW((void)world.createBox(dimensions), std::invalid_argument);
  for (const float radius : {0.0f, -1.0f, nan, infinity}) {
    EXPECT_THROW((void)world.createSphere(radius), std::invalid_argument);
    EXPECT_THROW((void)world.createCapsule(1.0f, radius), std::invalid_argument);
  }
  EXPECT_THROW((void)world.createCapsule(-1.0f, 1.0f), std::invalid_argument);
  EXPECT_THROW((void)world.createCapsule(nan, 1.0f), std::invalid_argument);
  const std::array<glm::vec3, 3> vertices{{{0, 0, 0}, {1, 0, 0}, {0, 0, 1}}};
  const std::array<uint32_t, 3> valid{0u, 2u, 1u};
  const std::array<uint32_t, 3> invalid{0u, 2u, 3u};
  const std::array<uint32_t, 3> degenerate{0u, 0u, 0u};
  const std::array<uint32_t, 2> incomplete{0u, 1u};
  EXPECT_THROW((void)world.createStaticMesh({}, {}), std::invalid_argument);
  EXPECT_THROW((void)world.createStaticMesh(vertices, invalid), std::invalid_argument);
  EXPECT_THROW((void)world.createStaticMesh(vertices, incomplete), std::invalid_argument);
  EXPECT_THROW((void)world.createStaticMesh(vertices, degenerate), std::invalid_argument);
  EXPECT_THROW((void)world.createStaticMesh(vertices, valid, {}), std::invalid_argument);
  auto nonfinite = vertices;
  nonfinite.front().x = nan;
  EXPECT_THROW((void)world.createStaticMesh(nonfinite, valid), std::invalid_argument);
  EXPECT_EQ(world.bodyCount(), 0u);
}

TEST(PhysicsWorldTests, RejectsInvalidSettingsPoseVelocityAndTimestep) {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  auto settings = testWorldSettings();
  settings.maxBodies = 0u;
  EXPECT_THROW(PhysicsWorld{settings}, std::invalid_argument);
  settings = testWorldSettings();
  settings.gravity.x = nan;
  EXPECT_THROW(PhysicsWorld{settings}, std::invalid_argument);
  settings = testWorldSettings();
  settings.workerThreads = std::numeric_limits<uint32_t>::max();
  EXPECT_THROW(PhysicsWorld{settings}, std::invalid_argument);

  PhysicsWorld world(testWorldSettings());
  std::vector<PhysicsBodySettings> invalid(11u);
  invalid[0].mass = 0.0f;
  invalid[1].mass = nan;
  invalid[2].position.x = std::numeric_limits<double>::infinity();
  invalid[3].rotation = glm::quat(0, 0, 0, 0);
  invalid[4].rotation.x = nan;
  invalid[5].linearVelocity.x = nan;
  invalid[6].friction = -1.0f;
  invalid[7].restitution = 1.1f;
  invalid[8].gravityFactor = nan;
  invalid[9].mass = std::numeric_limits<float>::denorm_min();
  invalid[10].position.x = std::numeric_limits<double>::max();
  for (const auto &bodySettings : invalid)
    EXPECT_THROW((void)world.createSphere(0.5f, bodySettings), std::invalid_argument);
  EXPECT_EQ(world.bodyCount(), 0u);
  const auto body = world.createSphere(0.5f);
  const auto kinematic = world.createSphere(0.5f,
      {.motionType = PhysicsMotionType::Kinematic, .position = {5.0, 0.0, 0.0}});
  EXPECT_THROW((void)world.setPose(body, {{nan, 0, 0}}), std::invalid_argument);
  EXPECT_THROW((void)world.setPose(body, {{}, {0, 0, 0, 0}}), std::invalid_argument);
  EXPECT_THROW((void)world.setLinearVelocity(body, {nan, 0, 0}), std::invalid_argument);
  EXPECT_THROW((void)world.setAngularVelocity(body, {nan, 0, 0}), std::invalid_argument);
  for (const float duration : {0.0f, -kStep, nan,
                                std::numeric_limits<float>::denorm_min(),
                                std::numeric_limits<float>::infinity()}) {
    EXPECT_THROW(world.step(duration), std::invalid_argument);
    EXPECT_THROW((void)world.moveKinematic(kinematic, {}, duration), std::invalid_argument);
  }
  EXPECT_EQ(world.bodyCount(), 2u);
}

TEST(PhysicsWorldTests, NonrepresentableInverseInertiaIsRejectedBeforeBodyPublication) {
  auto settings = testWorldSettings();
  settings.gravity = glm::vec3(0.0f);
  settings.maxBodies = 2;
  PhysicsWorld world(settings);
  const auto survivor = world.createSphere(0.5f,
      {.position = {10.0, 0.0, 0.0}, .linearVelocity = {1.0f, 0.0f, 0.0f}});
  PhysicsBodySettings invalid;
  invalid.mass = std::numeric_limits<float>::min() * 0.5f;
  ASSERT_TRUE(std::isfinite(1.0f / invalid.mass));
  for (const auto motion : {PhysicsMotionType::Dynamic, PhysicsMotionType::Kinematic}) {
    invalid.motionType = motion;
    for (int shape = 0; shape < 3; ++shape) {
      SCOPED_TRACE(::testing::Message() << "motion=" << int(motion) << " shape=" << shape);
      const auto create = [&] {
        if (shape == 0) return world.createBox(glm::vec3(0.5f), invalid);
        if (shape == 1) return world.createSphere(0.5f, invalid);
        return world.createCapsule(0.5f, 0.5f, invalid);
      };
      ASSERT_THROW((void)create(), std::invalid_argument);
      EXPECT_EQ(world.bodyCount(), 1u);
      ASSERT_TRUE(world.contains(survivor));
      world.step(kStep);
      EXPECT_TRUE(std::isfinite(world.pose(survivor)->position.x));
    }
  }
  EXPECT_GT(world.pose(survivor)->position.x, 10.0);
}

TEST(PhysicsWorldTests, UnrepresentableGravityAccelerationIsRejectedBeforeBodyPublication) {
  for (const float factor : {1e38f, 1e20f}) {
    SCOPED_TRACE(factor);
    PhysicsWorld world(testWorldSettings());
    PhysicsBodySettings invalid;
    invalid.gravityFactor = factor;
    // Stop before stepping the old backend's nonfinite squared-speed path.
    ASSERT_THROW((void)world.createBox(glm::vec3{.5f}, invalid), std::invalid_argument);
    EXPECT_EQ(world.bodyCount(), 0u);
    ASSERT_THROW((void)world.createSphere(.5f, invalid), std::invalid_argument);
    ASSERT_THROW((void)world.createCapsule(.5f, .5f, invalid), std::invalid_argument);
    EXPECT_EQ(world.bodyCount(), 0u);
  }
  for (const float factor : {-1e10f, 0.0f, 1e10f}) {
    SCOPED_TRACE(factor);
    PhysicsWorld world(testWorldSettings());
    const auto body = world.createSphere(.5f, {.gravityFactor = factor});
    world.step(kStep);
    const auto velocity = *world.linearVelocity(body);
    EXPECT_TRUE(std::isfinite(velocity.y));
    EXPECT_LE(glm::length(velocity), 500.0f);
    if (factor == 0)
      EXPECT_EQ(velocity, glm::vec3{0});
    else
      EXPECT_GT(velocity.y * -std::copysign(1.0f, factor), 0);
  }
  // Factors have no acceleration effect on these motion types or when the
  // world has no gravity. Retain finite, otherwise valid unused settings.
  PhysicsWorld withoutGravity({.gravity = {0, 0, 0}, .maxBodies = 4, .workerThreads = 1});
  const auto free = withoutGravity.createSphere(.5f, {.gravityFactor = 1e38f});
  withoutGravity.step(kStep);
  expectPosition(withoutGravity, free, {0, 0, 0});
  PhysicsWorld world(testWorldSettings());
  const auto stationary = world.createSphere(.5f,
      {.motionType = PhysicsMotionType::Static, .position = {10, 0, 0}, .gravityFactor = 1e38f});
  const auto kinematic = world.createSphere(.5f,
      {.motionType = PhysicsMotionType::Kinematic, .position = {20, 0, 0}, .gravityFactor = 1e38f});
  world.step(kStep);
  expectPosition(world, stationary, {10, 0, 0});
  expectPosition(world, kinematic, {20, 0, 0});
}

TEST(PhysicsWorldTests, MovingStaticSupportWakesOldAndNewRegionsWithoutWakingBetween) {
  PhysicsWorld world(testWorldSettings());
  const auto movingFloor = world.createBox({2.0f, 0.5f, 2.0f},
      {.motionType = PhysicsMotionType::Static, .position = {0.0, -0.5, 0.0}});
  (void)world.createBox({2.0f, 0.5f, 2.0f},
      {.motionType = PhysicsMotionType::Static, .position = {50.0, -0.5, 0.0}});
  (void)world.createBox({2.0f, 0.5f, 2.0f},
      {.motionType = PhysicsMotionType::Static, .position = {100.0, -0.5, 0.0}});
  const auto oldRegion = world.createBox({0.5f, 0.5f, 0.5f},
      {.position = {0.0, 3.0, 0.0}});
  const auto betweenRegions = world.createBox({0.5f, 0.5f, 0.5f},
      {.position = {50.0, 3.0, 0.0}});
  const auto newRegion = world.createBox({0.5f, 0.5f, 0.5f},
      {.position = {100.0, 3.0, 0.0}});
  simulate(world, 300u);
  ASSERT_EQ(world.activeBodyCount(), 0u);
  expectPosition(world, oldRegion, {0.0, 0.5, 0.0}, 0.04);
  expectPosition(world, betweenRegions, {50.0, 0.5, 0.0}, 0.04);
  expectPosition(world, newRegion, {100.0, 0.5, 0.0}, 0.04);
  ASSERT_TRUE(world.pose(movingFloor));
  EXPECT_TRUE(world.setPose(movingFloor, *world.pose(movingFloor), false));
  EXPECT_EQ(world.activeBodyCount(), 0u);
  auto equivalentPose = *world.pose(movingFloor);
  equivalentPose.rotation = -equivalentPose.rotation;
  EXPECT_TRUE(world.setPose(movingFloor, equivalentPose, false));
  EXPECT_EQ(world.activeBodyCount(), 0u);

  // A teleport affects its old and new neighborhoods, not the long corridor
  // between them. The destination floor overlaps the sleeping third box.
  ASSERT_TRUE(world.setPose(movingFloor, {{100.0, 0.25, 0.0}}, false));
  simulate(world, 1u);
  EXPECT_EQ(world.activeBodyCount(), 2u);
  simulate(world, 29u);
  EXPECT_LT(world.pose(oldRegion)->position.y, 0.0);
  EXPECT_GT(world.pose(newRegion)->position.y, 0.7);
  expectPosition(world, betweenRegions, {50.0, 0.5, 0.0}, 0.04);
  EXPECT_EQ(*world.linearVelocity(betweenRegions), glm::vec3{0.0f});
}

TEST(PhysicsWorldTests, RemovingStaticSupportWakesOnlyItsSleepingNeighborhood) {
  PhysicsWorld world(testWorldSettings());
  const auto removedFloor = world.createBox({2.0f, 0.5f, 2.0f},
      {.motionType = PhysicsMotionType::Static, .position = {0.0, -0.5, 0.0}});
  (void)world.createBox({2.0f, 0.5f, 2.0f},
      {.motionType = PhysicsMotionType::Static, .position = {20.0, -0.5, 0.0}});
  const auto supported = world.createBox({0.5f, 0.5f, 0.5f},
      {.position = {0.0, 3.0, 0.0}});
  const auto distant = world.createBox({0.5f, 0.5f, 0.5f},
      {.position = {20.0, 3.0, 0.0}});
  simulate(world, 300u);
  ASSERT_EQ(world.activeBodyCount(), 0u);
  expectPosition(world, supported, {0.0, 0.5, 0.0}, 0.04);
  expectPosition(world, distant, {20.0, 0.5, 0.0}, 0.04);

  ASSERT_TRUE(world.destroyBody(removedFloor));
  EXPECT_EQ(world.bodyCount(), 3u);
  simulate(world, 1u);
  EXPECT_EQ(world.activeBodyCount(), 1u);
  simulate(world, 29u);
  EXPECT_LT(world.pose(supported)->position.y, 0.0);
  expectPosition(world, distant, {20.0, 0.5, 0.0}, 0.04);
  EXPECT_EQ(*world.linearVelocity(distant), glm::vec3{0.0f});
}

TEST(PhysicsWorldTests, GrowingStaticColliderWakesNewOverlapsOutsideItsDepartedBounds) {
  auto settings = testWorldSettings();
  settings.gravity = glm::vec3(0.0f);
  PhysicsWorld world(settings);
  const auto original = world.createBox(glm::vec3(0.5f),
      {.motionType = PhysicsMotionType::Static});
  const auto affected = world.createBox(glm::vec3(0.5f), {.position = {4.0, 0.0, 0.0}});
  const auto remote = world.createBox(glm::vec3(0.5f), {.position = {20.0, 0.0, 0.0}});
  simulate(world, 120u);
  ASSERT_EQ(world.activeBodyCount(), 0u);
  // A replacement can grow into a sleeping body beyond the departed shape's
  // wake neighborhood. Its new bounds must wake that body on insertion.
  const auto replacement = world.createBox({5.0f, 0.5f, 2.0f},
      {.motionType = PhysicsMotionType::Static});
  EXPECT_EQ(world.activeBodyCount(), 1u);
  ASSERT_TRUE(world.destroyBody(original));
  simulate(world, 60u);
  ASSERT_TRUE(world.pose(affected));
  EXPECT_GT(std::abs(world.pose(affected)->position.y), 0.9);
  expectPosition(world, replacement, {0.0, 0.0, 0.0});
  expectPosition(world, remote, {20.0, 0.0, 0.0});
  EXPECT_EQ(world.linearVelocity(remote).value(), glm::vec3(0.0f));
  EXPECT_EQ(world.angularVelocity(remote).value(), glm::vec3(0.0f));
  EXPECT_EQ(world.bodyCount(), 3u);
}

TEST(PhysicsWorldTests, TeleportingKinematicSupportWakesOnlyOldAndNewRegions) {
  PhysicsWorld world(testWorldSettings());
  const auto movingFloor = world.createBox({2.0f, 0.5f, 2.0f},
      {.motionType = PhysicsMotionType::Kinematic, .position = {0.0, -0.5, 0.0}});
  (void)world.createBox({2.0f, 0.5f, 2.0f},
      {.motionType = PhysicsMotionType::Static, .position = {50.0, -0.5, 0.0}});
  (void)world.createBox({2.0f, 0.5f, 2.0f},
      {.motionType = PhysicsMotionType::Static, .position = {100.0, -0.5, 0.0}});
  const auto oldRegion = world.createBox({0.5f, 0.5f, 0.5f},
      {.position = {0.0, 3.0, 0.0}});
  const auto betweenRegions = world.createBox({0.5f, 0.5f, 0.5f},
      {.position = {50.0, 3.0, 0.0}});
  const auto newRegion = world.createBox({0.5f, 0.5f, 0.5f},
      {.position = {100.0, 3.0, 0.0}});
  simulate(world, 300u);
  ASSERT_EQ(world.activeBodyCount(), 0u);
  expectPosition(world, oldRegion, {0.0, 0.5, 0.0}, 0.04);
  expectPosition(world, betweenRegions, {50.0, 0.5, 0.0}, 0.04);
  expectPosition(world, newRegion, {100.0, 0.5, 0.0}, 0.04);

  ASSERT_TRUE(world.pose(movingFloor));
  EXPECT_TRUE(world.setPose(movingFloor, *world.pose(movingFloor), false));
  EXPECT_EQ(world.activeBodyCount(), 0u);
  auto equivalentPose = *world.pose(movingFloor);
  equivalentPose.rotation = -equivalentPose.rotation;
  EXPECT_TRUE(world.setPose(movingFloor, equivalentPose, false));
  EXPECT_EQ(world.activeBodyCount(), 0u);

  ASSERT_TRUE(world.setPose(movingFloor, {{100.0, 0.25, 0.0}}, false));
  // Both affected dynamics and the teleported kinematic wake. The sleeping
  // body in the long corridor between the old/new bounds remains untouched.
  EXPECT_EQ(world.activeBodyCount(), 3u);
  simulate(world, 30u);
  EXPECT_LT(world.pose(oldRegion)->position.y, 0.0);
  EXPECT_GT(world.pose(newRegion)->position.y, 0.7);
  expectPosition(world, betweenRegions, {50.0, 0.5, 0.0}, 0.04);
  EXPECT_EQ(*world.linearVelocity(betweenRegions), glm::vec3{0.0f});
}

PhysicsWorldSettings jointWorldSettings() {
  auto settings = testWorldSettings();
  settings.gravity = glm::vec3{0.0f};
  return settings;
}

TEST(PhysicsJointTests, FixedJointPreservesInitialRelativePoseAgainstGravity) {
  PhysicsWorld world(testWorldSettings());
  const auto base = world.createBox(glm::vec3{0.25f},
      {.motionType = PhysicsMotionType::Static, .position = {0.0, 4.0, 0.0},
       .rotation = glm::angleAxis(0.4f, glm::vec3{0, 0, 1})});
  const PhysicsBodySettings movingSettings{
      .position = {2.0, 4.0, 0.0},
      .rotation = glm::angleAxis(-0.3f, glm::vec3{0, 1, 0})};
  const auto moving = world.createBox(glm::vec3{0.25f}, movingSettings);
  PhysicsJointSettings settings;
  settings.type = PhysicsJointType::Fixed;
  settings.anchor = {1.0, 4.0, 0.0};
  const auto joint = world.createJoint(base, moving, settings);
  ASSERT_NE(joint, kInvalidPhysicsJointId);
  EXPECT_EQ(world.jointCount(), 1u);
  simulate(world, 120u);
  expectPosition(world, moving, movingSettings.position, 0.01);
  EXPECT_NEAR(std::abs(glm::dot(world.pose(moving)->rotation,
                                movingSettings.rotation)), 1.0f, 1e-4f);
  const auto state = world.jointState(joint);
  ASSERT_TRUE(state);
  EXPECT_EQ(state->type, PhysicsJointType::Fixed);
  EXPECT_EQ(state->bodyA, base);
  EXPECT_EQ(state->bodyB, moving);
  EXPECT_FLOAT_EQ(state->coordinate, 0.0f);
}

TEST(PhysicsJointTests, WorldJointsCorrectCentimeterEditsAtGeoreferencedCoordinates) {
  for (const auto type : {PhysicsJointType::Fixed, PhysicsJointType::Hinge, PhysicsJointType::Slider}) {
    for (const double origin : {0.0, 1'000'000.0, -1'000'000.0}) {
      SCOPED_TRACE(int(type));
      SCOPED_TRACE(origin);
      auto settings = jointWorldSettings();
      settings.maxBodies = 1;
      PhysicsWorld world(settings);
      settings.maxBodies = 2;
      PhysicsWorld control(settings);
      const glm::dvec3 authored{origin + .01, 3, -2};
      const auto body = world.createBox(glm::vec3{.25f}, {.position = authored});
      const auto support = control.createBox(glm::vec3{.25f},
          {.motionType = PhysicsMotionType::Static, .position = authored});
      const auto reference = control.createBox(glm::vec3{.25f}, {.position = authored});
      PhysicsJointSettings joint;
      joint.type = type;
      joint.anchor = authored;
      joint.axis = {0, 1, 0}; // X is constrained for all three joint types.
      joint.normal = {1, 0, 0};
      auto handle = world.createJoint(kInvalidPhysicsBodyId, body, joint);
      (void)control.createJoint(support, reference, joint);
      for (int round = 0; round < 2; ++round) {
        SCOPED_TRACE(round);
        const auto state = *world.jointState(handle);
        EXPECT_EQ(state.bodyA, kInvalidPhysicsBodyId);
        EXPECT_EQ(state.bodyB, body);
        EXPECT_EQ(state.frameA.anchor, authored);
        EXPECT_EQ(state.frameB.anchor, glm::dvec3{0});
        EXPECT_EQ(state.settings.anchor, authored);
        auto edit = *world.pose(body);
        edit.position.x += .01;
        ASSERT_TRUE(world.setPose(body, edit));
        edit = *control.pose(reference);
        edit.position.x += .01;
        ASSERT_TRUE(control.setPose(reference, edit));
        simulate(world, 120u);
        simulate(control, 120u);
        expectPosition(world, body, authored, 1e-5);
        expectPosition(world, body, control.pose(reference)->position, 1e-5);
        EXPECT_EQ(world.bodyCount(), 1u);
        EXPECT_EQ(world.jointCount(), 1u);
        if (round == 0) {
          ASSERT_TRUE(world.destroyJoint(handle));
          handle = world.createJointLocal(kInvalidPhysicsBodyId, body, state.settings,
                                          state.frameA, state.frameB);
        }
      }
    }
  }
}

TEST(PhysicsJointTests, WorldAnchorOwnershipDoesNotConsumeBodySlotsAndRetiresSafely) {
  auto settings = jointWorldSettings();
  settings.maxBodies = 1;
  PhysicsWorld world(settings);
  for (int round = 0; round < 3; ++round) {
    SCOPED_TRACE(round);
    const glm::dvec3 authored{1'000'000.01 + 100 * round, 3, -1'000'000.02};
    const auto body = world.createSphere(.25f, {.position = authored});
    PhysicsJointSettings joint;
    joint.type = PhysicsJointType::Fixed;
    joint.anchor = authored;
    const auto first = world.createJoint(kInvalidPhysicsBodyId, body, joint);
    const auto state = *world.jointState(first);
    const auto second = world.createJointLocal(kInvalidPhysicsBodyId, body, state.settings,
                                              state.frameA, state.frameB);
    EXPECT_EQ(world.bodyCount(), 1u);
    EXPECT_EQ(world.jointCount(), 2u);
    ASSERT_TRUE(world.destroyJoint(first));
    ASSERT_FALSE(world.containsJoint(first));
    ASSERT_TRUE(world.containsJoint(second));
    auto edit = *world.pose(body);
    edit.position.x += .01;
    ASSERT_TRUE(world.setPose(body, edit));
    simulate(world, 120u);
    expectPosition(world, body, authored, 1e-5);
    if (round == 0)
      ASSERT_TRUE(world.destroyBody(body));
    else
      world.clear();
    EXPECT_EQ(world.bodyCount(), 0u);
    EXPECT_EQ(world.jointCount(), 0u);
    EXPECT_FALSE(world.contains(body));
    EXPECT_FALSE(world.containsJoint(second));
  }
}

TEST(PhysicsJointTests, HingeWorldMotorSupportsVelocityAndPositionTargets) {
  PhysicsWorld world(jointWorldSettings());
  const auto body = world.createBox(glm::vec3{0.5f}, {.position = {0.0, 3.0, 0.0}});
  PhysicsJointSettings settings;
  settings.anchor = {0.0, 3.0, 0.0};
  settings.motor.mode = PhysicsJointMotorMode::Velocity;
  settings.motor.targetVelocity = 1.0f;
  const auto joint = world.createJoint(kInvalidPhysicsBodyId, body, settings);
  simulate(world, 30u);
  ASSERT_TRUE(world.jointState(joint));
  EXPECT_GT(world.jointState(joint)->coordinate, 0.3f);
  EXPECT_LT(world.jointState(joint)->coordinate, 0.6f);
  expectPosition(world, body, {0.0, 3.0, 0.0}, 1e-3);
  auto motor = settings.motor;
  motor.mode = PhysicsJointMotorMode::Position;
  motor.targetPosition = 0.2f;
  motor.frequencyHz = 4.0f;
  EXPECT_TRUE(world.setJointMotor(joint, motor));
  simulate(world, 120u);
  EXPECT_NEAR(world.jointState(joint)->coordinate, 0.2f, 0.025f);
  EXPECT_TRUE(world.containsJoint(joint));
  EXPECT_EQ(world.jointCount(), 1u);
}

TEST(PhysicsJointTests, HingeLimitsAndRuntimeMotorEditsRetainTheZeroFrame) {
  PhysicsWorld world(jointWorldSettings());
  const auto body = world.createBox(glm::vec3{0.5f}, {.position = {0.0, 3.0, 0.0}});
  PhysicsJointSettings settings;
  settings.anchor = {0.0, 3.0, 0.0};
  settings.limitsEnabled = true;
  settings.lowerLimit = -0.25f;
  settings.upperLimit = 0.4f;
  settings.motor.mode = PhysicsJointMotorMode::Velocity;
  settings.motor.targetVelocity = 3.0f;
  const auto joint = world.createJoint(0u, body, settings);
  simulate(world, 90u);
  EXPECT_NEAR(world.jointState(joint)->coordinate, 0.4f, 0.025f);
  EXPECT_TRUE(world.setJointLimits(joint, true, -0.2f, 0.2f));
  settings.motor.targetVelocity = -3.0f;
  EXPECT_TRUE(world.setJointMotor(joint, settings.motor));
  simulate(world, 90u);
  EXPECT_NEAR(world.jointState(joint)->coordinate, -0.2f, 0.025f);
  EXPECT_TRUE(world.setJointLimits(joint, false, -1.0f, 1.0f));
  settings.motor.targetVelocity = 1.0f;
  EXPECT_TRUE(world.setJointMotor(joint, settings.motor));
  simulate(world, 30u);
  EXPECT_GT(world.jointState(joint)->coordinate, 0.2f);
}

TEST(PhysicsJointTests, SliderRestrictsOtherAxesAndSupportsLimitsAndPositionMotor) {
  PhysicsWorld world(testWorldSettings());
  const auto body = world.createBox(glm::vec3{0.5f}, {.position = {0.0, 3.0, 0.0}});
  PhysicsJointSettings settings;
  settings.type = PhysicsJointType::Slider;
  settings.anchor = {0.0, 3.0, 0.0};
  settings.axis = {1, 0, 0};
  settings.normal = {0, 1, 0};
  settings.limitsEnabled = true;
  settings.lowerLimit = -0.5f;
  settings.upperLimit = 1.0f;
  settings.motor.mode = PhysicsJointMotorMode::Velocity;
  settings.motor.targetVelocity = 2.0f;
  const auto joint = world.createJoint(0u, body, settings);
  simulate(world, 90u);
  EXPECT_NEAR(world.jointState(joint)->coordinate, 1.0f, 0.025f);
  EXPECT_NEAR(world.pose(body)->position.y, 3.0, 1e-3);
  EXPECT_NEAR(world.pose(body)->position.z, 0.0, 1e-3);
  EXPECT_NEAR(std::abs(world.pose(body)->rotation.w), 1.0f, 1e-4f);
  auto motor = settings.motor;
  motor.mode = PhysicsJointMotorMode::Position;
  motor.targetPosition = 0.25f;
  motor.frequencyHz = 4.0f;
  EXPECT_TRUE(world.setJointMotor(joint, motor));
  simulate(world, 120u);
  EXPECT_NEAR(world.jointState(joint)->coordinate, 0.25f, 0.025f);
  EXPECT_TRUE(world.setJointLimits(joint, true, -0.1f, 0.1f));
  EXPECT_FLOAT_EQ(world.jointState(joint)->settings.motor.targetPosition, 0.1f);
  simulate(world, 90u);
  EXPECT_NEAR(world.jointState(joint)->coordinate, 0.1f, 0.025f);
}

TEST(PhysicsJointTests, CanonicalLocalFramesSurviveBodyRecreationAndIgnoreWorldFields) {
  PhysicsWorld world(jointWorldSettings());
  const PhysicsBodySettings baseSettings{
      .motionType = PhysicsMotionType::Static, .position = {3, 4, 5},
      .rotation = glm::angleAxis(0.7f, glm::vec3{0, 0, 1})};
  const PhysicsBodySettings bodySettings{
      .position = {4, 4, 5}, .rotation = glm::angleAxis(-0.3f, glm::vec3{1, 0, 0})};
  const auto base = world.createBox(glm::vec3{0.25f}, baseSettings);
  const auto body = world.createBox(glm::vec3{0.25f}, bodySettings);
  PhysicsJointSettings settings;
  settings.anchor = {3.5, 4, 5};
  settings.axis = {0, 2, 0};
  settings.normal = {3, 0, 0};
  const auto original = world.createJoint(base, body, settings);
  const auto saved = *world.jointState(original);
  const glm::dvec3 expectedLocal = glm::inverse(glm::dquat{baseSettings.rotation}) *
                                 (settings.anchor - baseSettings.position);
  EXPECT_NEAR(glm::length(saved.frameA.anchor - expectedLocal), 0.0, 1e-6);
  EXPECT_NEAR(glm::length(saved.frameA.axis), 1.0f, 1e-6f);
  EXPECT_NEAR(glm::dot(saved.frameA.axis, saved.frameA.normal), 0.0f, 1e-6f);
  EXPECT_TRUE(world.destroyBody(body));
  EXPECT_FALSE(world.containsJoint(original));
  const auto recreatedBody = world.createBox(glm::vec3{0.25f}, bodySettings);
  settings.anchor.x = std::numeric_limits<double>::quiet_NaN();
  settings.axis = glm::vec3{0.0f};
  const auto recreated = world.createJointLocal(base, recreatedBody, settings,
                                                saved.frameA, saved.frameB);
  ASSERT_TRUE(world.jointState(recreated));
  EXPECT_NE(recreated, original);
  EXPECT_NEAR(world.jointState(recreated)->coordinate, 0.0f, 1e-5f);
  simulate(world, 60u);
  expectPosition(world, recreatedBody, bodySettings.position, 1e-3);
  EXPECT_NEAR(std::abs(glm::dot(world.pose(recreatedBody)->rotation,
                                bodySettings.rotation)), 1.0f, 1e-4f);
}

TEST(PhysicsJointTests, DestroyingBodyRemovesItsJointsAndClearNeverReusesHandles) {
  PhysicsWorld world(jointWorldSettings());
  const auto first = world.createBox(glm::vec3{0.25f}, {.position = {0, 3, 0}});
  const auto second = world.createBox(glm::vec3{0.25f}, {.position = {2, 3, 0}});
  const auto third = world.createBox(glm::vec3{0.25f}, {.position = {4, 3, 0}});
  PhysicsJointSettings settings;
  settings.anchor = {0, 3, 0};
  const auto worldJoint = world.createJoint(0u, first, settings);
  settings.type = PhysicsJointType::Fixed;
  const auto connected = world.createJoint(first, second, settings);
  const auto unrelated = world.createJoint(0u, third, settings);
  EXPECT_TRUE(world.destroyBody(first));
  EXPECT_EQ(world.jointCount(), 1u);
  EXPECT_FALSE(world.containsJoint(worldJoint));
  EXPECT_FALSE(world.containsJoint(connected));
  EXPECT_TRUE(world.containsJoint(unrelated));
  EXPECT_FALSE(world.destroyJoint(worldJoint));
  world.clear();
  EXPECT_EQ(world.bodyCount(), 0u);
  EXPECT_EQ(world.jointCount(), 0u);
  EXPECT_FALSE(world.jointState(unrelated));
  const auto replacementBody = world.createBox(glm::vec3{0.25f});
  const auto replacement = world.createJoint(0u, replacementBody, settings);
  EXPECT_NE(replacement, unrelated);
  EXPECT_FALSE(world.containsJoint(unrelated));
}

TEST(PhysicsJointTests, InvalidEndpointsFramesLimitsAndMotorsDoNotModifyLiveJoints) {
  PhysicsWorld world(jointWorldSettings()), other(jointWorldSettings());
  const auto body = world.createBox(glm::vec3{0.25f});
  const auto fixed = world.createBox(glm::vec3{0.25f},
      {.motionType = PhysicsMotionType::Static, .position = {3, 0, 0}});
  const auto foreign = other.createBox(glm::vec3{0.25f});
  PhysicsJointSettings settings;
  EXPECT_THROW((void)world.createJoint(body, body, settings), std::invalid_argument);
  EXPECT_THROW((void)world.createJoint(body, 0u, settings), std::invalid_argument);
  EXPECT_THROW((void)world.createJoint(foreign, body, settings), std::invalid_argument);
  EXPECT_THROW((void)world.createJoint(0u, fixed, settings), std::invalid_argument);
  std::vector<PhysicsJointSettings> invalid(10u);
  invalid[0].axis = glm::vec3{0.0f};
  invalid[1].normal = invalid[1].axis;
  invalid[2].anchor.x = std::numeric_limits<double>::infinity();
  invalid[3].limitsEnabled = true;
  invalid[3].lowerLimit = 0.1f;
  invalid[4].limitsEnabled = true;
  invalid[4].upperLimit = 4.0f;
  invalid[5].motor.maxForceOrTorque = -1.0f;
  invalid[6].motor.mode = PhysicsJointMotorMode::Position;
  invalid[6].motor.frequencyHz = 0.0f;
  invalid[7].type = static_cast<PhysicsJointType>(255u);
  invalid[8].limitsEnabled = true;
  invalid[8].lowerLimit = invalid[8].upperLimit = 0.0f;
  invalid[9] = invalid[8];
  invalid[9].type = PhysicsJointType::Slider;
  for (const auto &entry : invalid)
    EXPECT_THROW((void)world.createJoint(0u, body, entry), std::invalid_argument);
  EXPECT_EQ(world.jointCount(), 0u);
  const auto valid = world.createJoint(0u, body, settings);
  auto motor = settings.motor;
  motor.dampingRatio = -1.0f;
  EXPECT_THROW((void)world.setJointMotor(valid, motor), std::invalid_argument);
  EXPECT_THROW((void)world.setJointLimits(valid, true, 0.2f, 0.3f), std::invalid_argument);
  EXPECT_THROW((void)world.setJointLimits(valid, true, 0.0f, 0.0f), std::invalid_argument);
  EXPECT_EQ(world.jointCount(), 1u);
  EXPECT_EQ(world.jointState(valid)->settings.motor.mode, PhysicsJointMotorMode::Off);
  EXPECT_FALSE(world.jointState(valid)->settings.limitsEnabled);
  EXPECT_FALSE(other.containsJoint(valid));
  EXPECT_FALSE(other.destroyJoint(valid));
  EXPECT_FALSE(other.setJointMotor(valid, {}));
  EXPECT_FALSE(other.setJointLimits(valid, true, -1, 1));
}

TEST(PhysicsJointTests, UnrepresentablePositionSpringCoefficientsFailBeforeJointMutation) {
  auto settings = testWorldSettings();
  settings.gravity = glm::vec3(0.0f);
  PhysicsWorld world(settings);
  const auto body = world.createBox(glm::vec3(0.5f));
  PhysicsJointSettings definition;
  definition.type = PhysicsJointType::Slider;
  definition.axis = {1, 0, 0};
  definition.normal = {0, 1, 0};
  const auto joint = world.createJoint(kInvalidPhysicsBodyId, body, definition);
  simulate(world, 120u);
  ASSERT_EQ(world.activeBodyCount(), 0u);
  const auto before = world.pose(body).value();
  std::array<PhysicsJointMotorSettings, 4> invalid;
  for (auto &motor : invalid) {
    motor.mode = PhysicsJointMotorMode::Position;
    motor.targetPosition = 1.0f;
  }
  invalid[0].frequencyHz = 1e-25f;
  invalid[0].dampingRatio = 0.0f;
  invalid[1].frequencyHz = 1e20f;
  invalid[2].dampingRatio = std::numeric_limits<float>::max();
  invalid[3].frequencyHz = 1e-25f;
  for (size_t index = 0; index < invalid.size(); ++index) {
    SCOPED_TRACE(index);
    ASSERT_THROW((void)world.setJointMotor(joint, invalid[index]), std::invalid_argument);
    EXPECT_EQ(world.activeBodyCount(), 0u);
    ASSERT_TRUE(world.jointState(joint));
    EXPECT_EQ(world.jointState(joint)->settings.motor.mode, PhysicsJointMotorMode::Off);
    EXPECT_FLOAT_EQ(world.jointState(joint)->settings.motor.frequencyHz, 2.0f);
    expectPosition(world, body, before.position);
    auto candidateDefinition = definition;
    candidateDefinition.motor = invalid[index];
    ASSERT_THROW((void)world.createJoint(kInvalidPhysicsBodyId, body, candidateDefinition), std::invalid_argument);
    EXPECT_EQ(world.jointCount(), 1u);
    EXPECT_EQ(world.activeBodyCount(), 0u);
  }
  ASSERT_TRUE(world.setJointMotor(joint,
      {.mode = PhysicsJointMotorMode::Position, .targetPosition = 0.5f}));
  simulate(world, 120u);
  EXPECT_NEAR(world.jointState(joint)->coordinate, 0.5f, 0.03f);
}

TEST(PhysicsJointTests, OffAndVelocityMotorsDoNotEvaluateUnusedPositionSpringCoefficients) {
  for (const auto mode : {PhysicsJointMotorMode::Off, PhysicsJointMotorMode::Velocity}) {
    for (const float frequency : {1e-25f, 1e20f}) {
      SCOPED_TRACE(::testing::Message() << "mode=" << int(mode) << " frequency=" << frequency);
      auto settings = testWorldSettings();
      settings.gravity = glm::vec3(0.0f);
      PhysicsWorld world(settings);
      const auto body = world.createBox(glm::vec3(0.5f));
      PhysicsJointSettings definition;
      definition.type = PhysicsJointType::Slider;
      definition.axis = {1, 0, 0};
      definition.normal = {0, 1, 0};
      definition.motor = {.mode = mode, .targetVelocity = 0.5f,
                          .frequencyHz = frequency, .dampingRatio = 0.0f};
      const auto joint = world.createJoint(kInvalidPhysicsBodyId, body, definition);
      ASSERT_TRUE(world.jointState(joint));
      EXPECT_FLOAT_EQ(world.jointState(joint)->settings.motor.frequencyHz, frequency);
      EXPECT_TRUE(world.setJointMotor(joint, definition.motor));
      simulate(world, 10u);
      if (mode == PhysicsJointMotorMode::Off)
        expectPosition(world, body, {});
      else
        EXPECT_GT(world.pose(body)->position.x, 0.05);
    }
  }
}

TEST(PhysicsJointTests, HugeUnlimitedHingePositionTargetsNormalizeBeforeSimulation) {
  constexpr float pi = 3.14159265358979323846f;
  for (const float target : {1e10f, -1e10f, std::numeric_limits<float>::max(),
                             -std::numeric_limits<float>::max()}) {
    SCOPED_TRACE(target);
    PhysicsWorld world(jointWorldSettings());
    const auto body = world.createBox(glm::vec3{0.5f});
    PhysicsJointSettings settings;
    settings.motor.mode = PhysicsJointMotorMode::Position;
    settings.motor.targetPosition = target;
    const auto joint = world.createJoint(kInvalidPhysicsBodyId, body, settings);
    ASSERT_TRUE(world.jointState(joint));
    const float canonical = world.jointState(joint)->settings.motor.targetPosition;
    // Fail before step() on the unfixed code: its backend angle-reduction loop
    // never terminates for these values, so a normal EXPECT would hang tests.
    ASSERT_LE(std::abs(canonical), pi);
    EXPECT_FLOAT_EQ(canonical, std::remainder(target, 2.0f * pi));
    simulate(world, 120u);
    EXPECT_NEAR(world.jointState(joint)->coordinate, canonical, .025f);
  }
}

TEST(PhysicsJointTests, HingeMotorEditsPreserveDraftTargetsAndClampEnabledLimits) {
  constexpr float pi = 3.14159265358979323846f;
  PhysicsWorld world(jointWorldSettings());
  const auto body = world.createBox(glm::vec3{0.5f});
  PhysicsJointSettings settings;
  settings.motor.targetPosition = 1e10f;
  const auto joint = world.createJoint(kInvalidPhysicsBodyId, body, settings);
  EXPECT_FLOAT_EQ(world.jointState(joint)->settings.motor.targetPosition, 1e10f);
  auto motor = settings.motor;
  motor.mode = PhysicsJointMotorMode::Velocity;
  motor.targetVelocity = .5f;
  ASSERT_TRUE(world.setJointMotor(joint, motor));
  EXPECT_FLOAT_EQ(world.jointState(joint)->settings.motor.targetPosition, 1e10f);
  simulate(world, 5u);
  motor.mode = PhysicsJointMotorMode::Position;
  motor.targetPosition = -1e10f;
  ASSERT_TRUE(world.setJointMotor(joint, motor));
  const float canonical = world.jointState(joint)->settings.motor.targetPosition;
  ASSERT_LE(std::abs(canonical), pi);
  EXPECT_FLOAT_EQ(canonical, std::remainder(motor.targetPosition, 2.0f * pi));
  simulate(world, 120u);
  EXPECT_NEAR(world.jointState(joint)->coordinate, canonical, .025f);
  ASSERT_TRUE(world.setJointLimits(joint, true, -.25f, .5f));
  for (const float target : {1e10f, -1e10f}) {
    motor.targetPosition = target;
    ASSERT_TRUE(world.setJointMotor(joint, motor));
    const float clamped = target > 0.0f ? .5f : -.25f;
    EXPECT_FLOAT_EQ(world.jointState(joint)->settings.motor.targetPosition, clamped);
    simulate(world, 120u);
    EXPECT_NEAR(world.jointState(joint)->coordinate, clamped, .025f);
  }
  settings.type = PhysicsJointType::Slider;
  settings.motor.mode = PhysicsJointMotorMode::Position;
  EXPECT_FLOAT_EQ(PhysicsWorld::validateJointSettings(settings).motor.targetPosition, 1e10f);
}

TEST(PhysicsJointTests, CollisionSuppressionRemainsUntilLastJointBetweenPairIsRemoved) {
  PhysicsWorld world(jointWorldSettings());
  const auto base = world.createBox(glm::vec3{0.5f},
      {.motionType = PhysicsMotionType::Static, .position = {0, 3, 0}});
  const auto body = world.createBox(glm::vec3{0.5f}, {.position = {0, 3, 0}});
  PhysicsJointSettings settings;
  settings.type = PhysicsJointType::Slider;
  settings.anchor = {0, 3, 0};
  const auto first = world.createJoint(base, body, settings);
  const auto second = world.createJoint(base, body, settings);
  simulate(world, 30u);
  expectPosition(world, body, {0, 3, 0}, 1e-3);
  EXPECT_TRUE(world.destroyJoint(first));
  simulate(world, 30u);
  expectPosition(world, body, {0, 3, 0}, 1e-3);
  EXPECT_TRUE(world.destroyJoint(second));
  simulate(world, 30u);
  EXPECT_GT(glm::length(world.pose(body)->position - glm::dvec3{0, 3, 0}), 0.5);
}

TEST(PhysicsJointTests, CollisionPolicyCanRetainContactsBetweenConnectedBodies) {
  for (const bool collide : {false, true}) {
    PhysicsWorld world(testWorldSettings());
    const auto floor = addFloor(world);
    const auto body = world.createBox(glm::vec3{0.5f}, {.position = {0, 3, 0}});
    PhysicsJointSettings settings;
    settings.type = PhysicsJointType::Slider;
    settings.anchor = {0, 3, 0};
    settings.collideConnected = collide;
    (void)world.createJoint(floor, body, settings);
    simulate(world, 120u);
    if (collide)
      EXPECT_NEAR(world.pose(body)->position.y, 0.5, 0.04);
    else
      EXPECT_LT(world.pose(body)->position.y, -1.0);
  }
}

TEST(PhysicsJointTests, FilterChangesInvalidateSleepingContactCacheAndRestoreCollisions) {
  PhysicsWorld world(testWorldSettings());
  const auto floor = addFloor(world);
  const auto body = world.createBox(glm::vec3{0.5f}, {.position = {0, 3, 0}});
  simulate(world, 300u);
  ASSERT_EQ(world.activeBodyCount(), 0u);
  PhysicsJointSettings settings;
  settings.type = PhysicsJointType::Slider;
  settings.anchor = world.pose(body)->position;
  const auto joint = world.createJoint(floor, body, settings);
  simulate(world, 30u);
  EXPECT_LT(world.pose(body)->position.y, 0.0);
  EXPECT_TRUE(world.destroyJoint(joint));
  ASSERT_TRUE(world.setPose(body, {{0, 3, 0}}));
  ASSERT_TRUE(world.setLinearVelocity(body, glm::vec3{0.0f}));
  simulate(world, 240u);
  EXPECT_NEAR(world.pose(body)->position.y, 0.5, 0.04);
  EXPECT_EQ(world.activeBodyCount(), 0u);
}

TEST(PhysicsJointTests, ThreeDynamicLinksMaintainSharedHingeAnchorsWhileSwingingUnderGravity) {
  PhysicsWorld world(testWorldSettings());
  const auto first = world.createBox({0.1f, 0.5f, 0.1f}, {.position = {0, 5, 0}});
  const auto second = world.createBox({0.1f, 0.5f, 0.1f}, {.position = {0, 4, 0}});
  const auto third = world.createBox({0.1f, 0.5f, 0.1f},
      {.position = {0, 3, 0}, .linearVelocity = {2, 0, 0}});
  PhysicsJointSettings settings;
  settings.axis = {0, 0, 1};
  settings.normal = {1, 0, 0};
  settings.anchor = {0, 5.5, 0};
  const auto pin = world.createJoint(0u, first, settings);
  settings.anchor = {0, 4.5, 0};
  const auto upper = world.createJoint(first, second, settings);
  settings.anchor = {0, 3.5, 0};
  const auto lower = world.createJoint(second, third, settings);
  const std::array joints{pin, upper, lower};
  const std::array bodies{first, second, third};
  ASSERT_EQ(world.jointCount(), 3u);
  // Only directly connected neighbors are suppressed. Link one and link
  // three retain the world's normal collision policy; no group-wide filter
  // disables all contacts within the articulated chain.
  for (const auto joint : joints)
    ASSERT_FALSE(world.jointState(joint)->settings.collideConnected);

  double maximumAnchorError = 0.0;
  double maximumLateralMotion = 0.0;
  const auto worldAnchor = [&](PhysicsBodyId body, const PhysicsJointFrame &frame) {
    if (body == kInvalidPhysicsBodyId)
      return frame.anchor;
    const auto pose = *world.pose(body);
    // Box shapes have their center of mass at the body origin.
    return pose.position + glm::dquat{pose.rotation} * frame.anchor;
  };
  for (unsigned frame = 0; frame < 240u; ++frame) {
    world.step(kStep);
    for (const auto body : bodies) {
      const auto pose = world.pose(body);
      ASSERT_TRUE(pose);
      for (int axis = 0; axis < 3; ++axis)
        ASSERT_TRUE(std::isfinite(pose->position[axis]));
      for (int component = 0; component < 4; ++component)
        ASSERT_TRUE(std::isfinite(pose->rotation[component]));
    }
    maximumLateralMotion = std::max(maximumLateralMotion,
                                    std::abs(world.pose(third)->position.x));
    for (const auto joint : joints) {
      const auto state = world.jointState(joint);
      ASSERT_TRUE(state);
      const auto anchorA = worldAnchor(state->bodyA, state->frameA);
      const auto anchorB = worldAnchor(state->bodyB, state->frameB);
      maximumAnchorError = std::max(maximumAnchorError, glm::length(anchorB - anchorA));
    }
  }
  EXPECT_GT(maximumLateralMotion, 0.15);
  EXPECT_LT(maximumAnchorError, 0.025);
  EXPECT_EQ(world.bodyCount(), 3u);
  EXPECT_EQ(world.jointCount(), 3u);
}

TEST(PhysicsJointTests, MovingStaticEndpointWakesDistantJointedBodyButNotUnconnectedSleepers) {
  PhysicsWorld world(testWorldSettings());
  const auto base = world.createBox(glm::vec3{0.25f},
      {.motionType = PhysicsMotionType::Static, .position = {0, 5, 0}});
  const auto connected = world.createBox(glm::vec3{0.25f}, {.position = {3, 5, 0}});
  const auto unconnected = world.createBox(glm::vec3{0.25f},
      {.position = {30, 5, 0}, .gravityFactor = 0.0f});
  PhysicsJointSettings settings;
  settings.type = PhysicsJointType::Fixed;
  settings.anchor = {1.5, 5, 0};
  const auto joint = world.createJoint(base, connected, settings);
  simulate(world, 300u);
  ASSERT_EQ(world.activeBodyCount(), 0u);
  expectPosition(world, connected, {3, 5, 0}, 0.03);

  const auto original = *world.pose(base);
  ASSERT_TRUE(world.setPose(base, original, false));
  EXPECT_EQ(world.activeBodyCount(), 0u);
  auto equivalent = original;
  equivalent.rotation = -equivalent.rotation;
  ASSERT_TRUE(world.setPose(base, equivalent, false));
  EXPECT_EQ(world.activeBodyCount(), 0u);

  // A two-unit edit leaves a half-unit gap between the new support AABB and
  // the sleeping connected body's AABB, so broad-phase wakeup cannot help.
  ASSERT_TRUE(world.setPose(base, {{2, 5, 0}}, false));
  // The connected body lies outside both old/new support shape AABBs. Only
  // joint adjacency can wake it; the far unconnected body must stay asleep.
  EXPECT_EQ(world.activeBodyCount(), 1u);
  simulate(world, 60u);
  expectPosition(world, connected, {5, 5, 0}, 0.03);
  const auto state = *world.jointState(joint);
  const auto basePose = *world.pose(base);
  const auto connectedPose = *world.pose(connected);
  const auto anchorA = basePose.position + glm::dquat{basePose.rotation} * state.frameA.anchor;
  const auto anchorB = connectedPose.position + glm::dquat{connectedPose.rotation} * state.frameB.anchor;
  EXPECT_LT(glm::length(anchorA - anchorB), 0.02);
  expectPosition(world, unconnected, {30, 5, 0});
  EXPECT_EQ(*world.linearVelocity(unconnected), glm::vec3{0.0f});
  EXPECT_TRUE(world.containsJoint(joint));
}

TEST(PhysicsJointTests, MotorAndLimitEditsWakeTheDynamicMechanismWithoutCrossingItsSupport) {
  for (const auto [supportMotion, movingSupport] : {
      std::pair{PhysicsMotionType::Static, false},
      std::pair{PhysicsMotionType::Kinematic, false},
      std::pair{PhysicsMotionType::Kinematic, true}}) {
    for (const bool motorEdit : {false, true}) {
      SCOPED_TRACE(::testing::Message() << "support=" << int(supportMotion) << " moving=" << movingSupport << " motor=" << motorEdit);
      PhysicsWorld actual(jointWorldSettings()), control(jointWorldSettings());
      struct Mechanism {
        std::array<PhysicsBodyId, 3> links{};
        PhysicsBodyId remote{}, support{};
        PhysicsJointId actuator{}, lastJoint{};
      };
      const auto populate = [&](PhysicsWorld &world) {
        Mechanism result;
        const auto support = world.createBox(glm::vec3{.5f},
            {.motionType = supportMotion, .position = {-2, 0, 0}});
        result.support = support;
        for (size_t index = 0; index < result.links.size(); ++index)
          result.links[index] = world.createBox(glm::vec3{.5f},
              {.position = {1.2 * index, 0, 0}});
        result.remote = world.createBox(glm::vec3{.5f}, {.position = {30, 0, 0}});
        PhysicsJointSettings settings;
        settings.axis = {0, 0, 1};
        settings.normal = {1, 0, 0};
        result.actuator = world.createJoint(support, result.links[0], settings);
        settings.type = PhysicsJointType::Fixed;
        settings.anchor = {.6, 0, 0};
        (void)world.createJoint(result.links[0], result.links[1], settings);
        settings.anchor = {1.8, 0, 0};
        result.lastJoint = world.createJoint(result.links[1], result.links[2], settings);
        // A shared nondynamic support does not join these two solver islands.
        settings.anchor = {15, 0, 0};
        (void)world.createJoint(support, result.remote, settings);
        return result;
      };
      const auto mechanism = populate(actual), reference = populate(control);
      simulate(actual, 180u);
      simulate(control, 180u);
      ASSERT_EQ(actual.activeBodyCount(), 0u);
      ASSERT_EQ(control.activeBodyCount(), 0u);
      if (movingSupport) {
        ASSERT_TRUE(actual.setLinearVelocity(mechanism.support, {.1f, 0, 0}));
        ASSERT_TRUE(control.setLinearVelocity(reference.support, {.1f, 0, 0}));
      }
      if (motorEdit) {
        PhysicsJointMotorSettings motor{.mode = PhysicsJointMotorMode::Velocity, .targetVelocity = 1};
        ASSERT_TRUE(actual.setJointMotor(mechanism.actuator, motor));
        ASSERT_TRUE(control.setJointMotor(reference.actuator, motor));
      } else {
        ASSERT_TRUE(actual.setJointLimits(mechanism.actuator, true, -.25f, .25f));
        ASSERT_TRUE(control.setJointLimits(reference.actuator, true, -.25f, .25f));
      }
      // Match the expected solve against an independently fully activated
      // control; endpoint-only waking omits the far joint on its first tick.
      for (const auto link : reference.links)
        ASSERT_TRUE(control.setPose(link, *control.pose(link), true));
      // Explicitly starting the kinematic driver's velocity also wakes its
      // other direct branch. The later joint edit must preserve that state.
      const size_t expectedActive = 3u + (movingSupport ? 2u : 0u);
      EXPECT_EQ(actual.activeBodyCount(), expectedActive);
      actual.step(kStep);
      control.step(kStep);
      for (size_t index = 0; index < mechanism.links.size(); ++index) {
        expectPosition(actual, mechanism.links[index], control.pose(reference.links[index])->position, 1e-6);
        EXPECT_NEAR(glm::length(*actual.angularVelocity(mechanism.links[index]) -
                                *control.angularVelocity(reference.links[index])), 0, 1e-5);
      }
      const auto gap = [](const PhysicsWorld &world, PhysicsJointId id) {
        const auto state = *world.jointState(id);
        const auto first = *world.pose(state.bodyA), second = *world.pose(state.bodyB);
        return glm::length(first.position + glm::dquat{first.rotation} * state.frameA.anchor -
                           second.position - glm::dquat{second.rotation} * state.frameB.anchor);
      };
      EXPECT_LT(gap(actual, mechanism.lastJoint), .002);
      if (motorEdit)
        EXPECT_GT(actual.pose(mechanism.links.back())->position.y, .001);
      if (movingSupport) {
        // Joint edits must retain an already-moving kinematic driver and the
        // direct branch already activated by its explicit velocity change.
        EXPECT_EQ(*actual.linearVelocity(mechanism.support), (glm::vec3{.1f, 0, 0}));
        expectPosition(actual, mechanism.support, {-2 + .1 * kStep, 0, 0}, 1e-6);
        expectPosition(actual, mechanism.remote, control.pose(reference.remote)->position, 1e-6);
        EXPECT_EQ(actual.activeBodyCount(), expectedActive);
      } else {
        expectPosition(actual, mechanism.remote, {30, 0, 0});
        EXPECT_EQ(*actual.linearVelocity(mechanism.remote), glm::vec3{0});
        EXPECT_EQ(actual.activeBodyCount(), expectedActive);
      }
    }
  }
}

TEST(PhysicsJointTests, MovingSupportWakesEveryDynamicLinkBeforeTheFirstConstraintSnapshot) {
  for (const auto [motion, kinematicTarget] : {
      std::pair{PhysicsMotionType::Static, false},
      std::pair{PhysicsMotionType::Kinematic, false},
      std::pair{PhysicsMotionType::Kinematic, true}}) {
    SCOPED_TRACE(::testing::Message() << "motion=" << int(motion) << " target=" << kinematicTarget);
    PhysicsWorld actual(jointWorldSettings()), control(jointWorldSettings());
    struct Mechanism {
      PhysicsBodyId support{}, remote{};
      std::array<PhysicsBodyId, 3> links{};
    };
    const auto populate = [&](PhysicsWorld &world) {
      Mechanism result;
      result.support = world.createBox(glm::vec3{.5f},
          {.motionType = motion, .position = {-2, 0, 0}});
      for (size_t index = 0; index < result.links.size(); ++index)
        result.links[index] = world.createBox(glm::vec3{.5f},
            {.position = {1.2 * index, 0, 0}});
      result.remote = world.createBox(glm::vec3{.5f}, {.position = {30, 0, 0}});
      PhysicsJointSettings settings;
      settings.type = PhysicsJointType::Fixed;
      settings.anchor = {-1, 0, 0};
      (void)world.createJoint(result.support, result.links[0], settings);
      for (size_t index = 1; index < result.links.size(); ++index) {
        settings.anchor = {1.2 * index - .6, 0, 0};
        (void)world.createJoint(result.links[index - 1], result.links[index], settings);
      }
      return result;
    };
    const auto mechanism = populate(actual), reference = populate(control);
    simulate(actual, 180u);
    simulate(control, 180u);
    ASSERT_EQ(actual.activeBodyCount(), 0u);
    ASSERT_EQ(control.activeBodyCount(), 0u);
    if (kinematicTarget) {
      auto unchanged = *actual.pose(mechanism.support);
      ASSERT_TRUE(actual.moveKinematic(mechanism.support, unchanged, kStep));
      unchanged.rotation = -unchanged.rotation;
      ASSERT_TRUE(actual.moveKinematic(mechanism.support, unchanged, kStep));
      EXPECT_EQ(actual.activeBodyCount(), 0u);
      ASSERT_TRUE(actual.moveKinematic(mechanism.support, {{-2, .1, 0}}, kStep));
      ASSERT_TRUE(control.moveKinematic(reference.support, {{-2, .1, 0}}, kStep));
    } else {
      ASSERT_TRUE(actual.setPose(mechanism.support, {{-2, .1, 0}}, false));
      ASSERT_TRUE(control.setPose(reference.support, {{-2, .1, 0}}, false));
    }
    for (const auto link : reference.links)
      ASSERT_TRUE(control.setPose(link, *control.pose(link), true));
    const size_t expectedActive = 3u + (motion == PhysicsMotionType::Kinematic ? 1u : 0u);
    EXPECT_EQ(actual.activeBodyCount(), expectedActive);
    actual.step(kStep);
    control.step(kStep);
    for (size_t index = 0; index < mechanism.links.size(); ++index)
      expectPosition(actual, mechanism.links[index], control.pose(reference.links[index])->position, 1e-6);
    expectPosition(actual, mechanism.remote, {30, 0, 0});
    EXPECT_EQ(*actual.linearVelocity(mechanism.remote), glm::vec3{0});
    EXPECT_EQ(actual.activeBodyCount(), expectedActive);
  }
}

TEST(PhysicsJointTests, ExplicitDynamicActuationWakesAllLinksBeforeTheirFirstSolve) {
  for (int operation = 0; operation < 3; ++operation) {
    SCOPED_TRACE(operation);
    PhysicsWorld actual(jointWorldSettings()), control(jointWorldSettings());
    const auto populate = [](PhysicsWorld &world) {
      std::array<PhysicsBodyId, 4> bodies{};
      for (size_t index = 0; index < 3; ++index)
        bodies[index] = world.createBox(glm::vec3{.5f}, {.position = {1.2 * index, 0, 0}});
      bodies.back() = world.createBox(glm::vec3{.5f}, {.position = {30, 0, 0}});
      PhysicsJointSettings settings;
      settings.type = PhysicsJointType::Fixed;
      for (size_t index = 1; index < 3; ++index) {
        settings.anchor = {1.2 * index - .6, 0, 0};
        (void)world.createJoint(bodies[index - 1], bodies[index], settings);
      }
      return bodies;
    };
    const auto bodies = populate(actual), reference = populate(control);
    simulate(actual, 180u);
    simulate(control, 180u);
    ASSERT_EQ(actual.activeBodyCount(), 0u);
    ASSERT_EQ(control.activeBodyCount(), 0u);
    ASSERT_TRUE(actual.setLinearVelocity(bodies[0], {}));
    ASSERT_TRUE(actual.setAngularVelocity(bodies[0], {}));
    auto moved = *actual.pose(bodies[0]);
    moved.position.y += .1;
    ASSERT_TRUE(actual.setPose(bodies[0], moved, false));
    EXPECT_EQ(actual.activeBodyCount(), 0u);
    ASSERT_TRUE(actual.setPose(bodies[0], *control.pose(reference[0]), false));
    const auto actuate = [&](PhysicsWorld &world, PhysicsBodyId first) {
      if (operation == 0)
        return world.setLinearVelocity(first, {0, 1, 0});
      if (operation == 1)
        return world.setAngularVelocity(first, {0, 0, 1});
      auto pose = *world.pose(first);
      pose.position.y += .1;
      return world.setPose(first, pose, true);
    };
    ASSERT_TRUE(actuate(actual, bodies[0]));
    ASSERT_TRUE(actuate(control, reference[0]));
    for (size_t index = 0; index < 3; ++index)
      ASSERT_TRUE(control.setPose(reference[index], *control.pose(reference[index]), true));
    EXPECT_EQ(actual.activeBodyCount(), 3u);
    actual.step(kStep);
    control.step(kStep);
    for (size_t index = 0; index < 3; ++index) {
      expectPosition(actual, bodies[index], control.pose(reference[index])->position, 1e-6);
      EXPECT_NEAR(glm::length(*actual.linearVelocity(bodies[index]) -
                              *control.linearVelocity(reference[index])), 0, 1e-5);
      EXPECT_NEAR(glm::length(*actual.angularVelocity(bodies[index]) -
                              *control.angularVelocity(reference[index])), 0, 1e-5);
    }
    expectPosition(actual, bodies.back(), {30, 0, 0});
    EXPECT_EQ(actual.activeBodyCount(), 3u);
  }
}

TEST(PhysicsJointTests, KinematicVelocityEditsWakeAttachedLinksBeforeTheirFirstSolve) {
  for (const bool angular : {false, true}) {
    SCOPED_TRACE(angular);
    PhysicsWorld actual(jointWorldSettings()), control(jointWorldSettings());
    struct Mechanism {
      PhysicsBodyId support{}, remote{};
      std::array<PhysicsBodyId, 3> links{};
    };
    const auto populate = [](PhysicsWorld &world) {
      Mechanism result;
      result.support = world.createBox(glm::vec3{.5f},
          {.motionType = PhysicsMotionType::Kinematic, .position = {-2, 0, 0}});
      for (size_t index = 0; index < result.links.size(); ++index)
        result.links[index] = world.createBox(glm::vec3{.5f},
            {.position = {1.2 * index, 0, 0}});
      result.remote = world.createBox(glm::vec3{.5f}, {.position = {30, 0, 0}});
      PhysicsJointSettings settings;
      settings.type = PhysicsJointType::Fixed;
      settings.anchor = {-1, 0, 0};
      (void)world.createJoint(result.support, result.links[0], settings);
      for (size_t index = 1; index < result.links.size(); ++index) {
        settings.anchor = {1.2 * index - .6, 0, 0};
        (void)world.createJoint(result.links[index - 1], result.links[index], settings);
      }
      return result;
    };
    const auto mechanism = populate(actual), reference = populate(control);
    simulate(actual, 180u);
    simulate(control, 180u);
    ASSERT_EQ(actual.activeBodyCount(), 0u);
    ASSERT_EQ(control.activeBodyCount(), 0u);
    ASSERT_TRUE(actual.setLinearVelocity(mechanism.support, {}));
    ASSERT_TRUE(actual.setAngularVelocity(mechanism.support, {}));
    EXPECT_EQ(actual.activeBodyCount(), 0u);
    const auto actuate = [&](PhysicsWorld &world, PhysicsBodyId support) {
      return angular ? world.setAngularVelocity(support, {0, 0, .5f})
                     : world.setLinearVelocity(support, {0, 1, 0});
    };
    ASSERT_TRUE(actuate(actual, mechanism.support));
    ASSERT_TRUE(actuate(control, reference.support));
    for (const auto link : reference.links)
      ASSERT_TRUE(control.setPose(link, *control.pose(link), true));
    EXPECT_EQ(actual.activeBodyCount(), 4u);
    // Reapplying the same nonzero driver velocity keeps its value and must
    // not activate any unrelated component.
    ASSERT_TRUE(actuate(actual, mechanism.support));
    EXPECT_EQ(actual.activeBodyCount(), 4u);
    actual.step(kStep);
    control.step(kStep);
    EXPECT_GT(glm::length(control.pose(reference.links.back())->position -
                           glm::dvec3{2.4, 0, 0}), 1e-5);
    for (size_t index = 0; index < mechanism.links.size(); ++index) {
      expectPosition(actual, mechanism.links[index], control.pose(reference.links[index])->position, 1e-6);
      EXPECT_NEAR(glm::length(*actual.linearVelocity(mechanism.links[index]) -
                              *control.linearVelocity(reference.links[index])), 0, 1e-5);
      EXPECT_NEAR(glm::length(*actual.angularVelocity(mechanism.links[index]) -
                              *control.angularVelocity(reference.links[index])), 0, 1e-5);
    }
    expectPosition(actual, mechanism.support, control.pose(reference.support)->position, 1e-6);
    expectPosition(actual, mechanism.remote, {30, 0, 0});
    EXPECT_EQ(*actual.linearVelocity(mechanism.remote), glm::vec3{0});
    EXPECT_EQ(actual.activeBodyCount(), 4u);
  }
}

TEST(PhysicsWorldTests, SlowPrescribedKinematicVelocitiesOutliveTheSleepTimeout) {
  for (const float speed : {.01f, 1e-7f}) {
    for (const bool angular : {false, true}) {
      for (const bool initial : {false, true}) {
        for (const bool articulated : {false, true}) {
          SCOPED_TRACE(testing::Message{} << "speed=" << speed << " angular=" << angular
                                        << " initial=" << initial
                                        << " articulated=" << articulated);
          PhysicsWorld actual(jointWorldSettings()), control(jointWorldSettings());
          struct Mechanism {
            PhysicsBodyId driver{}, remote{}, stationary{};
            std::vector<PhysicsBodyId> links;
          };
          const auto populate = [&](PhysicsWorld &world) {
            Mechanism result;
            PhysicsBodySettings settings{.motionType = PhysicsMotionType::Kinematic,
                                         .position = {-2, 0, 0}};
            if (initial) {
              if (angular) settings.angularVelocity.z = speed;
              else settings.linearVelocity.y = speed;
            }
            result.driver = world.createBox(glm::vec3{.5f}, settings);
            if (articulated) {
              auto previous = result.driver;
              for (size_t index = 0; index < 3; ++index) {
                const auto link = world.createBox(glm::vec3{.5f},
                    {.position = {1.2 * index, 0, 0}});
                PhysicsJointSettings joint{.type = PhysicsJointType::Fixed,
                    .anchor = {index == 0 ? -1.0 : 1.2 * index - .6, 0, 0}};
                (void)world.createJoint(previous, link, joint);
                result.links.push_back(link);
                previous = link;
              }
            }
            result.remote = world.createBox(glm::vec3{.5f}, {.position = {30, 0, 0}});
            result.stationary = world.createBox(glm::vec3{.5f},
                {.motionType = PhysicsMotionType::Kinematic, .position = {32, 0, 0}});
            (void)world.createJoint(result.stationary, result.remote,
                {.type = PhysicsJointType::Fixed, .anchor = {31, 0, 0}});
            return result;
          };
          const auto mechanism = populate(actual), reference = populate(control);
          if (!initial) {
            simulate(actual, 180);
            simulate(control, 180);
            ASSERT_EQ(actual.activeBodyCount(), 0u);
            const auto actuate = [&](PhysicsWorld &world, PhysicsBodyId driver) {
              return angular ? world.setAngularVelocity(driver, {0, 0, speed})
                             : world.setLinearVelocity(driver, {0, speed, 0});
            };
            ASSERT_TRUE(actuate(actual, mechanism.driver));
            ASSERT_TRUE(actuate(control, reference.driver));
          }
          for (size_t frame = 1; frame <= 600; ++frame) {
            // The control explicitly keeps every affected body awake. Prescribed
            // motion must be independent of the ordinary low-speed sleep test.
            ASSERT_TRUE(control.setPose(reference.driver, *control.pose(reference.driver), true));
            for (const auto link : reference.links)
              ASSERT_TRUE(control.setPose(link, *control.pose(link), true));
            actual.step(kStep);
            control.step(kStep);
            if (frame == 60 || frame == 600)
              EXPECT_EQ(actual.activeBodyCount(), 1u + mechanism.links.size());
          }
          const auto actualPose = *actual.pose(mechanism.driver);
          const auto expectedPose = *control.pose(reference.driver);
          expectPosition(actual, mechanism.driver, expectedPose.position, 1e-6);
          EXPECT_NEAR(std::abs(glm::dot(actualPose.rotation, expectedPose.rotation)), 1, 1e-6);
          if (angular) {
            const double angle = 2 * std::atan2(double(actualPose.rotation.z),
                                               double(actualPose.rotation.w));
            const double expectedAngle = 2 * std::atan2(double(expectedPose.rotation.z),
                                                       double(expectedPose.rotation.w));
            EXPECT_NEAR(angle, expectedAngle, 1e-5);
            // Jolt Body::AddRotationStep skips angular increments <=1e-6 rad.
            // The subquantum command must remain active/retained just like the
            // awake control; only resolvable increments have an analytic path.
            if (speed == .01f)
              EXPECT_NEAR(angle, speed * 10.0, speed * .001);
            EXPECT_EQ(*actual.angularVelocity(mechanism.driver), (glm::vec3{0, 0, speed}));
          } else {
            EXPECT_NEAR(actualPose.position.y, speed * 10.0, speed * .0001);
            EXPECT_EQ(*actual.linearVelocity(mechanism.driver), (glm::vec3{0, speed, 0}));
          }
          for (size_t index = 0; index < mechanism.links.size(); ++index) {
            // Explicit pose activation renormalizes the control quaternions.
            // Allow 1e-5 scene units for accumulated joint-solver rounding.
            expectPosition(actual, mechanism.links[index], control.pose(reference.links[index])->position, 1e-5);
            EXPECT_NEAR(glm::length(*actual.linearVelocity(mechanism.links[index]) -
                                    *control.linearVelocity(reference.links[index])), 0, 1e-5);
            EXPECT_NEAR(glm::length(*actual.angularVelocity(mechanism.links[index]) -
                                    *control.angularVelocity(reference.links[index])), 0, 1e-5);
          }
          expectPosition(actual, mechanism.remote, {30, 0, 0});
          expectPosition(actual, mechanism.stationary, {32, 0, 0});
          EXPECT_EQ(*actual.linearVelocity(mechanism.remote), glm::vec3{0});
        }
      }
    }
  }
}

TEST(PhysicsWorldTests, PrescribedKinematicDriveStopsRestartAndRetirementRestoreSleeping) {
  PhysicsWorld world({.gravity = {0, 0, 0}, .maxBodies = 4, .workerThreads = 1});
  const auto driver = world.createBox(glm::vec3{.5f},
      {.motionType = PhysicsMotionType::Kinematic,
       .linearVelocity = {.01f, 0, 0}, .angularVelocity = {0, 0, .01f}});
  const auto remote = world.createBox(glm::vec3{.5f}, {.position = {30, 0, 0}});
  simulate(world, 180);
  EXPECT_EQ(world.activeBodyCount(), 1u);
  EXPECT_NEAR(world.pose(driver)->position.x, .03, 1e-6);

  ASSERT_TRUE(world.setLinearVelocity(driver, {}));
  const auto linearStop = *world.pose(driver);
  simulate(world, 120);
  expectPosition(world, driver, linearStop.position);
  EXPECT_GT(std::abs(world.pose(driver)->rotation.z - linearStop.rotation.z), .009f);
  EXPECT_EQ(world.activeBodyCount(), 1u); // Angular drive remains commanded.
  ASSERT_TRUE(world.setAngularVelocity(driver, {}));
  simulate(world, 180);
  EXPECT_EQ(world.activeBodyCount(), 0u);
  ASSERT_TRUE(world.setLinearVelocity(driver, {.01f, 0, 0}));
  simulate(world, 120);
  EXPECT_NEAR(world.pose(driver)->position.x - linearStop.position.x, .02, 1e-6);

  // MoveKinematic also replaces the prescribed velocities. Completing a target
  // and then submitting its unchanged pose must retire the driver membership.
  auto target = *world.pose(driver);
  target.position.y += .01;
  ASSERT_TRUE(world.moveKinematic(driver, target, 1.0f));
  simulate(world, 60);
  expectPosition(world, driver, target.position, 1e-6);
  ASSERT_TRUE(world.moveKinematic(driver, *world.pose(driver), kStep));
  simulate(world, 180);
  EXPECT_EQ(world.activeBodyCount(), 0u);

  ASSERT_TRUE(world.setAngularVelocity(driver, {0, 0, .01f}));
  ASSERT_TRUE(world.destroyBody(driver));
  const auto reused = world.createBox(glm::vec3{.5f},
      {.motionType = PhysicsMotionType::Kinematic});
  simulate(world, 180);
  EXPECT_EQ(world.activeBodyCount(), 0u);
  expectPosition(world, reused, {0, 0, 0});
  expectPosition(world, remote, {30, 0, 0});
  world.clear();
  const auto reloaded = world.createSphere(.5f,
      {.motionType = PhysicsMotionType::Kinematic, .linearVelocity = {.01f, 0, 0}});
  simulate(world, 120);
  EXPECT_NEAR(world.pose(reloaded)->position.x, .02, 1e-6);
}

TEST(PhysicsJointTests, CreatingMotorJointWakesTheExistingDynamicComponent) {
  for (const bool kinematicAnchor : {false, true}) {
    SCOPED_TRACE(kinematicAnchor);
    PhysicsWorld actual(jointWorldSettings()), control(jointWorldSettings());
    struct Mechanism {
      std::array<PhysicsBodyId, 3> links{};
      PhysicsBodyId support{}, remote{};
    };
    const auto populate = [&](PhysicsWorld &world) {
      Mechanism result;
      if (kinematicAnchor)
        result.support = world.createBox(glm::vec3{.5f},
            {.motionType = PhysicsMotionType::Kinematic, .position = {-2, 0, 0}});
      for (size_t index = 0; index < result.links.size(); ++index)
        result.links[index] = world.createBox(glm::vec3{.5f}, {.position = {1.2 * index, 0, 0}});
      result.remote = world.createBox(glm::vec3{.5f}, {.position = {30, 0, 0}});
      PhysicsJointSettings settings;
      settings.type = PhysicsJointType::Fixed;
      for (size_t index = 1; index < result.links.size(); ++index) {
        settings.anchor = {1.2 * index - .6, 0, 0};
        (void)world.createJoint(result.links[index - 1], result.links[index], settings);
      }
      if (kinematicAnchor) {
        settings.anchor = {15, 0, 0};
        (void)world.createJoint(result.support, result.remote, settings);
      }
      return result;
    };
    const auto mechanism = populate(actual), reference = populate(control);
    simulate(actual, 180u);
    simulate(control, 180u);
    ASSERT_EQ(actual.activeBodyCount(), 0u);
    ASSERT_EQ(control.activeBodyCount(), 0u);
    PhysicsJointSettings settings;
    settings.axis = {0, 0, 1};
    settings.normal = {1, 0, 0};
    settings.motor = {.mode = PhysicsJointMotorMode::Velocity, .targetVelocity = 1};
    (void)actual.createJoint(mechanism.support, mechanism.links[0], settings);
    (void)control.createJoint(reference.support, reference.links[0], settings);
    for (const auto link : reference.links)
      ASSERT_TRUE(control.setPose(link, *control.pose(link), true));
    EXPECT_EQ(actual.activeBodyCount(), 3u);
    actual.step(kStep);
    control.step(kStep);
    EXPECT_GT(control.pose(reference.links.back())->position.y, .001);
    for (size_t index = 0; index < mechanism.links.size(); ++index) {
      expectPosition(actual, mechanism.links[index], control.pose(reference.links[index])->position, 1e-6);
      EXPECT_NEAR(glm::length(*actual.angularVelocity(mechanism.links[index]) -
                              *control.angularVelocity(reference.links[index])), 0, 1e-5);
    }
    expectPosition(actual, mechanism.remote, {30, 0, 0});
    EXPECT_EQ(actual.activeBodyCount(), 3u);
  }
}

TEST(PhysicsJointTests, RemovingAnchorJointOrBodyReleasesEverySleepingDynamicLink) {
  for (const bool removeBody : {false, true}) {
    SCOPED_TRACE(removeBody);
    PhysicsWorld actual(testWorldSettings()), control(testWorldSettings());
    struct Mechanism {
      std::array<PhysicsBodyId, 3> links{};
      PhysicsBodyId support{}, remote{};
      PhysicsJointId anchor{};
    };
    const auto populate = [&](PhysicsWorld &world) {
      Mechanism result;
      if (removeBody)
        result.support = world.createBox(glm::vec3{.5f},
            {.motionType = PhysicsMotionType::Static, .position = {0, 7, 0}});
      for (size_t index = 0; index < result.links.size(); ++index)
        result.links[index] = world.createBox(glm::vec3{.5f}, {.position = {0, 5 - 1.2 * index, 0}});
      result.remote = world.createBox(glm::vec3{.5f}, {.position = {30, 5, 0}, .gravityFactor = 0});
      PhysicsJointSettings settings;
      settings.type = PhysicsJointType::Fixed;
      settings.anchor = {0, removeBody ? 6.0 : 5.0, 0};
      result.anchor = world.createJoint(result.support, result.links[0], settings);
      for (size_t index = 1; index < result.links.size(); ++index) {
        settings.anchor = {0, 5.6 - 1.2 * index, 0};
        (void)world.createJoint(result.links[index - 1], result.links[index], settings);
      }
      return result;
    };
    const auto mechanism = populate(actual), reference = populate(control);
    simulate(actual, 360u);
    simulate(control, 360u);
    ASSERT_EQ(actual.activeBodyCount(), 0u);
    ASSERT_EQ(control.activeBodyCount(), 0u);
    const auto before = *actual.pose(mechanism.links.back());
    ASSERT_TRUE(removeBody ? actual.destroyBody(mechanism.support) : actual.destroyJoint(mechanism.anchor));
    ASSERT_TRUE(removeBody ? control.destroyBody(reference.support) : control.destroyJoint(reference.anchor));
    for (const auto link : reference.links)
      ASSERT_TRUE(control.setPose(link, *control.pose(link), true));
    EXPECT_EQ(actual.activeBodyCount(), 3u);
    EXPECT_EQ(actual.jointCount(), 2u);
    EXPECT_FALSE(actual.containsJoint(mechanism.anchor));
    actual.step(kStep);
    control.step(kStep);
    EXPECT_LT(control.pose(reference.links.back())->position.y, before.position.y - .001);
    for (size_t index = 0; index < mechanism.links.size(); ++index) {
      expectPosition(actual, mechanism.links[index], control.pose(reference.links[index])->position, 1e-6);
      EXPECT_NEAR(glm::length(*actual.linearVelocity(mechanism.links[index]) -
                              *control.linearVelocity(reference.links[index])), 0, 1e-5);
    }
    expectPosition(actual, mechanism.remote, {30, 5, 0});
    EXPECT_EQ(actual.activeBodyCount(), 3u);
  }
}

TEST(PhysicsJointTests, CyclicTopologyAndReusedBodySlotsDoNotReplayRetiredBodies) {
  auto settings = jointWorldSettings();
  settings.maxBodies = 4;
  PhysicsWorld world(settings);
  std::array<PhysicsBodyId, 3> links{};
  for (size_t index = 0; index < links.size(); ++index)
    links[index] = world.createBox(glm::vec3{.5f}, {.position = {1.2 * index, 0, 0}});
  const auto remote = world.createBox(glm::vec3{.5f}, {.position = {30, 0, 0}});
  const auto connect = [&](size_t a, size_t b) {
    PhysicsJointSettings joint;
    joint.type = PhysicsJointType::Fixed;
    joint.anchor = {.6 * (a + b), 0, 0};
    return world.createJoint(links[a], links[b], joint);
  };
  (void)connect(0, 1);
  (void)connect(1, 2);
  auto closing = connect(2, 0);
  for (int iteration = 0; iteration < 4; ++iteration) {
    SCOPED_TRACE(iteration);
    simulate(world, 180u);
    ASSERT_EQ(world.activeBodyCount(), 0u);
    ASSERT_TRUE(world.destroyJoint(closing));
    EXPECT_EQ(world.activeBodyCount(), 3u);
    closing = connect(2, 0);
    const auto retired = links[1];
    ASSERT_TRUE(world.destroyBody(retired));
    EXPECT_EQ(world.activeBodyCount(), 2u);
    EXPECT_FALSE(world.contains(retired));
    // All four slots were occupied, so this new body must reuse backend
    // storage while retaining a distinct public identity.
    links[1] = world.createBox(glm::vec3{.5f}, {.position = {1.2, 0, 0}});
    EXPECT_NE(links[1], retired);
    (void)connect(0, 1);
    (void)connect(1, 2);
    EXPECT_EQ(world.bodyCount(), 4u);
    EXPECT_EQ(world.jointCount(), 3u);
    EXPECT_EQ(world.activeBodyCount(), 3u);
    world.step(kStep);
    expectPosition(world, remote, {30, 0, 0});
  }
}

TEST(PhysicsWorldTests, ActivatingDynamicTeleportWakesDepartedAndDestinationContactsOnly) {
  for (const bool activate : {false, true}) {
    SCOPED_TRACE(activate);
    PhysicsWorld world(testWorldSettings());
    (void)world.createBox({40, .5f, 4}, {.motionType = PhysicsMotionType::Static, .position = {15, -.5, 0}});
    const auto lower = world.createBox(glm::vec3{.5f}, {.position = {0, .5, 0}});
    const auto upper = world.createBox(glm::vec3{.5f}, {.position = {0, 1.5, 0}});
    const auto destination = world.createBox(glm::vec3{.5f}, {.position = {10, .5, 0}});
    const auto remote = world.createBox(glm::vec3{.5f}, {.position = {30, .5, 0}});
    simulate(world, 360u);
    ASSERT_EQ(world.activeBodyCount(), 0u);
    const auto before = *world.pose(upper);
    ASSERT_TRUE(world.setPose(lower, {{10, .5, 0}}, activate));
    if (activate) {
      EXPECT_EQ(world.activeBodyCount(), 3u);
      simulate(world, 30u);
      EXPECT_LT(world.pose(upper)->position.y, before.position.y - .5);
      EXPECT_GT(glm::length(world.pose(destination)->position - glm::dvec3{10, .5, 0}), .1);
    } else {
      EXPECT_EQ(world.activeBodyCount(), 0u);
      world.step(kStep);
      expectPosition(world, upper, before.position);
    }
    expectPosition(world, remote, {30, .5, 0}, .04);
    EXPECT_EQ(*world.linearVelocity(remote), glm::vec3{0});
  }
}

TEST(PhysicsJointTests, LosingContactSupportWakesEverySleepingDynamicLink) {
  for (const bool removeSupport : {false, true}) {
    SCOPED_TRACE(removeSupport);
    PhysicsWorld actual(testWorldSettings()), control(testWorldSettings());
    struct Mechanism {
      std::array<PhysicsBodyId, 3> links{};
      PhysicsBodyId floor{}, remote{};
    };
    const auto populate = [&](PhysicsWorld &world) {
      Mechanism result;
      result.floor = world.createBox({2, .5f, 2},
          {.motionType = PhysicsMotionType::Static, .position = {0, -.5, 0}});
      for (size_t index = 0; index < result.links.size(); ++index)
        result.links[index] = world.createBox(glm::vec3{.5f},
            {.position = {0, .5 + 1.2 * index, 0}});
      PhysicsJointSettings joint;
      joint.type = PhysicsJointType::Fixed;
      for (size_t index = 1; index < result.links.size(); ++index) {
        joint.anchor = {0, 1.2 * index - .1, 0};
        (void)world.createJoint(result.links[index - 1], result.links[index], joint);
      }
      result.remote = world.createBox(glm::vec3{.5f},
          {.position = {30, .5, 0}, .gravityFactor = 0});
      return result;
    };
    const auto mechanism = populate(actual), reference = populate(control);
    simulate(actual, 360u);
    simulate(control, 360u);
    ASSERT_EQ(actual.activeBodyCount(), 0u);
    ASSERT_EQ(control.activeBodyCount(), 0u);
    const auto before = *actual.pose(mechanism.links.back());
    ASSERT_TRUE(removeSupport ? actual.destroyBody(mechanism.floor)
                              : actual.setPose(mechanism.floor, {{10, -.5, 0}}));
    ASSERT_TRUE(removeSupport ? control.destroyBody(reference.floor)
                              : control.setPose(reference.floor, {{10, -.5, 0}}));
    for (const auto link : reference.links)
      ASSERT_TRUE(control.setPose(link, *control.pose(link), true));
    // Only the lowest link overlaps the support's old bounds. Its farther
    // fixed joints must already participate in the first constraint snapshot.
    EXPECT_EQ(actual.activeBodyCount(), 3u);
    actual.step(kStep);
    control.step(kStep);
    EXPECT_LT(control.pose(reference.links.back())->position.y, before.position.y - .001);
    for (size_t index = 0; index < mechanism.links.size(); ++index) {
      expectPosition(actual, mechanism.links[index], control.pose(reference.links[index])->position, 1e-6);
      EXPECT_NEAR(glm::length(*actual.linearVelocity(mechanism.links[index]) -
                              *control.linearVelocity(reference.links[index])), 0, 1e-5);
    }
    expectPosition(actual, mechanism.remote, {30, .5, 0});
    EXPECT_EQ(*actual.linearVelocity(mechanism.remote), glm::vec3{0});
    EXPECT_EQ(actual.activeBodyCount(), 3u);
  }
}

TEST(PhysicsJointTests, NewContactSupportWakesEverySleepingDynamicLink) {
  PhysicsWorld actual(jointWorldSettings()), control(jointWorldSettings());
  struct Mechanism {
    std::array<PhysicsBodyId, 3> links{};
    PhysicsBodyId remote{};
  };
  const auto populate = [&](PhysicsWorld &world) {
    Mechanism result;
    for (size_t index = 0; index < result.links.size(); ++index)
      result.links[index] = world.createBox(glm::vec3{.5f},
          {.position = {1.2 * index, 0, 0}});
    PhysicsJointSettings joint;
    joint.type = PhysicsJointType::Fixed;
    for (size_t index = 1; index < result.links.size(); ++index) {
      joint.anchor = {1.2 * index - .6, 0, 0};
      (void)world.createJoint(result.links[index - 1], result.links[index], joint);
    }
    const auto stationary = world.createBox(glm::vec3{.5f},
        {.motionType = PhysicsMotionType::Kinematic, .position = {0, -1.6, 0}});
    result.remote = world.createBox(glm::vec3{.5f}, {.position = {30, -1.6, 0}});
    joint.anchor = {15, -1.6, 0};
    (void)world.createJoint(stationary, result.remote, joint);
    return result;
  };
  const auto mechanism = populate(actual), reference = populate(control);
  simulate(actual, 180u);
  simulate(control, 180u);
  ASSERT_EQ(actual.activeBodyCount(), 0u);
  ASSERT_EQ(control.activeBodyCount(), 0u);
  (void)actual.createBox(glm::vec3{.5f},
      {.motionType = PhysicsMotionType::Static, .position = {0, -.6, 0}});
  (void)control.createBox(glm::vec3{.5f},
      {.motionType = PhysicsMotionType::Static, .position = {0, -.6, 0}});
  for (const auto link : reference.links)
    ASSERT_TRUE(control.setPose(link, *control.pose(link), true));
  EXPECT_EQ(actual.activeBodyCount(), 3u);
  actual.step(kStep);
  control.step(kStep);
  EXPECT_GT(glm::length(control.pose(reference.links.back())->position -
                         glm::dvec3{2.4, 0, 0}), 1e-5);
  for (size_t index = 0; index < mechanism.links.size(); ++index) {
    expectPosition(actual, mechanism.links[index], control.pose(reference.links[index])->position, 1e-6);
    EXPECT_NEAR(glm::length(*actual.linearVelocity(mechanism.links[index]) -
                            *control.linearVelocity(reference.links[index])), 0, 1e-5);
    EXPECT_NEAR(glm::length(*actual.angularVelocity(mechanism.links[index]) -
                            *control.angularVelocity(reference.links[index])), 0, 1e-5);
  }
  // The new support also touches a stationary Kinematic boundary, whose
  // separate far-away mechanism must not be activated by this contact edit.
  expectPosition(actual, mechanism.remote, {30, -1.6, 0});
  EXPECT_EQ(actual.activeBodyCount(), 3u);
}

TEST(PhysicsJointTests, InsertedMovableCollidersWakeOverlappingMechanismsBeforeFirstSolve) {
  for (const auto motion : {PhysicsMotionType::Kinematic, PhysicsMotionType::Dynamic}) {
    SCOPED_TRACE(int(motion));
    PhysicsWorld actual(jointWorldSettings()), control(jointWorldSettings());
    struct Mechanism {
      std::array<PhysicsBodyId, 3> links{};
      PhysicsBodyId stationary{}, remote{};
    };
    const auto populate = [&](PhysicsWorld &world) {
      Mechanism result;
      for (size_t index = 0; index < result.links.size(); ++index)
        result.links[index] = world.createBox(glm::vec3{.5f},
            {.position = {1.2 * index, 0, 0}});
      PhysicsJointSettings joint;
      joint.type = PhysicsJointType::Fixed;
      for (size_t index = 1; index < result.links.size(); ++index) {
        joint.anchor = {1.2 * index - .6, 0, 0};
        (void)world.createJoint(result.links[index - 1], result.links[index], joint);
      }
      // This stationary support intersects the expanded query bounds, but its
      // 3 cm gap prevents contact with the inserted collider. Do not walk its
      // unrelated Dynamic branch merely because it was a broad-phase hit.
      result.stationary = world.createBox(glm::vec3{.1f},
          {.motionType = PhysicsMotionType::Kinematic, .position = {-.63, -.6, 0}});
      result.remote = world.createBox(glm::vec3{.5f}, {.position = {30, -.6, 0}});
      joint.anchor = {14.685, -.6, 0};
      (void)world.createJoint(result.stationary, result.remote, joint);
      return result;
    };
    const auto mechanism = populate(actual), reference = populate(control);
    simulate(actual, 180u);
    simulate(control, 180u);
    ASSERT_EQ(actual.activeBodyCount(), 0u);
    ASSERT_EQ(control.activeBodyCount(), 0u);
    const PhysicsBodySettings inserted{.motionType = motion, .position = {0, -.6, 0}};
    const auto actualBody = actual.createBox(glm::vec3{.5f}, inserted);
    const auto controlBody = control.createBox(glm::vec3{.5f}, inserted);
    for (const auto link : reference.links)
      ASSERT_TRUE(control.setPose(link, *control.pose(link), true));
    // Being active itself is insufficient: contact discovery occurs after
    // Jolt snapshots which existing joints will participate in this solve.
    EXPECT_EQ(actual.activeBodyCount(), 4u);
    actual.step(kStep);
    control.step(kStep);
    EXPECT_GT(glm::length(control.pose(reference.links.back())->position -
                           glm::dvec3{2.4, 0, 0}), 1e-5);
    for (size_t index = 0; index < mechanism.links.size(); ++index) {
      const auto a = *actual.pose(mechanism.links[index]);
      const auto b = *control.pose(reference.links[index]);
      expectPosition(actual, mechanism.links[index], b.position, 1e-6);
      const glm::dquat qa{a.rotation}, qb{b.rotation};
      EXPECT_LT(std::min(glm::length(qa - qb), glm::length(qa + qb)), 1e-6);
      EXPECT_NEAR(glm::length(*actual.linearVelocity(mechanism.links[index]) -
                              *control.linearVelocity(reference.links[index])), 0, 1e-5);
      EXPECT_NEAR(glm::length(*actual.angularVelocity(mechanism.links[index]) -
                              *control.angularVelocity(reference.links[index])), 0, 1e-5);
    }
    expectPosition(actual, actualBody, control.pose(controlBody)->position, 1e-6);
    expectPosition(actual, mechanism.stationary, {-.63, -.6, 0});
    expectPosition(actual, mechanism.remote, {30, -.6, 0});
    EXPECT_EQ(*actual.linearVelocity(mechanism.remote), glm::vec3{0});
    EXPECT_EQ(*actual.angularVelocity(mechanism.remote), glm::vec3{0});
    EXPECT_EQ(actual.activeBodyCount(), 4u);
  }
}

TEST(PhysicsJointTests, SlowMotorsKeepTheirDynamicMechanismsMovingPastTheSleepTimeout) {
  for (const auto type : {PhysicsJointType::Hinge, PhysicsJointType::Slider}) {
    for (const auto mode : {PhysicsJointMotorMode::Velocity, PhysicsJointMotorMode::Position}) {
      SCOPED_TRACE(int(type));
      SCOPED_TRACE(int(mode));
      PhysicsWorld actual(jointWorldSettings()), control(jointWorldSettings());
      struct Mechanism {
        std::array<PhysicsBodyId, 3> links{};
        PhysicsBodyId support{}, remote{};
        PhysicsJointId motor{};
      };
      const auto populate = [&](PhysicsWorld &world) {
        Mechanism result;
        result.support = world.createBox(glm::vec3{.5f},
            {.motionType = PhysicsMotionType::Kinematic, .position = {-2, 0, 0}});
        for (size_t index = 0; index < result.links.size(); ++index)
          result.links[index] = world.createBox(glm::vec3{.5f},
              {.position = {1.2 * index, 0, 0}});
        PhysicsJointSettings joint;
        joint.type = PhysicsJointType::Fixed;
        for (size_t index = 1; index < result.links.size(); ++index) {
          joint.anchor = {1.2 * index - .6, 0, 0};
          (void)world.createJoint(result.links[index - 1], result.links[index], joint);
        }
        result.remote = world.createBox(glm::vec3{.5f}, {.position = {30, 0, 0}});
        joint.anchor = {15, 0, 0};
        (void)world.createJoint(result.support, result.remote, joint);
        joint.type = type;
        joint.anchor = {0, 0, 0};
        result.motor = world.createJoint(result.support, result.links[0], joint);
        return result;
      };
      const auto mechanism = populate(actual), reference = populate(control);
      simulate(actual, 180u);
      simulate(control, 180u);
      ASSERT_EQ(actual.activeBodyCount(), 0u);
      ASSERT_EQ(control.activeBodyCount(), 0u);
      PhysicsJointMotorSettings motor;
      motor.mode = mode;
      motor.targetVelocity = .01f;
      motor.targetPosition = 1;
      // The extra links increase the effective inertia. This still starts
      // slowly enough to sleep, while its ten-second motion remains visible.
      motor.frequencyHz = .05f;
      ASSERT_TRUE(actual.setJointMotor(mechanism.motor, motor));
      ASSERT_TRUE(control.setJointMotor(reference.motor, motor));
      for (unsigned frame = 1; frame <= 600; ++frame) {
        for (const auto link : reference.links)
          ASSERT_TRUE(control.setPose(link, *control.pose(link), true));
        actual.step(kStep);
        control.step(kStep);
        if (frame == 60 || frame == 600)
          EXPECT_EQ(actual.activeBodyCount(), 3u);
      }
      const auto coordinate = control.jointState(reference.motor)->coordinate;
      EXPECT_GT(coordinate, .05f);
      EXPECT_NEAR(actual.jointState(mechanism.motor)->coordinate, coordinate, 1e-4);
      for (size_t index = 0; index < mechanism.links.size(); ++index) {
        expectPosition(actual, mechanism.links[index], control.pose(reference.links[index])->position, 1e-4);
        EXPECT_NEAR(glm::length(*actual.linearVelocity(mechanism.links[index]) -
                                *control.linearVelocity(reference.links[index])), 0, 1e-4);
        EXPECT_NEAR(glm::length(*actual.angularVelocity(mechanism.links[index]) -
                                *control.angularVelocity(reference.links[index])), 0, 1e-4);
      }
      expectPosition(actual, mechanism.support, {-2, 0, 0});
      expectPosition(actual, mechanism.remote, {30, 0, 0});
      EXPECT_EQ(*actual.linearVelocity(mechanism.remote), glm::vec3{0});
      EXPECT_EQ(*actual.angularVelocity(mechanism.remote), glm::vec3{0});
      EXPECT_EQ(actual.activeBodyCount(), 3u);
    }
  }
}

TEST(PhysicsJointTests, MotorProgressPolicyAllowsSettledUnpoweredAndRetiredDrivesToSleep) {
  enum class State { Off, ZeroVelocity, Unpowered, SettledPosition, Stop, Disable, Remove };
  for (const auto type : {PhysicsJointType::Hinge, PhysicsJointType::Slider}) {
    for (const auto state : {State::Off, State::ZeroVelocity, State::Unpowered,
                            State::SettledPosition, State::Stop, State::Disable, State::Remove}) {
      SCOPED_TRACE(int(type));
      SCOPED_TRACE(int(state));
      PhysicsWorld world(jointWorldSettings());
      const auto body = world.createBox(glm::vec3{.5f});
      const auto remote = world.createBox(glm::vec3{.5f}, {.position = {30, 0, 0}});
      PhysicsJointSettings joint;
      joint.type = type;
      const auto handle = world.createJoint(kInvalidPhysicsBodyId, body, joint);
      PhysicsJointMotorSettings motor;
      motor.mode = PhysicsJointMotorMode::Velocity;
      motor.targetVelocity = .01f;
      if (state == State::Off) motor.mode = PhysicsJointMotorMode::Off;
      if (state == State::ZeroVelocity) motor.targetVelocity = 0;
      if (state == State::Unpowered) motor.maxForceOrTorque = 0;
      if (state == State::SettledPosition) {
        motor.mode = PhysicsJointMotorMode::Position;
        motor.targetPosition = 0;
      }
      ASSERT_TRUE(world.setJointMotor(handle, motor));
      if (state == State::Stop || state == State::Disable || state == State::Remove) {
        simulate(world, 120u);
        EXPECT_GT(world.jointState(handle)->coordinate, .015f);
        if (state == State::Remove) {
          ASSERT_TRUE(world.destroyJoint(handle));
          EXPECT_FALSE(world.containsJoint(handle));
        } else {
          motor.mode = state == State::Disable ? PhysicsJointMotorMode::Off
                                              : PhysicsJointMotorMode::Velocity;
          motor.targetVelocity = 0;
          ASSERT_TRUE(world.setJointMotor(handle, motor));
        }
      }
      simulate(world, 180u);
      EXPECT_EQ(world.activeBodyCount(), 0u);
      expectPosition(world, remote, {30, 0, 0});
      EXPECT_EQ(*world.linearVelocity(remote), glm::vec3{0});
    }
  }
  // A full-range hinge has no effective hard stop, even when its limits UI
  // checkbox is enabled. Equivalent orientations must not trigger a full turn.
  constexpr float pi = 3.14159265358979323846f;
  for (const bool fullRangeEnabled : {false, true}) {
    for (const float direction : {-1.0f, 1.0f}) {
      SCOPED_TRACE(fullRangeEnabled);
      SCOPED_TRACE(direction);
      PhysicsWorld world(jointWorldSettings());
      const auto body = world.createBox(glm::vec3{.5f});
      PhysicsJointSettings joint;
      joint.axis = {0, 1, 0};
      joint.normal = {1, 0, 0};
      joint.limitsEnabled = fullRangeEnabled;
      joint.lowerLimit = -pi;
      joint.upperLimit = pi;
      joint.motor.mode = PhysicsJointMotorMode::Position;
      joint.motor.targetPosition = -direction * (pi - 2e-5f);
      const auto handle = world.createJoint(kInvalidPhysicsBodyId, body, joint);
      PhysicsBodyPose pose;
      pose.rotation = glm::angleAxis(direction * (pi - 2e-5f), joint.axis);
      ASSERT_TRUE(world.setPose(body, pose));
      float previous = world.jointState(handle)->coordinate;
      double travel = 0;
      for (unsigned frame = 0; frame < 240; ++frame) {
        world.step(kStep);
        const float coordinate = world.jointState(handle)->coordinate;
        travel += std::abs(std::remainder(double(coordinate) - previous, double(2.0f * pi)));
        previous = coordinate;
      }
      EXPECT_LT(travel, .001);
      EXPECT_EQ(world.activeBodyCount(), 0u);
      EXPECT_EQ(world.jointState(handle)->settings.limitsEnabled, fullRangeEnabled);
      EXPECT_LT(std::abs(std::remainder(previous - joint.motor.targetPosition, 2.0f * pi)), 1e-4f);
    }
  }
}

TEST(PhysicsJointTests, SlowVelocityDrivesSleepAtLimitsAndResumeAfterReversalOrLimitRemoval) {
  for (const auto type : {PhysicsJointType::Hinge, PhysicsJointType::Slider}) {
    SCOPED_TRACE(int(type));
    PhysicsWorld world(jointWorldSettings());
    const auto body = world.createBox(glm::vec3{.5f});
    PhysicsJointSettings joint;
    joint.type = type;
    joint.limitsEnabled = true;
    joint.lowerLimit = -.02f;
    joint.upperLimit = .02f;
    joint.motor.mode = PhysicsJointMotorMode::Velocity;
    joint.motor.targetVelocity = .01f;
    const auto handle = world.createJoint(kInvalidPhysicsBodyId, body, joint);
    simulate(world, 300u);
    EXPECT_NEAR(world.jointState(handle)->coordinate, .02f, 2e-4f);
    EXPECT_EQ(world.activeBodyCount(), 0u);
    auto motor = joint.motor;
    motor.targetVelocity = -.01f;
    ASSERT_TRUE(world.setJointMotor(handle, motor));
    simulate(world, 360u);
    EXPECT_NEAR(world.jointState(handle)->coordinate, -.02f, 2e-4f);
    EXPECT_EQ(world.activeBodyCount(), 0u);
    ASSERT_TRUE(world.setJointLimits(handle, false, -.02f, .02f));
    simulate(world, 120u);
    EXPECT_LT(world.jointState(handle)->coordinate, -.035f);
    EXPECT_EQ(world.activeBodyCount(), 1u);
  }
  // The endpoints of a full [-pi, pi] interval are not physical hard stops.
  // A slow drive must cross the wrap instead of sleeping in the stop deadband.
  constexpr float pi = 3.14159265358979323846f;
  for (const bool fullRangeEnabled : {false, true}) {
    for (const float direction : {-1.0f, 1.0f}) {
      SCOPED_TRACE(fullRangeEnabled);
      SCOPED_TRACE(direction);
      PhysicsWorld world(jointWorldSettings());
      const auto body = world.createBox(glm::vec3{.5f});
      PhysicsJointSettings joint;
      joint.axis = {0, 1, 0};
      joint.normal = {1, 0, 0};
      joint.limitsEnabled = fullRangeEnabled;
      joint.lowerLimit = -pi;
      joint.upperLimit = pi;
      joint.motor.mode = PhysicsJointMotorMode::Velocity;
      joint.motor.targetVelocity = direction * 1e-4f;
      const auto handle = world.createJoint(kInvalidPhysicsBodyId, body, joint);
      PhysicsBodyPose pose;
      pose.rotation = glm::angleAxis(direction * (pi - 9e-5f), joint.axis);
      ASSERT_TRUE(world.setPose(body, pose));
      float previous = world.jointState(handle)->coordinate;
      double travel = 0;
      for (unsigned frame = 1; frame <= 240; ++frame) {
        world.step(kStep);
        const float coordinate = world.jointState(handle)->coordinate;
        travel += std::abs(std::remainder(double(coordinate) - previous, double(2.0f * pi)));
        previous = coordinate;
        if (frame == 30 || frame == 240)
          EXPECT_EQ(world.activeBodyCount(), 1u);
      }
      EXPECT_NEAR(travel, 4e-4, 1e-5);
      EXPECT_EQ(world.jointState(handle)->settings.limitsEnabled, fullRangeEnabled);
    }
  }
}

TEST(PhysicsJointTests, LimitedHingePositionGoalsUseTheAdmissibleRouteAcrossTheAngleWrap) {
  for (const float direction : {-1.0f, 1.0f}) {
    SCOPED_TRACE(direction);
    PhysicsWorld world(jointWorldSettings());
    const auto body = world.createBox(glm::vec3{.5f});
    const auto remote = world.createBox(glm::vec3{.5f}, {.position = {30, 0, 0}});
    PhysicsJointSettings joint;
    joint.axis = {0, 1, 0};
    joint.normal = {1, 0, 0};
    joint.limitsEnabled = true;
    joint.lowerLimit = -3.1f;
    joint.upperLimit = 3.1f;
    joint.motor.mode = PhysicsJointMotorMode::Position;
    const float start = direction * 3;
    const float goal = -start;
    joint.motor.targetPosition = goal;
    const auto handle = world.createJoint(kInvalidPhysicsBodyId, body, joint);
    const auto setAngle = [&](float angle) {
      PhysicsBodyPose pose;
      pose.rotation = glm::angleAxis(angle, joint.axis);
      ASSERT_TRUE(world.setPose(body, pose));
    };
    const auto checkLimitedRoute = [&](float from, float target) {
      float previous = from;
      bool crossedZero = false;
      for (unsigned frame = 1; frame <= 600; ++frame) {
        world.step(kStep);
        const auto state = *world.jointState(handle);
        EXPECT_EQ(state.settings.motor.mode, PhysicsJointMotorMode::Position);
        EXPECT_FLOAT_EQ(state.settings.motor.targetPosition, target);
        if (frame == 1)
          EXPECT_LT(std::copysign(1.0f, from) * (state.coordinate - from), -1e-4f);
        EXPECT_GE(state.coordinate, -3.101f);
        EXPECT_LE(state.coordinate, 3.101f);
        EXPECT_LT(std::abs(state.coordinate - previous), .8f)
            << "The motor must traverse its allowed interval instead of crossing the wrap discontinuity";
        crossedZero |= state.coordinate * from <= 0;
        previous = state.coordinate;
      }
      EXPECT_TRUE(crossedZero);
      EXPECT_NEAR(world.jointState(handle)->coordinate, target, 2e-4f);
      EXPECT_EQ(world.activeBodyCount(), 0u);
      expectPosition(world, remote, {30, 0, 0});
    };
    setAngle(start);
    checkLimitedRoute(start, goal);
    // Changing a goal must discard any intermediate backend target from the
    // previous route while leaving the new canonical Position goal visible.
    auto motor = joint.motor;
    motor.targetPosition = start;
    ASSERT_TRUE(world.setJointMotor(handle, motor));
    checkLimitedRoute(world.jointState(handle)->coordinate, start);
    motor.targetPosition = goal;
    ASSERT_TRUE(world.setJointMotor(handle, motor));
    ASSERT_TRUE(world.setJointLimits(handle, false, -3.1f, 3.1f));
    const auto before = world.jointState(handle)->coordinate;
    world.step(kStep);
    EXPECT_GT(direction * (world.jointState(handle)->coordinate - before), 1e-4f)
        << "Removing limits restores the unlimited shortest route immediately";
    simulate(world, 240u);
    EXPECT_NEAR(world.jointState(handle)->coordinate, goal, 2e-4f);
    EXPECT_EQ(world.activeBodyCount(), 0u);
    // The same canonical goal must select a valid constrained route if the
    // allowed interval is enabled again before the next tick.
    setAngle(start);
    ASSERT_TRUE(world.setJointLimits(handle, true, -3.1f, 3.1f));
    checkLimitedRoute(start, goal);
  }
}

TEST(PhysicsWorldTests, ContinuousCollisionStopsFastSphereAtThinStaticWall) {
  PhysicsWorld world(jointWorldSettings());
  (void)world.createBox({0.025f, 2.0f, 2.0f},
      {.motionType = PhysicsMotionType::Static, .position = {0, 1, 0}});
  const auto body = world.createSphere(0.1f,
      {.position = {-5, 1, 0}, .linearVelocity = {500, 0, 0}, .useCCD = true});
  simulate(world, 2u);
  ASSERT_TRUE(world.pose(body));
  EXPECT_LT(world.pose(body)->position.x, -0.025);
  EXPECT_GT(world.pose(body)->position.x, -0.3);
  EXPECT_NEAR(world.linearVelocity(body)->x, 0.0f, 0.1f);
}

} // namespace
