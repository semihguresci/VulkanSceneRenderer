#include "Container/geometry/IfcTessellatedLoader.h"
#include "Container/robotics/DifferentialDrive.h"
#include "Container/robotics/DifferentialDriveSimulation.h"
#include "Container/robotics/RoboticsScene.h"
#include "Container/robotics/SimClock.h"

#include <gtest/gtest.h>
#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace {

using namespace container::robotics;
using namespace std::chrono_literals;

constexpr double floorElevation = 1.2;
constexpr uint32_t ticksPerSecond = 60;

struct DoorwayFixture {
  CookedBimCollision collision;
  std::string doorId;
  std::string wallId;
};

DoorwayFixture doorwayFixture() {
  const auto path = std::filesystem::path(CONTAINER_ROBOTICS_FIXTURES) / "doorway.ifc";
  const auto model = container::geometry::ifc::LoadFromFile(path);
  const auto scene = buildRoboticsScene(model, {.sourceNamespace = "drive-doorway"});
  const auto door = std::ranges::find(scene.entities, std::string_view("door-guid"), &SemanticEntity::guid);
  const auto wall = std::ranges::find(scene.entities, std::string_view("wall-guid"), &SemanticEntity::guid);
  if (door == scene.entities.end() || wall == scene.entities.end())
    throw std::runtime_error("The native differential-drive fixture is missing its door or wall");
  return {cookBimCollisionGeometry(model, scene), door->id, wall->id};
}

void stepTicks(DifferentialDriveSimulation& simulation, uint32_t count) {
  for (uint32_t tick = 0; tick < count; ++tick)
    simulation.step();
}

void expectPoseNear(const DrivePose& actual, const DrivePose& expected, double tolerance) {
  for (glm::length_t axis = 0; axis < 3; ++axis)
    EXPECT_NEAR(actual.position[axis], expected.position[axis], tolerance);
  EXPECT_NEAR(actual.yaw, expected.yaw, tolerance);
}

void expectWheelsNear(const DriveWheels& actual, DriveWheels expected, double tolerance) {
  EXPECT_NEAR(actual.left, expected.left, tolerance);
  EXPECT_NEAR(actual.right, expected.right, tolerance);
}

void expectSameHistory(const std::deque<DifferentialDriveState>& expected,
                       const std::deque<DifferentialDriveState>& actual) {
  ASSERT_FALSE(expected.empty());
  ASSERT_EQ(actual.size(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    SCOPED_TRACE(i);
    const auto& a = actual[i];
    const auto& e = expected[i];
    EXPECT_EQ(a.tick, e.tick);
    EXPECT_DOUBLE_EQ(a.seconds, e.seconds);
    expectPoseNear(a.base, e.base, 1e-8);
    expectPoseNear(a.odometry, e.odometry, 1e-8);
    expectWheelsNear(a.wheelAngles, e.wheelAngles, 1e-8);
    expectWheelsNear(a.wheelRates, e.wheelRates, 1e-8);
    expectWheelsNear(a.encoderAngles, e.encoderAngles, 1e-8);
    EXPECT_EQ(a.obstruction, e.obstruction);
    EXPECT_EQ(a.blockingEntityId, e.blockingEntityId);
  }
}

void queueReplay(DifferentialDriveSimulation& simulation) {
  // Asynchronous callers schedule the same command ticks, independent of frame cadence.
  simulation.queueCommand(0, {.linear = .4, .angular = .25});
  simulation.queueCommand(36, {.linear = .3, .angular = -.6});
  simulation.queueCommand(72, {.angular = .75});
  simulation.queueCommand(132, {.linear = -.2, .angular = .2});
}

std::deque<DifferentialDriveState> runAtCadence(const DoorwayFixture& fixture, uint32_t framesPerSecond) {
  const DifferentialDriveSimulationOptions options{.maxTicksPerAdvance = 7, .seed = 1234,
                                                    .encoderNoiseRadians = .001};
  BimCollisionWorld world(fixture.collision, {}, 1);
  DifferentialDriveSimulation simulation(world, {{-2.5, .3, floorElevation}, 0}, options);
  queueReplay(simulation);
  simulation.setPaused(false);
  constexpr auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(3s);
  if (framesPerSecond) {
    const uint32_t frames = 3 * framesPerSecond;
    auto elapsed = 0ns;
    for (uint32_t frame = 1; frame <= frames; ++frame) {
      const auto target = duration * frame / frames;
      EXPECT_LE(simulation.advance(target - elapsed), options.maxTicksPerAdvance);
      elapsed = target;
    }
  } else {
    // Fractional ticks across uneven frames plus a stall larger than the per-frame budget.
    for (const auto elapsed : {13ms, 31ms, 7ms, 2300ms, 111ms, 538ms})
      EXPECT_LE(simulation.advance(elapsed), options.maxTicksPerAdvance);
    EXPECT_GT(simulation.clock().pendingTicks(), 0u);
  }
  while (simulation.clock().pendingTicks() != 0) {
    if (simulation.advance(0ns) == 0) {
      ADD_FAILURE() << "Simulation stopped draining whole-tick backlog";
      break;
    }
  }
  EXPECT_EQ(simulation.clock().tick(), 180u);
  EXPECT_DOUBLE_EQ(simulation.clock().seconds(), 3.0);
  EXPECT_EQ(simulation.state().tick, 180u);
  EXPECT_GE(simulation.history().size(), 180u);
  for (const auto& row : simulation.history())
    EXPECT_EQ(row.obstruction, BimWalkabilityFailure::None);
  return simulation.history();
}

TEST(DifferentialDrive, NativeJoltStraightTurnAndArcMatchWheelAndPoseOracles) {
  const auto fixture = doorwayFixture();
  BimCollisionWorld world(fixture.collision, {}, 1);
  const DrivePose spawn{{-2.0, -1.0, floorElevation}, 0};
  DifferentialDriveSimulation simulation(world, spawn);
  const auto initialOdom = simulation.state().odometry;
  simulation.queueCommand(0, {.linear = .5});
  simulation.queueCommand(ticksPerSecond, {.angular = 1});
  simulation.queueCommand(2 * ticksPerSecond, {.linear = .6, .angular = .75});

  stepTicks(simulation, ticksPerSecond);
  expectPoseNear(simulation.state().base, {{-1.5, -1, floorElevation}, 0}, 1e-5);
  expectWheelsNear(simulation.state().wheelRates, {5, 5}, 1e-6);
  expectWheelsNear(simulation.state().wheelAngles, {5, 5}, 1e-6);
  EXPECT_EQ(simulation.state().obstruction, BimWalkabilityFailure::None);

  stepTicks(simulation, ticksPerSecond);
  expectPoseNear(simulation.state().base, {{-1.5, -1, floorElevation}, 1}, 1e-5);
  expectWheelsNear(simulation.state().wheelRates, {-2, 2}, 1e-6);
  expectWheelsNear(simulation.state().wheelAngles, {3, 7}, 1e-6);

  stepTicks(simulation, ticksPerSecond);
  constexpr double radius = .6 / .75;
  const DrivePose expectedArc{{-1.5 + radius * (std::sin(1.75) - std::sin(1.0)),
                               -1.0 + radius * (std::cos(1.0) - std::cos(1.75)),
                               floorElevation}, 1.75};
  expectPoseNear(simulation.state().base, expectedArc, 1e-5);
  expectWheelsNear(simulation.state().wheelRates, {4.5, 7.5}, 1e-6);
  expectWheelsNear(simulation.state().wheelAngles, {7.5, 14.5}, 1e-6);
  expectWheelsNear(simulation.state().encoderAngles, simulation.state().wheelAngles, 1e-6);
  const auto odometry = simulation.state().odometry;
  EXPECT_NEAR(odometry.position.x - initialOdom.position.x, expectedArc.position.x - spawn.position.x, 1e-5);
  EXPECT_NEAR(odometry.position.y - initialOdom.position.y, expectedArc.position.y - spawn.position.y, 1e-5);
  EXPECT_NEAR(odometry.yaw - initialOdom.yaw, expectedArc.yaw, 1e-5);
  const auto frames = simulation.frames();
  const auto mapFromBase = frames.mapFromOdometry * frames.odometryFromBase;
  EXPECT_NEAR(glm::length(glm::dvec3(mapFromBase[3]) - simulation.state().base.position), 0, 1e-8);
  EXPECT_NEAR(mapFromBase[0].x, std::cos(expectedArc.yaw), 1e-5);
  EXPECT_NEAR(mapFromBase[0].y, std::sin(expectedArc.yaw), 1e-5);
}

TEST(DifferentialDrive, NativeReplayMatchesEveryTickAtThirtySixtyAnd144HzAndAfterStall) {
  const auto fixture = doorwayFixture();
  const auto sixty = runAtCadence(fixture, 60);
  expectSameHistory(sixty, runAtCadence(fixture, 30));
  expectSameHistory(sixty, runAtCadence(fixture, 144));
  expectSameHistory(sixty, runAtCadence(fixture, 0));
}

TEST(DifferentialDrive, SeededEncodersReplayAfterPauseAndResetWithoutMovingGroundTruth) {
  const auto fixture = doorwayFixture();
  const DrivePose spawn{{-2.0, -.5, floorElevation}, 0};
  DifferentialDriveSimulationOptions options{.seed = 41, .encoderNoiseRadians = .02};
  BimCollisionWorld world(fixture.collision, {}, 1);
  DifferentialDriveSimulation simulation(world, spawn, options);
  const auto odomOrigin = simulation.state().odometry;
  simulation.queueCommand(0, {.linear = .5, .angular = .4});
  stepTicks(simulation, ticksPerSecond);
  const auto played = simulation.history();
  ASSERT_FALSE(played.empty());
  const auto first = simulation.state();
  const auto odomTravel = first.odometry.position - odomOrigin.position;
  const auto actualTravel = first.base.position - spawn.position;
  EXPECT_GT(glm::length(odomTravel - actualTravel) +
            std::abs(first.odometry.yaw - odomOrigin.yaw - first.base.yaw), 1e-6);
  EXPECT_GT(std::abs(first.encoderAngles.left - first.wheelAngles.left) +
            std::abs(first.encoderAngles.right - first.wheelAngles.right), 1e-6);

  simulation.reset();
  EXPECT_TRUE(simulation.clock().paused());
  EXPECT_EQ(simulation.clock().tick(), 0u);
  EXPECT_EQ(simulation.advance(1s), 0u);
  expectPoseNear(simulation.state().base, spawn, 1e-5);
  expectWheelsNear(simulation.state().encoderAngles, {}, 1e-12);
  stepTicks(simulation, ticksPerSecond);
  expectSameHistory(played, simulation.history());

  ++options.seed;
  BimCollisionWorld otherWorld(fixture.collision, {}, 1);
  DifferentialDriveSimulation other(otherWorld, spawn, options);
  other.queueCommand(0, {.linear = .5, .angular = .4});
  stepTicks(other, ticksPerSecond);
  expectPoseNear(other.state().base, first.base, 1e-8);
  expectWheelsNear(other.state().wheelAngles, first.wheelAngles, 1e-8);
  EXPECT_GT(std::abs(other.state().encoderAngles.left - first.encoderAngles.left) +
            std::abs(other.state().encoderAngles.right - first.encoderAngles.right), 1e-6);
}

TEST(DifferentialDrive, NativeDoorBlocksThenSameWorldOpeningResumesAndResetRestoresDoor) {
  const auto fixture = doorwayFixture();
  BimCollisionWorld world(fixture.collision, {}, 1);
  const DrivePose spawn{{-1.5, 0, floorElevation}, 0};
  DifferentialDriveSimulation simulation(world, spawn);
  simulation.queueCommand(0, {.linear = 1});
  EXPECT_THROW(simulation.queueCommand(0, {}), std::invalid_argument);
  stepTicks(simulation, 2 * ticksPerSecond);
  EXPECT_THROW(simulation.queueCommand(0, {}), std::invalid_argument);
  const auto blocked = simulation.state();
  EXPECT_GT(blocked.base.position.x, -1.4);
  EXPECT_LT(blocked.base.position.x, -.20);
  EXPECT_NEAR(blocked.base.position.z, floorElevation, 1e-5);
  EXPECT_EQ(blocked.obstruction, BimWalkabilityFailure::Blocked);
  EXPECT_EQ(blocked.blockingEntityId, fixture.doorId);

  world.setDoorOpen(fixture.doorId, true);
  stepTicks(simulation, 2 * ticksPerSecond);
  EXPECT_GT(simulation.state().base.position.x, .75);
  EXPECT_NEAR(simulation.state().base.position.z, floorElevation, 1e-5);
  EXPECT_EQ(simulation.state().obstruction, BimWalkabilityFailure::None);

  simulation.reset();
  EXPECT_FALSE(world.isDoorOpen(fixture.doorId));
  EXPECT_TRUE(simulation.clock().paused());
  expectPoseNear(simulation.state().base, spawn, 1e-5);
  simulation.step();
  EXPECT_EQ(simulation.clock().tick(), 1u);
  EXPECT_GT(simulation.state().base.position.x, spawn.position.x);
}

TEST(DifferentialDrive, OpenNativeDoorPassesCenterlineButStopsAtJambOrLowHeadroom) {
  const auto fixture = doorwayFixture();
  const std::array openDoors{fixture.doorId};
  const auto run = [&](double lateral, const BimWalkabilitySettings& capsule) {
    BimCollisionWorld world(fixture.collision, openDoors, 1);
    DifferentialDriveSimulation simulation(world, {{-1.5, lateral, floorElevation}, 0},
                                             {.capsule = capsule});
    simulation.queueCommand(0, {.linear = 1});
    stepTicks(simulation, 3 * ticksPerSecond);
    return simulation.state();
  };
  const auto center = run(0, {});
  EXPECT_NEAR(center.base.position.x, 1.5, 1e-5);
  EXPECT_NEAR(center.base.position.z, floorElevation, 1e-5);
  EXPECT_EQ(center.obstruction, BimWalkabilityFailure::None);

  for (const auto& obstacle : {run(.45, {}), run(0, {.radius = .20, .height = 2.15})}) {
    EXPECT_GT(obstacle.base.position.x, -1.4);
    EXPECT_LT(obstacle.base.position.x, 0);
    EXPECT_NEAR(obstacle.base.position.z, floorElevation, 1e-5);
    EXPECT_EQ(obstacle.obstruction, BimWalkabilityFailure::Blocked);
    EXPECT_EQ(obstacle.blockingEntityId, fixture.wallId);
  }
}

TEST(DifferentialDrive, ClosingAnOccupiedDoorRollsBackTheColliderChange) {
  const auto fixture = doorwayFixture();
  const std::array openDoors{fixture.doorId};
  BimCollisionWorld world(fixture.collision, openDoors, 1);
  DifferentialDriveSimulation simulation(world, {{0, 0, floorElevation}, 0});
  const auto openColliderCount = world.activeColliderCount();
  EXPECT_THROW(world.setDoorOpen(fixture.doorId, false), std::invalid_argument);
  EXPECT_TRUE(world.isDoorOpen(fixture.doorId));
  EXPECT_EQ(world.activeColliderCount(), openColliderCount);

  simulation.queueCommand(0, {.linear = 1});
  stepTicks(simulation, ticksPerSecond);
  EXPECT_NEAR(simulation.state().base.position.x, 1.0, 1e-5);
  EXPECT_EQ(simulation.state().obstruction, BimWalkabilityFailure::None);
  EXPECT_NO_THROW(world.setDoorOpen(fixture.doorId, false));
  EXPECT_FALSE(world.isDoorOpen(fixture.doorId));
  EXPECT_EQ(world.activeColliderCount(), openColliderCount + 1);
}

TEST(DifferentialDriveClock, BoundedAdvanceKeepsDebtWhilePausedAndManualStepCanRun) {
  SimClock clock(60, 3);
  std::vector<uint64_t> ticks;
  const auto record = [&](uint64_t tick, double seconds) {
    EXPECT_DOUBLE_EQ(seconds, clock.tickSeconds());
    ticks.push_back(tick);
  };
  EXPECT_EQ(clock.advance(100ms, record), 3u);
  EXPECT_EQ(clock.pendingTicks(), 3u);
  clock.setPaused(true);
  EXPECT_EQ(clock.advance(1s, record), 0u);
  EXPECT_EQ(clock.pendingTicks(), 3u);
  clock.setPaused(false);
  EXPECT_EQ(clock.advance(0ns, record), 3u);
  EXPECT_EQ(clock.pendingTicks(), 0u);
  EXPECT_EQ(ticks, (std::vector<uint64_t>{0, 1, 2, 3, 4, 5}));
  EXPECT_DOUBLE_EQ(clock.seconds(), .1);
  clock.reset();
  clock.setPaused(true);
  clock.step(record);
  EXPECT_EQ(clock.tick(), 1u);
  EXPECT_EQ(ticks.back(), 0u);
}

TEST(DifferentialDriveClock, FailedCallbackRetainsTickDebtForRetry) {
  SimClock clock;
  EXPECT_THROW(clock.advance(50ms, [](uint64_t, double) { throw std::runtime_error("tick failed"); }),
               std::runtime_error);
  EXPECT_EQ(clock.tick(), 0u);
  EXPECT_EQ(clock.pendingTicks(), 3u);
  const auto retry = [&](uint64_t, double) {
    EXPECT_THROW(clock.reset(), std::logic_error);
    EXPECT_THROW(clock.step([](uint64_t, double) {}), std::logic_error);
  };
  EXPECT_EQ(clock.advance(0ns, retry), 3u);
  EXPECT_EQ(clock.tick(), 3u);
  EXPECT_EQ(clock.pendingTicks(), 0u);
}

} // namespace
