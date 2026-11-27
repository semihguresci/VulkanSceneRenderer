#pragma once

#include "Container/robotics/DifferentialDrive.h"

#include <array>
#include <optional>

namespace container::robotics {

struct DifferentialDriveActuatorSettings {
  // Zero leaves the shaft acceleration unlimited.
  double maxWheelAccelerationRadiansPerSecondSquared{};
  // Optional longitudinal acceleration bound of coefficient times gravity.
  std::optional<double> tractionCoefficient;
  // Fraction of shaft travel lost to slip in each ground track.
  DriveWheels slipRatio;
};

struct DifferentialDriveRates {
  DriveWheels shaft;
  DriveWheels ground;
};

// Row-major covariance in the planar odometry frame, ordered x, y, yaw.
using PlanarOdometryCovariance = std::array<double, 9>;

void validateDriveActuators(const DifferentialDriveActuatorSettings& settings,
                            DriveWheels wheelTravelVariancePerMeter);
[[nodiscard]] DriveWheels limitDifferentialWheelRates(const DifferentialDriveSettings& drive,
                                                      DriveWheels request);
[[nodiscard]] DifferentialDriveRates applyDriveActuators(const DifferentialDriveSettings& drive,
    const DifferentialDriveActuatorSettings& settings, DifferentialDriveRates previous,
    DriveWheels target, double seconds);

// Linearize the same planar arc as integrateWheelTravel. Process variances
// describe independent left and right distance measurements in metres squared.
[[nodiscard]] PlanarOdometryCovariance propagatePlanarOdometryCovariance(
    const PlanarOdometryCovariance& previous, DrivePose prior, double trackWidth,
    DriveWheels measuredMetres, DriveWheels wheelVarianceMetresSquared);

} // namespace container::robotics
