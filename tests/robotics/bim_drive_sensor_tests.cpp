#include "Container/geometry/IfcTessellatedLoader.h"
#include "Container/robotics/DifferentialDriveSimulation.h"
#include "Container/robotics/DriveSensors.h"
#include "Container/robotics/RoboticsScene.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <vector>

namespace {

using namespace container::robotics;
using namespace std::chrono_literals;

constexpr uint32_t ticksPerSecond = 60;
constexpr double accelerationTolerance = 2e-4;

CookedBimCollision corner() {
  const auto path = std::filesystem::path(CONTAINER_ROBOTICS_FIXTURES) / "lidar-corner.ifc";
  const auto model = container::geometry::ifc::LoadFromFile(path);
  const auto scene = buildRoboticsScene(model, {.sourceNamespace = "drive-sensor-corner"});
  return cookBimCollisionGeometry(model, scene);
}

void queueMotion(DifferentialDriveSimulation& robot) {
  robot.queueCommand(12, {.linear = .6});
  robot.queueCommand(60, {.angular = .75});
  robot.queueCommand(108, {.linear = .4, .angular = .5});
  robot.queueCommand(168, {});
}

std::vector<DriveSensorSample> record(DifferentialDriveSimulation& robot, DriveSensorModel& sensors,
                                     const DifferentialDriveSettings& drive, bool useBatches) {
  std::vector<DriveSensorSample> samples;
  const auto observe = [&](const DifferentialDriveState& state) {
    EXPECT_EQ(state.tick, robot.clock().tick());
    EXPECT_DOUBLE_EQ(state.seconds, robot.clock().seconds());
    EXPECT_EQ(state.obstruction, BimWalkabilityFailure::None);
    const auto yawRate = (state.wheelRates.right - state.wheelRates.left) * drive.wheelRadius / drive.trackWidth;
    if (const auto sample = sensors.observe({.tick = state.tick, .base = state.base,
                                            .encoderRadians = state.encoderAngles,
                                            .yawRateRadiansPerSecond = yawRate})) {
      EXPECT_EQ(sample->tick, state.tick);
      EXPECT_DOUBLE_EQ(sample->seconds, state.seconds);
      samples.push_back(*sample);
    }
  };
  observe(robot.state());
  if (useBatches) {
    robot.setPaused(false);
    for (const auto elapsed : {13ms, 31ms, 7ms, 2300ms, 111ms, 538ms})
      robot.advance(elapsed, observe);
    EXPECT_GT(robot.clock().pendingTicks(), 0u);
    while (robot.clock().pendingTicks() != 0) {
      if (robot.advance(0ns, observe) == 0) {
        ADD_FAILURE() << "Physics stopped draining fixed-tick backlog";
        break;
      }
    }
  } else {
    robot.setPaused(true);
    for (uint32_t tick = 0; tick < 3 * ticksPerSecond; ++tick)
      robot.step(observe);
  }
  EXPECT_EQ(robot.clock().tick(), 180u);
  EXPECT_DOUBLE_EQ(robot.clock().seconds(), 3.0);
  return samples;
}

TEST(BimDriveSensors, NativeCommittedMotionMatchesStationaryStraightSpinAndArcOracles) {
  BimCollisionWorld world(corner(), {}, 1);
  DifferentialDriveSimulation robot(world, {{0, 0, 0}, 0});
  queueMotion(robot);
  const DifferentialDriveSettings drive;
  DriveSensorModel sensors(SensorSchedule(ticksPerSecond, 12'000), drive);
  const auto samples = record(robot, sensors, drive, false);
  ASSERT_EQ(samples.size(), 37u);
  const auto& resting = samples[0];
  EXPECT_EQ(resting.encoderCounts, EncoderCounts{});
  EXPECT_EQ(resting.angularVelocity, glm::dvec3(0.0));
  EXPECT_EQ(resting.specificForce, glm::dvec3(0, 0, 9.81));

  const auto& straight = samples[6];
  ASSERT_EQ(straight.tick, 30u);
  const double radiansPerCount = 2 * std::numbers::pi / sensors.settings().encoders.countsPerRevolution;
  const auto nearestCount = [radiansPerCount](double radians) {
    return static_cast<int64_t>(std::round(radians / radiansPerCount));
  };
  const auto straightCount = nearestCount(1.8);
  EXPECT_EQ(straight.encoderCounts.left, straightCount);
  EXPECT_EQ(straight.encoderCounts.right, straightCount);
  EXPECT_NEAR(straight.odometry.position.x, straightCount * radiansPerCount * drive.wheelRadius, 1e-10);
  EXPECT_NEAR(straight.odometry.position.y, 0, 1e-10);
  EXPECT_NEAR(straight.odometry.yaw, 0, 1e-10);
  EXPECT_NEAR(straight.specificForce.x, 0, accelerationTolerance);
  EXPECT_NEAR(straight.specificForce.y, 0, accelerationTolerance);
  EXPECT_NEAR(straight.specificForce.z, 9.81, accelerationTolerance);
  EXPECT_EQ(straight.angularVelocity, glm::dvec3(0.0));

  const auto& spin = samples[18];
  ASSERT_EQ(spin.tick, 90u);
  EXPECT_NEAR(spin.angularVelocity.z, .75, 1e-10);
  EXPECT_NEAR(spin.specificForce.x, 0, accelerationTolerance);
  EXPECT_NEAR(spin.specificForce.y, 0, accelerationTolerance);
  EXPECT_NEAR(spin.odometry.position.x, .48, .001);
  EXPECT_NEAR(spin.odometry.position.y, 0, .001);
  EXPECT_NEAR(spin.odometry.yaw, .375, .002);
  EXPECT_EQ(spin.encoderCounts.left, nearestCount(4.8 - .75));
  EXPECT_EQ(spin.encoderCounts.right, nearestCount(4.8 + .75));

  const auto& arc = samples[26];
  ASSERT_EQ(arc.tick, 130u);
  EXPECT_NEAR(arc.angularVelocity.z, .5, 1e-10);
  EXPECT_NEAR(arc.specificForce.x, .2 * std::sin(.5 / ticksPerSecond), accelerationTolerance);
  EXPECT_NEAR(arc.specificForce.y, .2 * std::cos(.5 / ticksPerSecond), accelerationTolerance);
  EXPECT_NEAR(arc.specificForce.z, 9.81, accelerationTolerance);

  const auto& stopped = samples.back();
  EXPECT_EQ(stopped.tick, 180u);
  EXPECT_EQ(stopped.angularVelocity, glm::dvec3(0.0));
  EXPECT_NEAR(stopped.specificForce.z, 9.81, accelerationTolerance);
  const double finalX = .48 + .8 * (std::sin(1.1) - std::sin(.6));
  const double finalY = .8 * (std::cos(.6) - std::cos(1.1));
  EXPECT_NEAR(robot.state().base.position.x, finalX, 1e-5);
  EXPECT_NEAR(robot.state().base.position.y, finalY, 1e-5);
  EXPECT_NEAR(robot.state().base.yaw, 1.1, 1e-5);
  EXPECT_NEAR(stopped.odometry.position.x, finalX, .002);
  EXPECT_NEAR(stopped.odometry.position.y, finalY, .002);
  EXPECT_NEAR(stopped.odometry.yaw, 1.1, .002);
}

TEST(BimDriveSensors, ResetAndIrregularBatchesReplayEveryPublishedNoisySample) {
  BimCollisionWorld world(corner(), {}, 1);
  const DifferentialDriveSimulationOptions options{.maxTicksPerAdvance = 7, .seed = 831,
                                                    .encoderNoiseRadians = .002};
  DifferentialDriveSimulation robot(world, {{0, 0, 0}, 0}, options);
  queueMotion(robot);
  const DriveSensorSettings settings{
      .accelerometer = {.bias = {.01, .02, .03}, .noiseStdDev = {.002, .003, .004}},
      .gyroscope = {.noiseStdDev = {.001, .001, .002}, .driftPerSecond = {0, 0, -.005}},
      .encoders = {.noiseStdDevRadians = {.004, .004}}, .seed = 253};
  DriveSensorModel sensors(SensorSchedule(ticksPerSecond, 13'000, 3), options.drive, settings);
  const auto stepped = record(robot, sensors, options.drive, false);
  ASSERT_EQ(stepped.size(), 39u);
  EXPECT_EQ(stepped.front().tick, 3u);
  EXPECT_EQ(stepped.back().tick, 179u);

  robot.reset();
  sensors.reset();
  const auto batched = record(robot, sensors, options.drive, true);
  ASSERT_EQ(batched.size(), stepped.size());
  for (size_t i = 0; i < stepped.size(); ++i) {
    SCOPED_TRACE(i);
    const auto& expected = stepped[i];
    const auto& actual = batched[i];
    EXPECT_EQ(actual.tick, 3 + (i * 60 + 12) / 13);
    EXPECT_EQ(actual.tick, expected.tick);
    EXPECT_DOUBLE_EQ(actual.seconds, expected.seconds);
    EXPECT_EQ(actual.specificForce, expected.specificForce);
    EXPECT_EQ(actual.angularVelocity, expected.angularVelocity);
    EXPECT_EQ(actual.encoderCounts, expected.encoderCounts);
    EXPECT_DOUBLE_EQ(actual.encoderRadians.left, expected.encoderRadians.left);
    EXPECT_DOUBLE_EQ(actual.encoderRadians.right, expected.encoderRadians.right);
    EXPECT_EQ(actual.odometry.position, expected.odometry.position);
    EXPECT_DOUBLE_EQ(actual.odometry.yaw, expected.odometry.yaw);
  }
}

} // namespace
