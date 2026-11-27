#include "Container/robotics/DifferentialDriveActuation.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {

using namespace container::robotics;

void expectRates(DriveWheels actual, DriveWheels expected, double tolerance = 1e-12) {
  EXPECT_NEAR(actual.left, expected.left, tolerance);
  EXPECT_NEAR(actual.right, expected.right, tolerance);
}

void expectCovariance(const PlanarOdometryCovariance& actual, const PlanarOdometryCovariance& expected,
                      double tolerance = 1e-12) {
  for (size_t entry = 0; entry < actual.size(); ++entry) {
    SCOPED_TRACE(entry);
    EXPECT_NEAR(actual[entry], expected[entry], tolerance);
  }
}

TEST(DriveActuation, DirectWheelLimitRetainsRatioAndDefaultsPassThrough) {
  const DifferentialDriveSettings drive{.maxWheelRadiansPerSecond = 8};
  expectRates(limitDifferentialWheelRates(drive, {4, -2}), {4, -2});
  expectRates(limitDifferentialWheelRates(drive, {-16, 4}), {-8, 2});
  expectRates(limitDifferentialWheelRates(drive, {}), {});
  constexpr double largest = std::numeric_limits<double>::max();
  expectRates(limitDifferentialWheelRates(drive, {largest, -.5 * largest}), {8, -4});

  const auto passthrough = applyDriveActuators(drive, {}, {{-2, 6}, {-2, 6}}, {2, -5}, .25);
  expectRates(passthrough.shaft, {2, -5});
  expectRates(passthrough.ground, {2, -5});
}

TEST(DriveActuation, WheelSaturationKeepsRepresentableRatesAcrossExtremeRatios) {
  // The small/large ratio underflows even though scaling the smaller wheel is
  // representable. Dividing the two requested rates first would lose the sign.
  const auto skewed = limitDifferentialWheelRates({.maxWheelRadiansPerSecond = 1e299}, {-1e-300, 1e300});
  EXPECT_NEAR(skewed.left / 1e-301, -1, 1e-14);
  EXPECT_DOUBLE_EQ(skewed.right, 1e299);

  // A precomputed common multiplier underflows for this pair, but both
  // scaled wheel rates fit in a double.
  const auto reduced = limitDifferentialWheelRates({.maxWheelRadiansPerSecond = 1e-299}, {1e298, -1e299});
  EXPECT_NEAR(reduced.left / 1e-300, 1, 1e-14);
  EXPECT_DOUBLE_EQ(reduced.right, -1e-299);
}

TEST(DriveActuation, ShaftSlewPrecedesSlipAndLongitudinalTractionSlew) {
  const DifferentialDriveSettings drive;
  const DifferentialDriveActuatorSettings settings{.maxWheelAccelerationRadiansPerSecondSquared = 10,
                                                   .tractionCoefficient = .05, .slipRatio = {.25, .5}};
  // One quarter-second permits 2.5 rad/s of shaft change and 1.22625 rad/s
  // of ground change. The right wheel reaches its smaller target immediately.
  const auto first = applyDriveActuators(drive, settings, {}, {6, -2}, .25);
  expectRates(first.shaft, {2.5, -2});
  expectRates(first.ground, {1.22625, -1});
  const auto second = applyDriveActuators(drive, settings, first, {6, -2}, .25);
  expectRates(second.shaft, {5, -2});
  expectRates(second.ground, {2.4525, -1});
  const auto reversed = applyDriveActuators(drive, settings, second, {-6, 6}, .25);
  expectRates(reversed.shaft, {2.5, .5});
  expectRates(reversed.ground, {1.875, .22625});

  const auto noGrip = applyDriveActuators(drive, {.tractionCoefficient = 0}, {}, {6, -4}, .25);
  expectRates(noGrip.shaft, {6, -4});
  expectRates(noGrip.ground, {});
  const auto completeSlip = applyDriveActuators(drive, {.slipRatio = {1, .25}}, {}, {6, -4}, .25);
  expectRates(completeSlip.shaft, {6, -4});
  expectRates(completeSlip.ground, {0, -3});
}

TEST(DriveActuation, StraightCovarianceContainsSignedCrossTermsAndHasAContinuousLimit) {
  const PlanarOdometryCovariance expected{
      .0125, -.006, -.03,
      -.006, .008, .04,
      -.03, .04, .2};
  expectCovariance(propagatePlanarOdometryCovariance({}, {}, .5, {.4, .4}, {.04, .01}), expected);
  expectCovariance(propagatePlanarOdometryCovariance({}, {}, .5, {.4, .4 + 1e-12}, {.04, .01}),
                   expected, 1e-10);

  const PlanarOdometryCovariance prior{0, 0, 0, 0, 0, 0, 0, 0, .04};
  const PlanarOdometryCovariance propagated{
      .01, 0, -.02,
      0, 0, 0,
      -.02, 0, .04};
  expectCovariance(propagatePlanarOdometryCovariance(prior, {.yaw = std::numbers::pi / 2},
                                                      .5, {.5, .5}, {}), propagated);
  expectCovariance(propagatePlanarOdometryCovariance(prior, {}, .5, {}, {}), prior);
}

// Evaluate an arc independently of the drive helper for a nonzero turn.
std::array<double, 3> arcPose(DrivePose prior, double trackWidth, DriveWheels travel) {
  const double turn = (travel.right - travel.left) / trackWidth;
  const double radius = (travel.left + travel.right) / (2 * turn);
  const double heading = prior.yaw + turn;
  return {prior.position.x + radius * (std::sin(heading) - std::sin(prior.yaw)),
          prior.position.y + radius * (std::cos(prior.yaw) - std::cos(heading)), heading};
}

TEST(DriveActuation, ArcCovarianceMatchesNumericalWheelDistanceJacobians) {
  const DrivePose prior{{2, 3, 0}, .7};
  constexpr double track = .4;
  const DriveWheels travel{.25, .55}, variance{.014, .026};
  constexpr double delta = 1e-5;
  const auto leftPlus = arcPose(prior, track, {travel.left + delta, travel.right});
  const auto leftMinus = arcPose(prior, track, {travel.left - delta, travel.right});
  const auto rightPlus = arcPose(prior, track, {travel.left, travel.right + delta});
  const auto rightMinus = arcPose(prior, track, {travel.left, travel.right - delta});
  std::array<double, 3> left{}, right{};
  for (size_t axis = 0; axis < left.size(); ++axis) {
    left[axis] = (leftPlus[axis] - leftMinus[axis]) / (2 * delta);
    right[axis] = (rightPlus[axis] - rightMinus[axis]) / (2 * delta);
  }
  PlanarOdometryCovariance expected{};
  for (size_t row = 0; row < left.size(); ++row)
    for (size_t col = 0; col < left.size(); ++col)
      expected[3 * row + col] = left[row] * left[col] * variance.left + right[row] * right[col] * variance.right;
  const auto actual = propagatePlanarOdometryCovariance({}, prior, track, travel, variance);
  expectCovariance(actual, expected, 1e-8);
  EXPECT_GT(actual[0], 0);
  EXPECT_GT(actual[4], 0);
  EXPECT_GT(actual[8], 0);
}

TEST(DriveActuation, RejectsNonfiniteOrNegativePhysicalInputs) {
  const DifferentialDriveSettings drive;
  constexpr auto nan = std::numeric_limits<double>::quiet_NaN();
  constexpr auto infinity = std::numeric_limits<double>::infinity();
  EXPECT_NO_THROW(validateDriveActuators({}, {}));
  EXPECT_THROW(validateDriveActuators({.maxWheelAccelerationRadiansPerSecondSquared = -1}, {}),
               std::invalid_argument);
  EXPECT_THROW(validateDriveActuators({.tractionCoefficient = -.5}, {}), std::invalid_argument);
  EXPECT_THROW(validateDriveActuators({.slipRatio = {nan, 0}}, {}), std::invalid_argument);
  EXPECT_THROW(validateDriveActuators({.slipRatio = {1.01, 0}}, {}), std::invalid_argument);
  EXPECT_THROW(validateDriveActuators({}, {0, -.1}), std::invalid_argument);
  EXPECT_THROW((void)limitDifferentialWheelRates(drive, {1, infinity}), std::invalid_argument);
  EXPECT_THROW((void)applyDriveActuators(drive, {}, {}, {1, 2}, 0), std::invalid_argument);
  EXPECT_THROW((void)applyDriveActuators(drive, {}, {{0, infinity}, {}}, {}, .01), std::invalid_argument);
  EXPECT_THROW((void)propagatePlanarOdometryCovariance({}, {}, .5, {}, {0, -.01}), std::invalid_argument);
}

} // namespace
