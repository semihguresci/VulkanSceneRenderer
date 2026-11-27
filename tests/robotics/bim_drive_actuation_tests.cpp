#include "Container/geometry/IfcTessellatedLoader.h"
#include "Container/robotics/DifferentialDriveSimulation.h"
#include "Container/robotics/RoboticsScene.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

using namespace container::robotics;
using namespace std::chrono_literals;

constexpr double poseTolerance = 1e-5;
constexpr std::string_view xWallGuid = "0000000000000000000027";

struct CornerFixture {
  CookedBimCollision geometry;
  std::string xWallId;
};

CornerFixture corner() {
  const auto path = std::filesystem::path(CONTAINER_ROBOTICS_FIXTURES) / "lidar-corner.ifc";
  const auto model = container::geometry::ifc::LoadFromFile(path);
  const auto scene = buildRoboticsScene(model, {.sourceNamespace = "drive-actuation-corner"});
  const auto wall = std::ranges::find(scene.entities, xWallGuid, &SemanticEntity::guid);
  if (wall == scene.entities.end()) throw std::runtime_error("The native actuation fixture is missing its X wall");
  return {cookBimCollisionGeometry(model, scene), wall->id};
}

void stepTicks(DifferentialDriveSimulation& robot, uint32_t count) {
  for (uint32_t tick = 0; tick < count; ++tick)
    robot.step();
}

void expectWheels(DriveWheels actual, DriveWheels expected, double tolerance = 1e-10) {
  EXPECT_NEAR(actual.left, expected.left, tolerance);
  EXPECT_NEAR(actual.right, expected.right, tolerance);
}

void expectPose(DrivePose actual, DrivePose expected) {
  for (glm::length_t axis = 0; axis < 3; ++axis)
    EXPECT_NEAR(actual.position[axis], expected.position[axis], poseTolerance);
  EXPECT_NEAR(actual.yaw, expected.yaw, poseTolerance);
}

TEST(BimDriveActuation, WheelSpeedAndShaftSlewSeparateStraightGroundAndEncoderMotion) {
  auto fixture = corner();
  BimCollisionWorld world(std::move(fixture.geometry), {}, 1);
  const DifferentialDriveSimulationOptions options{
      .drive = {.maxWheelRadiansPerSecond = 10}, .maxTicksPerAdvance = 7,
      .actuators = {.maxWheelAccelerationRadiansPerSecondSquared = 20, .slipRatio = {.25, .25}},
      .wheelTravelVariancePerMeter = {.04, .01}};
  const DrivePose spawn{{0, 0, 0}, 0};
  DifferentialDriveSimulation robot(world, spawn, options);
  ASSERT_TRUE(robot.state().commanded);
  expectPose(*robot.state().commanded, spawn);
  EXPECT_EQ(robot.state().odometryCovariance, PlanarOdometryCovariance{});

  EXPECT_THROW(robot.queueWheelCommand(0, {1, std::numeric_limits<double>::infinity()}), std::invalid_argument);
  robot.queueWheelCommand(0, {100, 100});
  EXPECT_THROW(robot.queueCommand(0, {.linear = .5}), std::invalid_argument);
  robot.step();
  expectWheels(robot.state().targetWheelRates, {10, 10});
  expectWheels(robot.state().wheelRates, {1.0 / 3, 1.0 / 3});
  expectWheels(robot.state().groundWheelRates, {.25, .25});
  stepTicks(robot, 59);

  const auto state = robot.state();
  EXPECT_EQ(state.obstruction, BimWalkabilityFailure::None);
  ASSERT_TRUE(state.commanded);
  // The first 30 ticks ramp the shafts to 10 rad/s. The next 30 hold.
  constexpr double shaftRadians = (155.0 + 300.0) / 60;
  constexpr double encoderMetres = shaftRadians * .1;
  expectPose(*state.commanded, {{1, 0, 0}, 0});
  expectPose(state.base, {{.75 * encoderMetres, 0, 0}, 0});
  expectPose(state.odometry, {{encoderMetres, 0, 0}, 0});
  expectWheels(state.targetWheelRates, {10, 10});
  expectWheels(state.wheelRates, {10, 10});
  expectWheels(state.groundWheelRates, {7.5, 7.5});
  expectWheels(state.wheelAngles, {shaftRadians, shaftRadians});
  expectWheels(state.encoderAngles, {shaftRadians, shaftRadians});
  EXPECT_NEAR(state.odometryCovariance[8], encoderMetres * .05 / (.4 * .4), 1e-10);

  const auto manual = robot.history();
  robot.reset();
  EXPECT_EQ(robot.state().odometryCovariance, PlanarOdometryCovariance{});
  robot.setPaused(false);
  for (const auto elapsed : {17ms, 3ms, 391ms, 589ms})
    robot.advance(elapsed);
  ASSERT_GT(robot.clock().pendingTicks(), 0u);
  while (robot.clock().pendingTicks() != 0)
    ASSERT_GT(robot.advance(0ns), 0u);
  ASSERT_EQ(robot.history().size(), manual.size());
  for (size_t i = 0; i < manual.size(); ++i) {
    SCOPED_TRACE(i);
    const auto& expected = manual[i];
    const auto& batched = robot.history()[i];
    ASSERT_TRUE(batched.commanded && expected.commanded);
    EXPECT_EQ(batched.tick, expected.tick);
    expectWheels(batched.wheelRates, expected.wheelRates);
    expectWheels(batched.groundWheelRates, expected.groundWheelRates);
    expectPose(*batched.commanded, *expected.commanded);
    expectPose(batched.base, expected.base);
    expectPose(batched.odometry, expected.odometry);
    EXPECT_EQ(batched.odometryCovariance, expected.odometryCovariance);
  }
}

TEST(BimDriveActuation, AsymmetricSlipChangesGroundArcWhileEncodersReportShaftSpin) {
  auto fixture = corner();
  BimCollisionWorld world(std::move(fixture.geometry), {}, 1);
  const DifferentialDriveSimulationOptions options{.actuators = {.slipRatio = {.25, .5}},
                                                    .wheelTravelVariancePerMeter = {.04, .01}};
  DifferentialDriveSimulation robot(world, {{0, 0, 0}, 0}, options);
  robot.queueWheelCommand(0, {-8, 8});
  stepTicks(robot, 30);

  const auto& state = robot.state();
  EXPECT_EQ(state.obstruction, BimWalkabilityFailure::None);
  ASSERT_TRUE(state.commanded);
  expectWheels(state.wheelRates, {-8, 8});
  expectWheels(state.groundWheelRates, {-6, 4});
  expectWheels(state.encoderAngles, {-4, 4});
  expectPose(*state.commanded, {{0, 0, 0}, 2});
  expectPose(state.odometry, {{0, 0, 0}, 2});
  // The effective wheels drive at -0.1 m/s and +2.5 rad/s, giving a
  // signed radius of -0.04 m for the half-second ground arc.
  expectPose(state.base, {{-.04 * std::sin(1.25), -.04 * (1 - std::cos(1.25)), 0}, 1.25});
  EXPECT_NEAR(state.odometryCovariance[8], .4 * .05 / (.4 * .4), 1e-10);
}

TEST(BimDriveActuation, WallContactBrakesImmediatelyAndAllowsSlewLimitedReverse) {
  auto fixture = corner();
  BimCollisionWorld world(std::move(fixture.geometry), {}, 1);
  const DifferentialDriveSimulationOptions options{
      .actuators = {.maxWheelAccelerationRadiansPerSecondSquared = 12, .tractionCoefficient = .4},
      .wheelTravelVariancePerMeter = {.02, .02}};
  DifferentialDriveSimulation robot(world, {{3, 0, 0}, 0}, options);
  robot.queueWheelCommand(0, {12, 12});
  stepTicks(robot, 150);

  const auto stopped = robot.state();
  EXPECT_EQ(stopped.obstruction, BimWalkabilityFailure::Blocked);
  EXPECT_EQ(stopped.blockingEntityId, fixture.xWallId);
  EXPECT_GT(stopped.base.position.x, 3.6);
  EXPECT_LE(stopped.base.position.x, 3.75 + poseTolerance);
  ASSERT_TRUE(stopped.commanded);
  EXPECT_GT(stopped.commanded->position.x, 4);
  expectWheels(stopped.targetWheelRates, {12, 12});
  expectWheels(stopped.wheelRates, {});
  expectWheels(stopped.groundWheelRates, {});
  stepTicks(robot, 15);
  expectPose(robot.state().base, stopped.base);
  expectWheels(robot.state().encoderAngles, stopped.encoderAngles);
  EXPECT_EQ(robot.state().odometryCovariance, stopped.odometryCovariance);

  robot.queueWheelCommand(robot.clock().tick(), {-6, -6});
  robot.step();
  expectWheels(robot.state().wheelRates, {-.2, -.2});
  expectWheels(robot.state().groundWheelRates, {-.2, -.2});
  EXPECT_LT(robot.state().base.position.x, stopped.base.position.x);
  stepTicks(robot, 29);
  EXPECT_EQ(robot.state().obstruction, BimWalkabilityFailure::None);
  EXPECT_LT(robot.state().base.position.x, stopped.base.position.x - .1);
  EXPECT_GT(robot.state().odometryCovariance[8], stopped.odometryCovariance[8]);
}

} // namespace
