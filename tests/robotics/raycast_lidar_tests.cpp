#include "Container/robotics/RaycastLidar.h"

#include <gtest/gtest.h>
#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {

using namespace container::robotics;
constexpr double pi = std::numbers::pi;

void expectVector(glm::dvec3 actual, glm::dvec3 expected) {
  for (glm::length_t axis = 0; axis < 3; ++axis)
    EXPECT_NEAR(actual[axis], expected[axis], 1e-12);
}

TEST(RaycastLidar, RoundsToFirstEligibleTickWithoutDriftOrIntegerWrap) {
  const LidarSchedule schedule(60, 7'000, 2);
  const std::array<uint64_t, 8> due{2, 11, 20, 28, 37, 45, 54, 62};
  uint64_t counted = 0;
  for (uint64_t tick = 0; tick <= due.back(); ++tick) {
    SCOPED_TRACE(tick);
    const bool expected = std::ranges::find(due, tick) != due.end();
    counted += expected;
    EXPECT_EQ(schedule.due(tick), expected);
    EXPECT_EQ(schedule.scansThrough(tick), counted);
    EXPECT_DOUBLE_EQ(schedule.seconds(tick), static_cast<double>(tick) / 60.0);
  }
  EXPECT_EQ(schedule.scansThrough(60'002), 7'001u);
  EXPECT_TRUE(schedule.due(60'002));
  EXPECT_FALSE(schedule.due(60'001));

  const LidarSchedule everySecondTick(32, 16'000);
  constexpr auto lastTick = std::numeric_limits<uint64_t>::max();
  EXPECT_TRUE(everySecondTick.due(lastTick - 1));
  EXPECT_FALSE(everySecondTick.due(lastTick));
  EXPECT_EQ(everySecondTick.scansThrough(lastTick), uint64_t{1} << 63);
  EXPECT_THROW(LidarSchedule(0, 1), std::invalid_argument);
  EXPECT_THROW(LidarSchedule(60, 0), std::invalid_argument);
  EXPECT_THROW(LidarSchedule(60, 60'001), std::invalid_argument);
}

TEST(RaycastLidar, SamplingUsesRep103AxesAndRetainsAuthoredChannelOrder) {
  const RaycastLidar full({.azimuthSamples = 4, .azimuthStart = 0});
  const auto around = full.directions();
  ASSERT_EQ(around.size(), 4u);
  expectVector(around[0], {1, 0, 0});
  expectVector(around[1], {0, 1, 0});
  expectVector(around[2], {-1, 0, 0});
  expectVector(around[3], {0, -1, 0});

  const RaycastLidar partial({.azimuthSamples = 3, .azimuthStart = -pi / 2, .azimuthSpan = pi,
                             .elevationAngles = {pi / 6, -pi / 6}});
  const auto grid = partial.directions();
  ASSERT_EQ(grid.size(), 6u);
  const double horizontal = std::sqrt(3.0) / 2;
  expectVector(grid[0], {0, -horizontal, .5});
  expectVector(grid[1], {horizontal, 0, .5});
  expectVector(grid[2], {0, horizontal, .5});
  expectVector(grid[3], {0, -horizontal, -.5});
  expectVector(grid[4], {horizontal, 0, -.5});
  expectVector(grid[5], {0, horizontal, -.5});
  for (const auto direction : grid)
    EXPECT_NEAR(glm::length(direction), 1.0, 1e-12);

  const RaycastLidar one({.azimuthSamples = 1, .azimuthStart = pi / 2, .azimuthSpan = pi});
  ASSERT_EQ(one.directions().size(), 1u);
  expectVector(one.directions().front(), {0, 1, 0});
}

TEST(RaycastLidar, ComposesBaseAndMountForAllWorldCastEndpoints) {
  const LidarPose base{{10, 20, 30}, glm::angleAxis(pi / 2, glm::dvec3(0, 0, 1))};
  const LidarPose mount{{2, 3, 4}, glm::angleAxis(pi / 2, glm::dvec3(0, 1, 0))};
  const RaycastLidar lidar({.azimuthSamples = 2, .azimuthStart = 0, .azimuthSpan = pi / 2,
                           .maxRangeMeters = 12, .baseFromSensor = mount});
  const LidarSchedule schedule(60, 10'000, 42);
  const std::array<glm::dvec3, 2> expectedDisplacements{glm::dvec3(0, 0, -12), glm::dvec3(-12, 0, 0)};
  size_t beam = 0;
  const LidarCast cast = [&](glm::dvec3 origin, glm::dvec3 displacement) -> std::optional<LidarHit> {
    expectVector(origin, {7, 22, 34});
    if (beam >= expectedDisplacements.size()) {
      ADD_FAILURE() << "Unexpected extra ray";
      return std::nullopt;
    }
    expectVector(displacement, expectedDisplacements[beam]);
    return ++beam == 1 ? LidarHit{.25, "floor"} : LidarHit{.5, "wall"};
  };
  EXPECT_THROW((void)lidar.scan(base, schedule, 43, cast), std::invalid_argument);
  EXPECT_EQ(beam, 0u);
  const auto scan = lidar.scan(base, schedule, 42, cast);
  EXPECT_EQ(beam, expectedDisplacements.size());
  EXPECT_EQ(scan.tick, 42u);
  EXPECT_DOUBLE_EQ(scan.seconds, .7);
  expectVector(scan.worldFromSensor.position, {7, 22, 34});
  expectVector(scan.worldFromSensor.rotation * glm::dvec3(1, 0, 0), {0, 0, -1});
  ASSERT_EQ(scan.returns.size(), 2u);
  EXPECT_EQ(scan.returns[0], (LidarReturn{3.0, LidarReturnStatus::Valid, "floor"}));
  EXPECT_EQ(scan.returns[1], (LidarReturn{6.0, LidarReturnStatus::Valid, "wall"}));
}

TEST(RaycastLidar, DistinguishesMissNearOccluderRangeEndpointsAndDropout) {
  const RaycastLidar lidar({.azimuthSamples = 4, .minRangeMeters = 2, .maxRangeMeters = 10});
  const LidarSchedule schedule(60, 60'000);
  const std::array<std::optional<LidarHit>, 4> measurements{
      std::nullopt, LidarHit{.05, "near"}, LidarHit{.2, "minimum"}, LidarHit{1, "maximum"}};
  size_t beam = 0;
  const auto scan = lidar.scan({}, schedule, 0, [&](glm::dvec3, glm::dvec3) { return measurements.at(beam++); });
  ASSERT_EQ(scan.returns.size(), 4u);
  EXPECT_EQ(scan.returns[0], LidarReturn{});
  EXPECT_EQ(scan.returns[1], (LidarReturn{std::nullopt, LidarReturnStatus::BelowMinimum, "near"}));
  EXPECT_EQ(scan.returns[2], (LidarReturn{2.0, LidarReturnStatus::Valid, "minimum"}));
  EXPECT_EQ(scan.returns[3], (LidarReturn{10.0, LidarReturnStatus::Valid, "maximum"}));

  const RaycastLidar dropped({.azimuthSamples = 4, .minRangeMeters = 2, .maxRangeMeters = 10,
                              .dropoutProbability = 1});
  beam = 0;
  const auto droppedScan = dropped.scan({}, schedule, 0,
      [&](glm::dvec3, glm::dvec3) { return measurements.at(beam++); });
  ASSERT_EQ(droppedScan.returns.size(), measurements.size());
  EXPECT_EQ(droppedScan.returns[0], LidarReturn{});
  EXPECT_EQ(droppedScan.returns[1], scan.returns[1]);
  for (size_t i = 2; i < droppedScan.returns.size(); ++i) {
    EXPECT_EQ(droppedScan.returns[i].status, LidarReturnStatus::Dropped);
    EXPECT_FALSE(droppedScan.returns[i].rangeMeters.has_value());
  }
}

TEST(RaycastLidar, NoiseAndDropoutReplayByTickAndSeedWithoutRangeClamping) {
  RaycastLidarSettings settings{.azimuthSamples = 64, .maxRangeMeters = 10, .noiseStdDevMeters = .03,
                                .dropoutProbability = .25, .seed = 719};
  const RaycastLidar lidar(settings);
  const LidarSchedule schedule(60, 60'000);
  const LidarCast plane = [](glm::dvec3, glm::dvec3) { return LidarHit{.5, "plane"}; };
  const auto original = lidar.scan({}, schedule, 42, plane).returns;
  EXPECT_NE(original, lidar.scan({}, schedule, 43, plane).returns);
  EXPECT_EQ(original, lidar.scan({}, schedule, 42, plane).returns);
  EXPECT_EQ(original, RaycastLidar(settings).scan({}, schedule, 42, plane).returns);
  ++settings.seed;
  EXPECT_NE(original, RaycastLidar(settings).scan({}, schedule, 42, plane).returns);
  EXPECT_TRUE(std::ranges::any_of(original, [](const LidarReturn& value) {
    return value.status == LidarReturnStatus::Dropped && !value.rangeMeters;
  }));
  EXPECT_TRUE(std::ranges::any_of(original, [](const LidarReturn& value) {
    return value.status == LidarReturnStatus::Valid && value.rangeMeters &&
           std::abs(*value.rangeMeters - 5.0) > 1e-6 && value.entityId == "plane";
  }));

  settings.minRangeMeters = 9.99999;
  settings.dropoutProbability = 0;
  const auto narrow = RaycastLidar(settings).scan({}, schedule, 42,
      [](glm::dvec3, glm::dvec3) { return LidarHit{.9999995, "plane"}; });
  EXPECT_TRUE(std::ranges::any_of(narrow.returns, [](const LidarReturn& value) {
    return value.status == LidarReturnStatus::NoiseOutOfRange && !value.rangeMeters;
  }));
}

TEST(RaycastLidar, RejectsMalformedBackendFractionsAndInvalidSamplingSettings) {
  const RaycastLidar lidar({.azimuthSamples = 1});
  const LidarSchedule schedule(60, 60'000);
  for (double fraction : {-0.01, 1.01, std::numeric_limits<double>::infinity(),
                          std::numeric_limits<double>::quiet_NaN()}) {
    SCOPED_TRACE(fraction);
    EXPECT_THROW((void)lidar.scan({}, schedule, 0, [fraction](glm::dvec3, glm::dvec3) {
      return LidarHit{fraction, "invalid"};
    }), std::invalid_argument);
  }
  EXPECT_THROW((void)lidar.scan({}, schedule, 0, {}), std::invalid_argument);
  EXPECT_THROW(RaycastLidar({.azimuthSamples = 0}), std::invalid_argument);
  EXPECT_THROW(RaycastLidar({.elevationAngles = {}}), std::invalid_argument);
  EXPECT_THROW(RaycastLidar({.azimuthSpan = 3 * pi}), std::invalid_argument);
  EXPECT_THROW(RaycastLidar({.minRangeMeters = 10, .maxRangeMeters = 9}), std::invalid_argument);
  EXPECT_THROW(RaycastLidar({.dropoutProbability = 1.01}), std::invalid_argument);
}

} // namespace
