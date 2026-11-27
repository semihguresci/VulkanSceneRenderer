#include "Container/geometry/IfcTessellatedLoader.h"
#include "Container/robotics/DifferentialDrive.h"
#include "Container/robotics/DifferentialDriveSimulation.h"
#include "Container/robotics/RoboticsScene.h"
#include "Container/robotics/SimClock.h"

#include <gtest/gtest.h>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <filesystem>
#include <stdexcept>
#include <utility>
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
    EXPECT_EQ(a.contact, e.contact);
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
  ASSERT_TRUE(blocked.contact);
  EXPECT_NEAR(blocked.contact->position.x, -.03, .003);
  EXPECT_NEAR(blocked.contact->position.y, 0, .05);
  EXPECT_GE(blocked.contact->position.z, floorElevation);
  EXPECT_LE(blocked.contact->position.z, floorElevation + 1.6);
  ASSERT_TRUE(blocked.contact->normal);
  EXPECT_LT(blocked.contact->normal->x, -.99);
  EXPECT_NEAR(glm::length(*blocked.contact->normal), 1, 1e-6);

  world.setDoorOpen(fixture.doorId, true);
  simulation.step();
  EXPECT_FALSE(simulation.state().contact);
  stepTicks(simulation, 2 * ticksPerSecond - 1);
  EXPECT_GT(simulation.state().base.position.x, .75);
  EXPECT_NEAR(simulation.state().base.position.z, floorElevation, 1e-5);
  EXPECT_EQ(simulation.state().obstruction, BimWalkabilityFailure::None);

  simulation.reset();
  EXPECT_FALSE(world.isDoorOpen(fixture.doorId));
  EXPECT_TRUE(simulation.clock().paused());
  expectPoseNear(simulation.state().base, spawn, 1e-5);
  EXPECT_FALSE(simulation.state().contact);
  simulation.step();
  EXPECT_EQ(simulation.clock().tick(), 1u);
  EXPECT_GT(simulation.state().base.position.x, spawn.position.x);
  stepTicks(simulation, 2 * ticksPerSecond - 1);
  EXPECT_EQ(simulation.state().contact, blocked.contact);
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

FleetRobotSpawn fleetRobot(std::string id, DrivePose pose, uint32_t rate = ticksPerSecond) {
  return {.spawn = pose, .options = {.ticksPerSecond = rate, .robotId = std::move(id)}};
}

TEST(DifferentialDriveFleet, SweptCrossingStopsBothRobotsEvenWhenEndpointsAreClear) {
  const auto fixture = doorwayFixture();
  const auto a = fleetRobot("a", {{-2.4, -1.0, floorElevation}, 0}, 1);
  const auto b = fleetRobot("b", {{-1.7, -1.7, floorElevation}, glm::half_pi<double>()}, 1);
  const auto run = [&](std::vector<FleetRobotSpawn> spawns) {
    BimCollisionWorld world(fixture.collision, {}, 1, 2);
    DifferentialDriveFleetSimulation fleet(world, std::move(spawns));
    fleet.queueCommand("a", 0, {.linear = 1.4});
    fleet.queueCommand("b", 0, {.linear = 1.4});
    fleet.step();
    EXPECT_EQ(fleet.clock().tick(), 1u);
    for (const auto& [spawn, blocker] : {std::pair{&a, "b"}, std::pair{&b, "a"}}) {
      const auto& state = fleet.state(spawn->options.robotId);
      EXPECT_EQ(state.tick, 1u);
      expectPoseNear(state.base, spawn->spawn, 1e-5);
      EXPECT_EQ(state.obstruction, BimWalkabilityFailure::Blocked);
      EXPECT_EQ(state.blockingEntityId, blocker);
      EXPECT_FALSE(state.contact);
      expectWheelsNear(state.encoderAngles, {}, 1e-12);
      EXPECT_TRUE(state.commanded.has_value());
      if (state.commanded)
        EXPECT_NEAR(glm::length(state.commanded->position - state.base.position), 1.4, 1e-5);
    }
    return std::pair{fleet.history("a"), fleet.history("b")};
  };
  const auto forward = run({a, b});
  const auto reversed = run({b, a});
  expectSameHistory(forward.first, reversed.first);
  expectSameHistory(forward.second, reversed.second);
}

TEST(DifferentialDriveFleet, RechecksRoutesAfterStoppingAFollowingPeer) {
  const auto fixture = doorwayFixture();
  BimCollisionWorld world(fixture.collision, {}, 1, 3);
  DifferentialDriveFleetSimulation fleet(world, {
      fleetRobot("a", {{-2.0, -1, floorElevation}, 0}, 1),
      fleetRobot("b", {{-1.35, -1, floorElevation}, 0}, 1),
      fleetRobot("c", {{-.70, -1, floorElevation}, 0}, 1),
  });
  const auto parked = fleet.state("c").base.position;
  fleet.queueCommand("a", 0, {.linear = .6});
  fleet.queueCommand("b", 0, {.linear = .6});
  fleet.step();
  EXPECT_EQ(fleet.state("a").blockingEntityId, "b");
  EXPECT_EQ(fleet.state("b").blockingEntityId, "c");
  EXPECT_EQ(fleet.state("c").obstruction, BimWalkabilityFailure::None);
  EXPECT_DOUBLE_EQ(fleet.state("a").base.position.x, -2.0);
  EXPECT_DOUBLE_EQ(fleet.state("b").base.position.x, -1.35);
  EXPECT_EQ(fleet.state("c").base.position, parked);
}

TEST(DifferentialDriveFleet, StackedFloorsShareXYPositionsAndResetDeterministically) {
  const auto fixture = doorwayFixture();
  BimCollisionWorld world(fixture.collision, {}, 1, 2);
  DifferentialDriveFleetSimulation fleet(world, {
      fleetRobot("upper", {{-2, -1, 3.7}, 0}),
      fleetRobot("lower", {{-2, -1, floorElevation}, 0}),
  });
  fleet.queueCommand("lower", 0, {.linear = .6});
  fleet.queueCommand("upper", 0, {.linear = .6});
  for (uint32_t tick = 0; tick < 30; ++tick)
    fleet.step();
  for (const auto id : {"lower", "upper"}) {
    const auto& state = fleet.state(id);
    EXPECT_EQ(state.obstruction, BimWalkabilityFailure::None);
    EXPECT_NEAR(state.base.position.x, -1.7, 1e-5);
    const auto body = world.robotPose(id);
    ASSERT_TRUE(body);
    EXPECT_NEAR(body->position.x, state.base.position.x, 1e-5);
    EXPECT_NEAR(body->position.y, state.base.position.y, 1e-5);
  }
  EXPECT_NEAR(fleet.state("lower").base.position.z, floorElevation, 1e-5);
  EXPECT_NEAR(fleet.state("upper").base.position.z, 3.7, 1e-5);
  const auto lower = fleet.history("lower"), upper = fleet.history("upper");
  fleet.reset();
  EXPECT_EQ(world.robotCount(), 2u);
  EXPECT_TRUE(fleet.clock().paused());
  fleet.setPaused(false);
  EXPECT_EQ(fleet.advance(500ms), 30u);
  expectSameHistory(lower, fleet.history("lower"));
  expectSameHistory(upper, fleet.history("upper"));
}

TEST(DifferentialDriveFleet, SharesOnePhysicsStepPerCommittedTick) {
  auto fixture = doorwayFixture();
  const auto prop = std::ranges::find(fixture.collision.colliders, BimColliderKind::DynamicBox, &BimCollider::kind);
  ASSERT_NE(prop, fixture.collision.colliders.end());
  prop->originMeters.z = 6.0;
  const auto rayOrigin = prop->originMeters + glm::dvec3(0, 0, 2);
  const auto probe = [&](const BimCollisionWorld& world) {
    const auto hit = world.walkabilityQueries().castSegment(rayOrigin, {0, 0, -3});
    EXPECT_TRUE(hit);
    return hit ? hit->position.z : 0.0;
  };
  BimCollisionWorld singleWorld(fixture.collision, {}, 1);
  BimCollisionWorld fleetWorld(fixture.collision, {}, 1, 2);
  const auto a = fleetRobot("a", {{-2, -.8, floorElevation}, 0});
  const auto b = fleetRobot("b", {{-1, .8, floorElevation}, 0});
  DifferentialDriveSimulation single(singleWorld, a.spawn, a.options);
  DifferentialDriveFleetSimulation fleet(fleetWorld, {a, b});
  const auto start = probe(singleWorld);
  EXPECT_NEAR(start, probe(fleetWorld), 1e-5);
  single.step();
  fleet.step();
  const auto next = probe(singleWorld);
  EXPECT_LT(next, start - .001);
  EXPECT_NEAR(next, probe(fleetWorld), 1e-5);
  EXPECT_EQ(fleet.state("a").tick, 1u);
  EXPECT_EQ(fleet.state("b").tick, 1u);
}

TEST(DifferentialDriveFleet, CleansUpPartialAttachmentsAndResolvesPeersAndDoorOccupancy) {
  const auto fixture = doorwayFixture();
  const std::array open{fixture.doorId};
  BimCollisionWorld world(fixture.collision, open, 1, 2);
  const auto a = fleetRobot("a", {{-1.5, 0, floorElevation}, 0});
  const auto b = fleetRobot("b", {{0, 0, floorElevation}, 0});
  const auto overlap = fleetRobot("b", a.spawn);
  EXPECT_THROW(DifferentialDriveFleetSimulation(world, {a, overlap}), std::invalid_argument);
  auto invalid = b;
  invalid.options.robotId = fixture.wallId;
  EXPECT_THROW(DifferentialDriveFleetSimulation(world, {a, invalid}), std::invalid_argument);
  EXPECT_EQ(world.robotCount(), 0u);
  {
    DifferentialDriveFleetSimulation fleet(world, {b, a});
    EXPECT_EQ(world.robotCount(), 2u);
    EXPECT_THROW(DifferentialDriveSimulation(world, a.spawn, a.options), std::invalid_argument);
    EXPECT_THROW((void)fleet.state("missing"), std::invalid_argument);
    const auto ray = world.walkabilityQueries("a").castSegment({-1.5, 0, 2}, {2, 0, 0});
    ASSERT_TRUE(ray);
    EXPECT_EQ(ray->entityId, "b");
    EXPECT_FALSE(ray->walkableSupport);
    const auto scan = world.lidarRaycasts("a")({-1.5, 0, 2}, {2, 0, 0});
    ASSERT_TRUE(scan);
    EXPECT_EQ(scan->entityId, "b");
    EXPECT_THROW(world.setDoorOpen(fixture.doorId, false), std::invalid_argument);
    EXPECT_TRUE(world.isDoorOpen(fixture.doorId));
  }
  EXPECT_EQ(world.robotCount(), 0u);
  EXPECT_NO_THROW(world.setDoorOpen(fixture.doorId, false));
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
