#include "Container/robotics/DriveSensors.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace {

using namespace container::robotics;
constexpr double pi = std::numbers::pi;
constexpr double gravity = 9.81;

void expectVector(glm::dvec3 actual, glm::dvec3 expected, double tolerance = 1e-11) {
  for (glm::length_t axis = 0; axis < 3; ++axis)
    EXPECT_NEAR(actual[axis], expected[axis], tolerance);
}

void expectSameSample(const DriveSensorSample& actual, const DriveSensorSample& expected) {
  EXPECT_EQ(actual.tick, expected.tick);
  EXPECT_DOUBLE_EQ(actual.seconds, expected.seconds);
  EXPECT_EQ(actual.specificForce, expected.specificForce);
  EXPECT_EQ(actual.angularVelocity, expected.angularVelocity);
  EXPECT_EQ(actual.encoderCounts.left, expected.encoderCounts.left);
  EXPECT_EQ(actual.encoderCounts.right, expected.encoderCounts.right);
  EXPECT_DOUBLE_EQ(actual.encoderRadians.left, expected.encoderRadians.left);
  EXPECT_DOUBLE_EQ(actual.encoderRadians.right, expected.encoderRadians.right);
  EXPECT_EQ(actual.odometry.position, expected.odometry.position);
  EXPECT_DOUBLE_EQ(actual.odometry.yaw, expected.odometry.yaw);
}

TEST(DriveSensor, StartsAtRestAndMeasuresWorldAccelerationInBaseAxes) {
  DriveSensorModel sensors(SensorSchedule(10, 10'000), DifferentialDriveSettings{});
  const DrivePose spawn{{7, 12, 2}, pi / 2};
  auto sample = sensors.observe({.tick = 0, .base = spawn});
  ASSERT_TRUE(sample);
  EXPECT_EQ(sample->tick, 0u);
  EXPECT_DOUBLE_EQ(sample->seconds, 0);
  expectVector(sample->specificForce, {0, 0, gravity});
  expectVector(sample->angularVelocity, {0, 0, 0});
  expectVector(sample->odometry.position, {0, 0, 0});
  EXPECT_DOUBLE_EQ(sample->odometry.yaw, 0);

  for (uint64_t tick = 1; tick <= 4; ++tick) {
    SCOPED_TRACE(tick);
    const double seconds = static_cast<double>(tick) / 10;
    const DrivePose base{spawn.position + glm::dvec3(0, .4 * seconds, 0), spawn.yaw};
    sample = sensors.observe({.tick = tick, .base = base, .encoderRadians = {4 * seconds, 4 * seconds}});
    ASSERT_TRUE(sample);
    EXPECT_DOUBLE_EQ(sample->seconds, seconds);
    expectVector(sample->specificForce, {tick == 1 ? 4.0 : 0, 0, gravity});
    expectVector(sample->angularVelocity, {0, 0, 0});
  }
}

TEST(DriveSensor, MeasuresBothTurnSignsAndUsesSuppliedYawRateAcrossWrap) {
  const DifferentialDriveSettings drive;
  constexpr double v = .6;
  constexpr double yaw0 = pi - .03;
  for (const double omega : {1.2, -1.2}) {
    SCOPED_TRACE(omega);
    DriveSensorModel sensors(SensorSchedule(10, 10'000), drive);
    const double radius = v / omega;
    const double delta = omega / 10;
    const double factor = 2 * (1 - std::cos(delta)) * radius * 100;
    for (uint64_t tick = 0; tick <= 4; ++tick) {
      SCOPED_TRACE(tick);
      const double seconds = static_cast<double>(tick) / 10;
      const double theta = omega * seconds;
      const double x = radius * std::sin(theta), y = radius * (1 - std::cos(theta));
      const auto sample = sensors.observe({.tick = tick,
          .base = {{std::cos(yaw0) * x - std::sin(yaw0) * y,
                    std::sin(yaw0) * x + std::cos(yaw0) * y, 0}, std::remainder(yaw0 + theta, 2 * pi)},
          .encoderRadians = {(v - omega * drive.trackWidth / 2) * seconds / drive.wheelRadius,
                             (v + omega * drive.trackWidth / 2) * seconds / drive.wheelRadius},
          .yawRateRadiansPerSecond = tick ? omega : 0});
      ASSERT_TRUE(sample);
      if (tick >= 2)
        expectVector(sample->specificForce, {factor * std::sin(delta), factor * std::cos(delta), gravity});
      expectVector(sample->angularVelocity, {0, 0, tick ? omega : 0});
    }
  }

  // The true rotation per tick exceeds a revolution, so differencing the
  // wrapped orientation would alias even if the wrap itself were handled.
  DriveSensorModel fast(SensorSchedule(10, 10'000), {.maxWheelRadiansPerSecond = 500});
  ASSERT_TRUE(fast.observe({.tick = 0}));
  const auto spin = fast.observe({.tick = 1, .base = {.yaw = std::remainder(8.0, 2 * pi)},
                                 .encoderRadians = {-16, 16}, .yawRateRadiansPerSecond = 80});
  ASSERT_TRUE(spin);
  expectVector(spin->angularVelocity, {0, 0, 80});
  expectVector(spin->specificForce, {0, 0, gravity});
}

TEST(DriveSensor, IntegratesSignedQuantizedCountDeltasBetweenPublishedSamples) {
  const DifferentialDriveSettings drive;
  const DriveSensorSettings settings{.encoders = {.countsPerRevolution = 8}};
  DriveSensorModel dense(SensorSchedule(10, 10'000), drive, settings);
  DriveSensorModel sparse(SensorSchedule(10, 5'000), drive, settings);
  const double rad = 2 * pi / 8;
  const DriveSensorTruth initial{.tick = 0, .encoderRadians = {.49 * rad, -.49 * rad}};
  const auto first = dense.observe(initial);
  ASSERT_TRUE(first);
  EXPECT_EQ(first->encoderCounts.left, 0);
  EXPECT_EQ(first->encoderCounts.right, 0);
  ASSERT_TRUE(sparse.observe(initial));

  const DriveSensorTruth turn{.tick = 1, .encoderRadians = {-1.49 * rad, 1.49 * rad}};
  const auto turned = dense.observe(turn);
  ASSERT_TRUE(turned);
  EXPECT_EQ(turned->encoderCounts.left, -1);
  EXPECT_EQ(turned->encoderCounts.right, 1);
  EXPECT_FALSE(sparse.observe(turn));

  const DriveSensorTruth forward{.tick = 2, .encoderRadians = {1.51 * rad, 3.51 * rad}};
  const auto sample = sparse.observe(forward);
  const auto witness = dense.observe(forward);
  ASSERT_TRUE(sample && witness);
  expectSameSample(*sample, *witness);
  EXPECT_EQ(sample->encoderCounts.left, 2);
  EXPECT_EQ(sample->encoderCounts.right, 4);
  EXPECT_DOUBLE_EQ(sample->encoderRadians.left, 2 * rad);
  EXPECT_DOUBLE_EQ(sample->encoderRadians.right, 4 * rad);
  const double angle = 2 * rad * drive.wheelRadius / drive.trackWidth;
  const double distance = 3 * rad * drive.wheelRadius;
  expectVector(sample->odometry.position, {distance * std::cos(angle), distance * std::sin(angle), 0});
  EXPECT_NEAR(sample->odometry.yaw, angle, 1e-11);

  DriveSensorModel ties(SensorSchedule(10, 10'000), drive, settings);
  const auto halfCount = ties.observe({.tick = 0, .encoderRadians = {-.5 * rad, .5 * rad}});
  ASSERT_TRUE(halfCount);
  EXPECT_EQ(halfCount->encoderCounts.left, -1);
  EXPECT_EQ(halfCount->encoderCounts.right, 1);
}

TEST(DriveSensor, LinearBiasAndDriftAreTimeBasedAndSeededNoiseReplaysAfterReset) {
  const double rad = 2 * pi / 4096;
  DriveSensorSettings settings{.accelerometer = {.bias = {.1, .2, -.05}, .driftPerSecond = {.01, -.02, .03}},
      .gyroscope = {.bias = {-.1, .05, .02}, .driftPerSecond = {.02, .01, -.03}},
      .encoders = {.countsPerRevolution = 4096, .biasRadians = {2 * rad, -rad},
                   .driftRadiansPerSecond = {2 * rad, -3 * rad}}, .seed = 381};
  DriveSensorModel biased(SensorSchedule(10, 1'000), {}, settings);
  const auto first = biased.observe({.tick = 0});
  ASSERT_TRUE(first);
  expectVector(first->specificForce, {.1, .2, gravity - .05});
  expectVector(first->angularVelocity, {-.1, .05, .02});
  EXPECT_EQ(first->encoderCounts.left, 2);
  EXPECT_EQ(first->encoderCounts.right, -1);
  expectVector(first->odometry.position, {0, 0, 0});
  EXPECT_DOUBLE_EQ(first->odometry.yaw, 0);

  for (uint64_t tick = 1; tick < 10; ++tick)
    EXPECT_FALSE(biased.observe({.tick = tick}));
  const auto drifted = biased.observe({.tick = 10});
  ASSERT_TRUE(drifted);
  expectVector(drifted->specificForce, {.11, .18, gravity - .02});
  expectVector(drifted->angularVelocity, {-.08, .06, -.01});
  EXPECT_EQ(drifted->encoderCounts.left, 4);
  EXPECT_EQ(drifted->encoderCounts.right, -4);

  settings.accelerometer.noiseStdDev = {.1, .2, .3};
  settings.gyroscope.noiseStdDev = {.01, .02, .03};
  settings.encoders.noiseStdDevRadians = {.05, .05};
  DriveSensorModel noisy(SensorSchedule(10, 10'000), {}, settings);
  std::vector<DriveSensorSample> reference;
  for (uint64_t tick = 0; tick <= 10; ++tick) {
    const auto sample = noisy.observe({.tick = tick});
    ASSERT_TRUE(sample);
    reference.push_back(*sample);
  }
  EXPECT_NE(reference.back().specificForce, drifted->specificForce);
  EXPECT_NE(reference.back().angularVelocity, drifted->angularVelocity);
  EXPECT_TRUE(reference.back().encoderCounts.left != drifted->encoderCounts.left ||
              reference.back().encoderCounts.right != drifted->encoderCounts.right);
  noisy.reset();
  ++settings.seed;
  DriveSensorModel otherSeed(SensorSchedule(10, 10'000), {}, settings);
  bool changedCounts = false;
  for (const auto& expected : reference) {
    const auto replay = noisy.observe({.tick = expected.tick});
    const auto different = otherSeed.observe({.tick = expected.tick});
    ASSERT_TRUE(replay && different);
    expectSameSample(*replay, expected);
    EXPECT_NE(different->specificForce, expected.specificForce);
    EXPECT_NE(different->angularVelocity, expected.angularVelocity);
    changedCounts |= different->encoderCounts.left != expected.encoderCounts.left ||
                     different->encoderCounts.right != expected.encoderCounts.right;
  }
  EXPECT_TRUE(changedCounts);
}

TEST(DriveSensor, InvalidOrNoncontiguousTicksLeaveHistoryRetryableEvenBetweenSamples) {
  DriveSensorModel sensors(SensorSchedule(10, 5'000), {});
  DriveSensorModel control(SensorSchedule(10, 5'000), {});
  EXPECT_THROW((void)sensors.observe({.tick = 1}), std::invalid_argument);
  const auto initial = sensors.observe({.tick = 0});
  const auto expectedInitial = control.observe({.tick = 0});
  ASSERT_TRUE(initial && expectedInitial);
  expectSameSample(*initial, *expectedInitial);
  EXPECT_THROW((void)sensors.observe({.tick = 0}), std::invalid_argument);
  EXPECT_THROW((void)sensors.observe({.tick = 2}), std::invalid_argument);
  EXPECT_THROW((void)sensors.observe({.tick = 1,
      .base = {.position = {std::numeric_limits<double>::quiet_NaN(), 2, 3}}}), std::invalid_argument);
  EXPECT_THROW((void)sensors.observe({.tick = 1,
      .encoderRadians = {0, std::numeric_limits<double>::infinity()}}), std::invalid_argument);
  EXPECT_THROW((void)sensors.observe({.tick = 1, .encoderRadians = {1e100, 0}}), std::invalid_argument);
  EXPECT_THROW((void)sensors.observe({.tick = 1,
      .yawRateRadiansPerSecond = std::numeric_limits<double>::quiet_NaN()}), std::invalid_argument);

  const DriveSensorTruth next{.tick = 1, .base = {.position = {.04, 0, 0}}, .encoderRadians = {.4, .4}};
  EXPECT_FALSE(sensors.observe(next));
  EXPECT_FALSE(control.observe(next));
  const DriveSensorTruth end{.tick = 2, .base = {.position = {.08, 0, 0}}, .encoderRadians = {.8, .8}};
  const auto resumed = sensors.observe(end);
  const auto expected = control.observe(end);
  ASSERT_TRUE(resumed && expected);
  expectSameSample(*resumed, *expected);
  EXPECT_THROW(DriveSensorModel(SensorSchedule(10, 5'000), {}, {.encoders = {.countsPerRevolution = 0}}),
               std::invalid_argument);
  EXPECT_THROW(DriveSensorModel(SensorSchedule(10, 5'000), {}, {.gyroscope = {.noiseStdDev = {0, 0, -.1}}}),
               std::invalid_argument);
}

} // namespace
