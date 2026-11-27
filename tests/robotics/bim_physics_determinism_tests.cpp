#include "Container/geometry/IfcTessellatedLoader.h"
#include "Container/robotics/DifferentialDriveSimulation.h"
#include "Container/robotics/RoboticsScene.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace container::robotics;
using namespace std::chrono_literals;

constexpr uint32_t tickHz = 60;
constexpr uint32_t batchLimit = 3;
constexpr uint32_t finalTick = 240;
constexpr double floorElevation = 1.2;
// Same build, single worker and identical actions. Jolt contact positions
// need not compare bitwise after native bodies are retired and reinstalled.
constexpr double positionReplayTolerance = 1e-4;
constexpr double wheelReplayTolerance = 1e-4;

struct DoorwayFixture {
  CookedBimCollision geometry;
  std::string doorId;
  std::string propId;
};

DoorwayFixture doorway() {
  const auto path = std::filesystem::path(CONTAINER_ROBOTICS_FIXTURES) / "doorway.ifc";
  const auto model = container::geometry::ifc::LoadFromFile(path);
  const auto scene = buildRoboticsScene(model, {.sourceNamespace = "determinism-doorway"});
  const auto idForGuid = [&](std::string_view guid) {
    const auto found = std::ranges::find(scene.entities, guid, &SemanticEntity::guid);
    if (found == scene.entities.end()) throw std::runtime_error("Missing semantic product in doorway.ifc");
    return found->id;
  };
  return {cookBimCollisionGeometry(model, scene), idForGuid("door-guid"), idForGuid("prop-guid")};
}

void checkPose(const DrivePose& actual, const DrivePose& expected, double tolerance = positionReplayTolerance) {
  for (glm::length_t axis = 0; axis < 3; ++axis)
    EXPECT_NEAR(actual.position[axis], expected.position[axis], tolerance);
  EXPECT_NEAR(actual.yaw, expected.yaw, tolerance);
}

void checkWheels(DriveWheels actual, DriveWheels expected, double tolerance = wheelReplayTolerance) {
  EXPECT_NEAR(actual.left, expected.left, tolerance);
  EXPECT_NEAR(actual.right, expected.right, tolerance);
}

void checkRun(const std::deque<DifferentialDriveState>& expected,
              const std::deque<DifferentialDriveState>& actual) {
  ASSERT_EQ(expected.size(), finalTick + 1);
  ASSERT_EQ(actual.size(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    SCOPED_TRACE(i);
    const auto& first = expected[i];
    const auto& second = actual[i];
    EXPECT_EQ(first.tick, i);
    EXPECT_EQ(second.tick, i);
    EXPECT_DOUBLE_EQ(second.seconds, first.seconds);
    EXPECT_EQ(second.obstruction, first.obstruction);
    EXPECT_EQ(second.blockingEntityId, first.blockingEntityId);
    checkPose(second.base, first.base);
    checkPose(second.odometry, first.odometry);
    ASSERT_TRUE(second.commanded && first.commanded);
    checkPose(*second.commanded, *first.commanded);
    checkWheels(second.targetWheelRates, first.targetWheelRates);
    checkWheels(second.groundWheelRates, first.groundWheelRates);
    checkWheels(second.wheelRates, first.wheelRates);
    checkWheels(second.wheelAngles, first.wheelAngles);
    checkWheels(second.encoderAngles, first.encoderAngles);
    for (size_t cell = 0; cell < first.odometryCovariance.size(); ++cell)
      EXPECT_NEAR(second.odometryCovariance[cell], first.odometryCovariance[cell], 1e-8);
  }
}

void play(DifferentialDriveSimulation& robot, const DifferentialDriveSimulation::TickObserver& observe,
          bool varyingWallTime) {
  ASSERT_EQ(robot.clock().tick(), 0u);
  ASSERT_EQ(robot.clock().pendingTicks(), 0u);
  observe(robot.state());
  if (varyingWallTime) {
    robot.setPaused(false);
    // 69 ms at 60 Hz submits four whole ticks and a .14 tick remainder.
    // The per-advance budget leaves one whole tick queued.
    ASSERT_EQ(robot.advance(69ms, observe), 3u);
    ASSERT_EQ(robot.clock().pendingTicks(), 1u);
    robot.setPaused(true);
    const auto beforePause = robot.state();
    const auto rowsBeforePause = robot.history().size();
    EXPECT_EQ(robot.advance(30s, observe), 0u);
    EXPECT_EQ(robot.clock().tick(), beforePause.tick);
    EXPECT_EQ(robot.history().size(), rowsBeforePause);
    EXPECT_EQ(robot.clock().pendingTicks(), 1u);
    checkPose(robot.state().base, beforePause.base, 0);
    checkWheels(robot.state().encoderAngles, beforePause.encoderAngles, 0);

    robot.step(observe);
    EXPECT_TRUE(robot.clock().paused());
    EXPECT_EQ(robot.clock().tick(), 4u);
    EXPECT_EQ(robot.clock().pendingTicks(), 1u);
    robot.setPaused(false);
    // The .14 tick fraction survives pause and manual stepping. The next
    // 14 ms adds .84 tick and drains the old debt, then 1 ms adds a new tick.
    ASSERT_EQ(robot.advance(14ms, observe), 1u);
    ASSERT_EQ(robot.clock().tick(), 5u);
    ASSERT_EQ(robot.advance(1ms, observe), 1u);
    ASSERT_EQ(robot.clock().tick(), 6u);
    ASSERT_EQ(robot.clock().pendingTicks(), 0u);

    // The explicit paused step is separate from wall time, so wall time only
    // has to generate finalTick - 1 ticks. No actual sleep is needed.
    constexpr auto frameBudget = std::chrono::nanoseconds(
        ((finalTick - 1) * 1'000'000'000LL + tickHz - 1) / tickHz);
    constexpr std::array frames{21ms, 9ms, 973ms, 37ms};
    std::chrono::nanoseconds elapsed = 84ms;
    for (size_t frame = 0; elapsed < frameBudget; ++frame) {
      const auto size = std::min(std::chrono::duration_cast<std::chrono::nanoseconds>(frames[frame % frames.size()]),
                                 frameBudget - elapsed);
      EXPECT_LE(robot.advance(size, observe), batchLimit);
      elapsed += size;
    }
    ASSERT_GT(robot.clock().pendingTicks(), 0u);
    while (robot.clock().pendingTicks() != 0)
      ASSERT_GT(robot.advance(0ns, observe), 0u);
  } else {
    robot.setPaused(true);
    for (uint32_t tick = 0; tick < finalTick; ++tick)
      robot.step(observe);
    EXPECT_TRUE(robot.clock().paused());
  }
  EXPECT_EQ(robot.clock().tick(), finalTick);
  EXPECT_EQ(robot.clock().pendingTicks(), 0u);
  EXPECT_DOUBLE_EQ(robot.clock().seconds(), static_cast<double>(finalTick) / tickHz);
}

TEST(BimPhysicsDeterminism, DynamicIfcFurnitureSettlesOnItsSlabAndRepeatsAfterReset) {
  auto fixture = doorway();
  const auto prop = std::ranges::find(fixture.geometry.colliders, fixture.propId, &BimCollider::entityId);
  ASSERT_NE(prop, fixture.geometry.colliders.end());
  ASSERT_EQ(prop->kind, BimColliderKind::DynamicBox);
  ASSERT_NEAR(prop->originMeters.x, 1.8, 1e-5);
  ASSERT_NEAR(prop->originMeters.y, -2, 1e-5);
  // Raise the cooked copy of the native furniture by .8 m so the same
  // authored box and slab exhibit an actual dynamic impact on every run.
  prop->originMeters.z += .8;
  constexpr double rayStartZ = 3.4; // below the underside of the upper slab
  constexpr double rayLength = 2.5;
  const glm::dvec3 origin{prop->originMeters.x, prop->originMeters.y, rayStartZ};
  const double restingTop = floorElevation + 2 * static_cast<double>(prop->halfExtents.z);
  EXPECT_NEAR(restingTop, 1.65, 1e-5);
  BimCollisionWorld world(std::move(fixture.geometry), {}, 1);
  const auto cast = world.lidarRaycasts();
  const auto colliderCount = world.activeColliderCount();
  const auto shapes = world.cachedStaticMeshCount();
  DifferentialDriveSimulation robot(world, {{-1.5, 0, floorElevation}, 0},
                                      {.ticksPerSecond = tickHz, .maxTicksPerAdvance = batchLimit});

  std::vector<double> tops;
  const auto observe = [&](const DifferentialDriveState& state) {
    SCOPED_TRACE(state.tick);
    EXPECT_EQ(state.tick, tops.size());
    EXPECT_EQ(state.tick, robot.clock().tick());
    EXPECT_EQ(state.obstruction, BimWalkabilityFailure::None);
    const auto hit = cast(origin, {0, 0, -rayLength});
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->entityId, fixture.propId);
    tops.push_back(rayStartZ - rayLength * hit->fraction);
  };
  play(robot, observe, false);
  ASSERT_EQ(tops.size(), finalTick + 1);
  EXPECT_NEAR(tops.front(), restingTop + .8, 1e-5);
  EXPECT_LT(tops[12], tops.front() - .1);
  EXPECT_GT(tops[12], restingTop + .25);
  EXPECT_GE(*std::ranges::min_element(tops), restingTop - .08);
  const auto tail = std::minmax_element(tops.begin() + 180, tops.end());
  EXPECT_NEAR(tops.back(), restingTop, .04);
  EXPECT_LE(*tail.second - *tail.first, .002);
  const auto recorded = std::move(tops);
  const auto history = robot.history();

  robot.reset();
  EXPECT_TRUE(robot.clock().paused());
  EXPECT_EQ(world.activeColliderCount(), colliderCount);
  EXPECT_EQ(world.cachedStaticMeshCount(), shapes);
  tops.clear();
  play(robot, observe, true);
  ASSERT_EQ(tops.size(), recorded.size());
  for (size_t tick = 0; tick < recorded.size(); ++tick) {
    SCOPED_TRACE(tick);
    EXPECT_NEAR(tops[tick], recorded[tick], positionReplayTolerance);
  }
  checkRun(history, robot.history());
}

TEST(BimPhysicsDeterminism, DoorBlocksOnBothSidesAndTickDrivenChangesRepeatAfterReset) {
  auto fixture = doorway();
  BimCollisionWorld world(std::move(fixture.geometry), {}, 1);
  const auto closedColliders = world.activeColliderCount();
  const DrivePose spawn{{-1.5, 0, floorElevation}, 0};
  DifferentialDriveSimulation robot(world, spawn, {.ticksPerSecond = tickHz, .maxTicksPerAdvance = batchLimit,
                                                    .seed = 617, .encoderNoiseRadians = .002});
  robot.queueCommand(0, {.linear = 1});
  robot.queueCommand(168, {.linear = -1});

  const auto observe = [&](const DifferentialDriveState& state) {
    SCOPED_TRACE(state.tick);
    EXPECT_EQ(state.tick, robot.clock().tick());
    EXPECT_DOUBLE_EQ(state.seconds, robot.clock().seconds());
    if (state.tick == 120) {
      EXPECT_EQ(state.obstruction, BimWalkabilityFailure::Blocked);
      EXPECT_EQ(state.blockingEntityId, fixture.doorId);
      world.setDoorOpen(fixture.doorId, true);
      EXPECT_EQ(world.activeColliderCount() + 1, closedColliders);
    } else if (state.tick == 135) {
      EXPECT_NEAR(state.base.position.x, 0, .08);
      EXPECT_THROW(world.setDoorOpen(fixture.doorId, false), std::invalid_argument);
      EXPECT_TRUE(world.isDoorOpen(fixture.doorId));
      EXPECT_EQ(world.activeColliderCount() + 1, closedColliders);
    } else if (state.tick == 168) {
      EXPECT_GT(state.base.position.x, .45);
      EXPECT_EQ(state.obstruction, BimWalkabilityFailure::None);
      world.setDoorOpen(fixture.doorId, false);
      EXPECT_EQ(world.activeColliderCount(), closedColliders);
    }
  };
  play(robot, observe, false);
  const auto recorded = robot.history();
  ASSERT_EQ(recorded.size(), finalTick + 1);
  const auto checkStall = [&](size_t first, size_t last, double direction) {
    const auto& reference = recorded[first];
    EXPECT_NEAR(direction * reference.base.position.x, .28, .05);
    for (size_t tick = first; tick <= last; ++tick) {
      SCOPED_TRACE(tick);
      const auto& state = recorded[tick];
      EXPECT_EQ(state.obstruction, BimWalkabilityFailure::Blocked);
      EXPECT_EQ(state.blockingEntityId, fixture.doorId);
      checkPose(state.base, reference.base, 1e-10);
      checkWheels(state.groundWheelRates, {}, 1e-10);
      checkWheels(state.wheelRates, {}, 1e-10);
      checkWheels(state.encoderAngles, reference.encoderAngles, 1e-10);
    }
  };
  checkStall(100, 120, -1);
  checkStall(205, finalTick, 1);
  EXPECT_EQ(recorded[121].obstruction, BimWalkabilityFailure::None);
  EXPECT_TRUE(recorded[121].blockingEntityId.empty());
  EXPECT_GT(recorded[121].base.position.x, recorded[120].base.position.x);
  EXPECT_LT(recorded[169].base.position.x, recorded[168].base.position.x);

  world.setDoorOpen(fixture.doorId, true);
  ASSERT_TRUE(world.isDoorOpen(fixture.doorId));
  robot.reset();
  EXPECT_TRUE(robot.clock().paused());
  EXPECT_FALSE(world.isDoorOpen(fixture.doorId));
  EXPECT_EQ(world.activeColliderCount(), closedColliders);
  checkPose(robot.state().base, spawn);
  checkWheels(robot.state().encoderAngles, {}, 0);
  play(robot, observe, true);
  checkRun(recorded, robot.history());
}

} // namespace
