#pragma once

#include "Container/robotics/DifferentialDrive.h"
#include "Container/robotics/SensorSchedule.h"

#include <cstdint>
#include <optional>

namespace container::robotics {

// All errors use the corresponding signal's units. Drift is the signed change
// in bias per simulated second. Noise is independent Gaussian white noise.
struct ImuAxisError {
  glm::dvec3 bias{0.0};
  glm::dvec3 noiseStdDev{0.0};
  glm::dvec3 driftPerSecond{0.0};
};

struct EncoderObservationSettings {
  uint32_t countsPerRevolution{2048};
  DriveWheels biasRadians;
  DriveWheels noiseStdDevRadians;
  DriveWheels driftRadiansPerSecond;
};

struct DriveSensorSettings {
  ImuAxisError accelerometer;
  ImuAxisError gyroscope;
  EncoderObservationSettings encoders;
  uint64_t seed{};
};

struct DriveSensorTruth {
  uint64_t tick{};
  DrivePose base;
  DriveWheels encoderRadians;
  // Accepted yaw rate in radians per second. An absolute heading can wrap
  // between ticks and cannot supply the unambiguous rotation by itself.
  double yawRateRadiansPerSecond{};
};

struct EncoderCounts {
  int64_t left{};
  int64_t right{};
  bool operator==(const EncoderCounts&) const = default;
};

struct DriveSensorSample {
  uint64_t tick{};
  double seconds{};
  glm::dvec3 specificForce{0.0}; // metres per second squared
  glm::dvec3 angularVelocity{0.0}; // radians per second
  EncoderCounts encoderCounts;
  DriveWheels encoderRadians;
  DrivePose odometry;
};

// A base-aligned REP-103 IMU and a pair of signed wheel encoders. Supply every
// committed tick in order, starting at zero at rest. A publication includes
// the synchronized IMU, encoder and encoder-integrated odometry readings.
class DriveSensorModel {
public:
  DriveSensorModel(SensorSchedule schedule, DifferentialDriveSettings drive,
                   DriveSensorSettings settings = {});
  [[nodiscard]] const SensorSchedule& schedule() const noexcept { return schedule_; }
  [[nodiscard]] const DriveSensorSettings& settings() const noexcept { return settings_; }
  [[nodiscard]] std::optional<DriveSensorSample> observe(const DriveSensorTruth& truth);
  void reset() noexcept { previous_.reset(); }

private:
  struct History {
    uint64_t tick;
    glm::dvec3 position;
    glm::dvec3 worldVelocity;
    EncoderCounts counts;
    DrivePose odometry;
  };

  SensorSchedule schedule_;
  DifferentialDriveSettings drive_;
  DriveSensorSettings settings_;
  std::optional<History> previous_;
};

} // namespace container::robotics
