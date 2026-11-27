#include "Container/geometry/IfcTessellatedLoader.h"
#include "Container/robotics/BimCollisionWorld.h"
#include "Container/robotics/DifferentialDriveSimulation.h"
#include "Container/robotics/RaycastLidar.h"
#include "Container/robotics/RoboticsScene.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace container::robotics;
using namespace std::chrono_literals;

constexpr double pi = std::numbers::pi;
constexpr double lidarHeight = 1.25;
constexpr double hitTolerance = 1e-5;
constexpr std::string_view xWallGuid = "0000000000000000000027";
constexpr std::string_view yWallGuid = "0000000000000000000037";
constexpr std::string_view slabGuid = "0000000000000000000017";

struct BimFixture {
  RoboticsScene scene;
  CookedBimCollision collision;
};

BimFixture bimFixture(const char* name) {
  const auto path = std::filesystem::path(CONTAINER_ROBOTICS_FIXTURES) / name;
  const auto model = container::geometry::ifc::LoadFromFile(path);
  auto scene = buildRoboticsScene(model, {.sourceNamespace = path.stem().string()});
  auto collision = cookBimCollisionGeometry(model, scene);
  return {std::move(scene), std::move(collision)};
}

const SemanticEntity* byGuid(const RoboticsScene& scene, std::string_view guid) {
  const auto entity = std::ranges::find(scene.entities, guid, &SemanticEntity::guid);
  return entity == scene.entities.end() ? nullptr : &*entity;
}

void expectHit(const LidarReturn& result, double metres, const SemanticEntity& entity) {
  EXPECT_EQ(result.status, LidarReturnStatus::Valid);
  ASSERT_TRUE(result.rangeMeters);
  EXPECT_NEAR(*result.rangeMeters, metres, hitTolerance);
  EXPECT_EQ(result.entityId, entity.id);
}

TEST(BimLidar, NativeMillimetreCornerProducesPlanarAndElevatedMetricReturns) {
  auto fixture = bimFixture("lidar-corner.ifc");
  const auto* xWall = byGuid(fixture.scene, xWallGuid);
  const auto* yWall = byGuid(fixture.scene, yWallGuid);
  const auto* slab = byGuid(fixture.scene, slabGuid);
  ASSERT_TRUE(xWall && yWall && slab);
  ASSERT_TRUE(fixture.scene.frame.sourceMetersPerUnit);
  EXPECT_DOUBLE_EQ(*fixture.scene.frame.sourceMetersPerUnit, .001);
  BimCollisionWorld world(std::move(fixture.collision), {}, 1);
  const auto cast = world.lidarRaycasts();
  const LidarSchedule schedule(60, 10'000);
  const LidarPose scanner{.position = {0, 0, lidarHeight}};
  const RaycastLidar planar({.azimuthSamples = 8, .azimuthStart = 0, .maxRangeMeters = 8});
  const auto scan = planar.scan(scanner, schedule, 0, cast);
  ASSERT_EQ(scan.returns.size(), 8u);
  expectHit(scan.returns[0], 4.0, *xWall);
  expectHit(scan.returns[1], 3 * std::sqrt(2.0), *yWall);
  expectHit(scan.returns[2], 3.0, *yWall);
  for (size_t beam = 3; beam < scan.returns.size(); ++beam) {
    SCOPED_TRACE(beam);
    EXPECT_EQ(scan.returns[beam], LidarReturn{});
  }

  const RaycastLidar vertical({.azimuthSamples = 1, .azimuthStart = 0,
                               .elevationAngles = {pi / 12, pi / 6, -pi / 6}, .maxRangeMeters = 8});
  const auto verticalScan = vertical.scan(scanner, schedule, 0, cast);
  ASSERT_EQ(verticalScan.returns.size(), 3u);
  expectHit(verticalScan.returns[0], 4 / std::cos(pi / 12), *xWall);
  EXPECT_EQ(verticalScan.returns[1], LidarReturn{});
  expectHit(verticalScan.returns[2], 2.5, *slab);
}

TEST(BimLidar, OneWorldQueryTracksDoorGeometryChanges) {
  auto fixture = bimFixture("doorway.ifc");
  const auto* door = byGuid(fixture.scene, "door-guid");
  const auto* wall = byGuid(fixture.scene, "wall-guid");
  ASSERT_TRUE(door && wall);
  BimCollisionWorld world(std::move(fixture.collision), {}, 1);
  const auto cast = world.lidarRaycasts();
  const RaycastLidar lidar({.azimuthSamples = 1, .azimuthStart = 0, .maxRangeMeters = 3});
  const LidarSchedule schedule(60, 60'000);
  const LidarPose threshold{.position = {-1.5, 0, 2}};
  const LidarPose jamb{.position = {-1.5, .9, 2}};
  expectHit(lidar.scan(threshold, schedule, 0, cast).returns.at(0), 1.47, *door);
  world.setDoorOpen(door->id, true);
  EXPECT_EQ(lidar.scan(threshold, schedule, 1, cast).returns.at(0), LidarReturn{});
  expectHit(lidar.scan(jamb, schedule, 1, cast).returns.at(0), 1.4, *wall);
  world.setDoorOpen(door->id, false);
  expectHit(lidar.scan(threshold, schedule, 2, cast).returns.at(0), 1.47, *door);
}

TEST(BimLidar, TickScansFollowTheMovingRobotAndIgnoreItsCurrentColliderAfterReset) {
  auto fixture = bimFixture("lidar-corner.ifc");
  const auto* xWall = byGuid(fixture.scene, xWallGuid);
  ASSERT_NE(xWall, nullptr);
  BimCollisionWorld world(std::move(fixture.collision), {}, 1);
  // Reuse the callback across robot attachment and reset. The beam from an
  // external origin crosses the capsule before reaching the BIM wall.
  const auto cast = world.lidarRaycasts();
  const LidarSchedule everyTick(60, 60'000);
  const RaycastLidar unmounted({.azimuthSamples = 1, .azimuthStart = 0, .maxRangeMeters = 8});
  const LidarPose exterior{.position = {-1, 0, lidarHeight}};
  expectHit(unmounted.scan(exterior, everyTick, 0, cast).returns.at(0), 5.0, *xWall);
  DifferentialDriveSimulation robot(world, {{0, 0, 0}, 0});
  expectHit(unmounted.scan(exterior, everyTick, 0, cast).returns.at(0), 5.0, *xWall);
  robot.queueCommand(0, {.linear = .6});

  const RaycastLidar mounted({.azimuthSamples = 1, .azimuthStart = 0, .maxRangeMeters = 8,
                              .baseFromSensor = {.position = {0, 0, lidarHeight}}});
  const LidarSchedule sensorClock(60, 10'000, 6);
  std::vector<LidarScan> scans;
  const auto observe = [&](const DifferentialDriveState& state) {
    EXPECT_EQ(state.tick, robot.clock().tick());
    EXPECT_DOUBLE_EQ(state.seconds, robot.clock().seconds());
    if (!sensorClock.due(state.tick)) return;
    const auto rotation = glm::angleAxis(state.base.yaw, glm::dvec3(0, 0, 1));
    scans.push_back(mounted.scan({state.base.position, rotation}, sensorClock, state.tick, cast));
  };
  robot.setPaused(false);
  EXPECT_EQ(robot.advance(1s, observe), 60u);
  ASSERT_EQ(scans.size(), 10u);
  EXPECT_EQ(robot.state().obstruction, BimWalkabilityFailure::None);
  for (size_t i = 0; i < scans.size(); ++i) {
    SCOPED_TRACE(i);
    const auto& scan = scans[i];
    EXPECT_EQ(scan.tick, 6 * (i + 1));
    EXPECT_DOUBLE_EQ(scan.seconds, static_cast<double>(scan.tick) / 60.0);
    const double metresTravelled = .6 * scan.seconds;
    EXPECT_NEAR(scan.worldFromSensor.position.x, metresTravelled, hitTolerance);
    EXPECT_NEAR(scan.worldFromSensor.position.z, lidarHeight, hitTolerance);
    expectHit(scan.returns.at(0), 4 - metresTravelled, *xWall);
  }

  auto firstPass = std::move(scans);
  scans.clear();
  robot.reset();
  EXPECT_TRUE(robot.clock().paused());
  expectHit(unmounted.scan(exterior, everyTick, 0, cast).returns.at(0), 5.0, *xWall);
  for (uint64_t tick = 0; tick < 60; ++tick)
    robot.step(observe);
  ASSERT_EQ(scans.size(), firstPass.size());
  for (size_t i = 0; i < scans.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_EQ(scans[i].tick, firstPass[i].tick);
    EXPECT_DOUBLE_EQ(scans[i].seconds, firstPass[i].seconds);
    EXPECT_EQ(scans[i].returns, firstPass[i].returns);
  }
}

TEST(BimLidar, ThrowingObserverKeepsCompletedPhysicsTickAndDrainsOnlyRemainingTime) {
  auto fixture = bimFixture("lidar-corner.ifc");
  BimCollisionWorld world(std::move(fixture.collision), {}, 1);
  DifferentialDriveSimulation robot(world, {{0, 0, 0}, 0});
  robot.queueCommand(0, {.linear = .6});
  robot.setPaused(false);

  struct SensorError : std::runtime_error { using std::runtime_error::runtime_error; };
  const auto failingObserver = [&](const DifferentialDriveState& state) {
    EXPECT_EQ(state.tick, 1u);
    EXPECT_EQ(robot.clock().tick(), state.tick);
    EXPECT_THROW(robot.reset(), std::logic_error);
    EXPECT_THROW(robot.advance(0ns), std::logic_error);
    EXPECT_EQ(robot.state().tick, 1u);
    EXPECT_EQ(robot.history().size(), 2u);
    EXPECT_NEAR(state.base.position.x, .01, hitTolerance);
    throw SensorError("Rejected scan after completed tick");
  };
  EXPECT_THROW(robot.advance(100ms, failingObserver), SensorError);
  EXPECT_EQ(robot.clock().tick(), 1u);
  EXPECT_EQ(robot.clock().pendingTicks(), 5u);
  EXPECT_EQ(robot.state().tick, 1u);
  ASSERT_EQ(robot.history().size(), 2u);
  EXPECT_EQ(robot.history().back().tick, 1u);

  std::vector<uint64_t> observed;
  EXPECT_EQ(robot.advance(0ns, [&](const DifferentialDriveState& state) { observed.push_back(state.tick); }), 5u);
  EXPECT_EQ(observed, (std::vector<uint64_t>{2, 3, 4, 5, 6}));
  EXPECT_EQ(robot.clock().pendingTicks(), 0u);
  EXPECT_DOUBLE_EQ(robot.state().seconds, .1);
  EXPECT_NEAR(robot.state().base.position.x, .06, hitTolerance);
  EXPECT_THROW(robot.step([](const DifferentialDriveState&) { throw SensorError("Manual scan failed"); }),
               SensorError);
  EXPECT_EQ(robot.clock().tick(), 7u);
  EXPECT_EQ(robot.state().tick, 7u);
  EXPECT_NEAR(robot.state().base.position.x, .07, hitTolerance);
}

} // namespace
