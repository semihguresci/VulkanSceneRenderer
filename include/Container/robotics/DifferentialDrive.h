#pragma once

#include <glm/vec3.hpp>

namespace container::robotics {

struct DifferentialDriveSettings {
  double wheelRadius{0.1};
  double trackWidth{0.4};
  double maxWheelRadiansPerSecond{20.0};
};

// Horizontal REP-103 twist expressed in the robot's local base frame.
struct DriveTwist {
  double linear{};
  double angular{};
};

struct DriveWheels {
  double left{};
  double right{};
};

struct DrivePose {
  glm::dvec3 position{0.0};
  double yaw{};
};

void validateDifferentialDrive(const DifferentialDriveSettings& settings);
[[nodiscard]] DriveWheels differentialWheelRates(
    const DifferentialDriveSettings& settings, DriveTwist command);
// Angular velocity about the base +Z axis from accepted wheel rates in rad/s.
[[nodiscard]] double differentialYawRate(
    const DifferentialDriveSettings& settings, DriveWheels rates);
[[nodiscard]] DrivePose integrateWheelTravel(
    DrivePose initial, double trackWidth, DriveWheels metres);

} // namespace container::robotics
