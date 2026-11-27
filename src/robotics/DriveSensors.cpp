#include "Container/robotics/DriveSensors.h"
#include "SensorNoise.h"

#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <utility>

namespace container::robotics {
namespace {

constexpr double revolution = 2 * std::numbers::pi;
constexpr int64_t maxExactCount = 9'007'199'254'740'991;
constexpr uint64_t driveNoiseDomain = 0xbb67ae8584caa73bULL;

void requireFinite(double value, const char* name) {
  if (!std::isfinite(value))
    throw std::invalid_argument(std::string("Drive sensor ") + name + " must be finite");
}

void requireFinite(glm::dvec3 value, const char* name) {
  requireFinite(value.x, name);
  requireFinite(value.y, name);
  requireFinite(value.z, name);
}

void validateError(double bias, double standardDeviation, double drift) {
  requireFinite(bias, "bias");
  requireFinite(standardDeviation, "noise standard deviation");
  requireFinite(drift, "drift per second");
  if (standardDeviation < 0)
    throw std::invalid_argument("Drive sensor noise standard deviation cannot be negative");
}

void validateError(const ImuAxisError& error) {
  for (glm::length_t axis = 0; axis < 3; ++axis)
    validateError(error.bias[axis], error.noiseStdDev[axis], error.driftPerSecond[axis]);
}

double measured(double truth, double bias, double standardDeviation, double drift,
                double seconds, uint64_t tickKey, uint64_t channel) {
  const auto noise = standardDeviation == 0 ? 0 :
      standardDeviation * sensor_noise::normal(sensor_noise::mix(tickKey ^ channel));
  const auto value = truth + bias + drift * seconds + noise;
  requireFinite(value, "measurement");
  return value;
}

glm::dvec3 measured(glm::dvec3 truth, const ImuAxisError& error, double seconds,
                    uint64_t tickKey, uint64_t firstChannel) {
  glm::dvec3 result;
  for (glm::length_t axis = 0; axis < 3; ++axis)
    result[axis] = measured(truth[axis], error.bias[axis], error.noiseStdDev[axis],
                            error.driftPerSecond[axis], seconds, tickKey, firstChannel + uint64_t(axis));
  return result;
}

int64_t quantize(double radians, uint32_t countsPerRevolution) {
  const auto count = std::round(radians * (double(countsPerRevolution) / revolution));
  // Round signed nearest counts, with half ties away from zero. Check before
  // converting to an integer so overflow and inexact JSON integers never escape.
  if (!std::isfinite(count) || count < -double(maxExactCount) || count > double(maxExactCount))
    throw std::invalid_argument("Drive sensor encoder count exceeds the exact JSON integer range");
  return static_cast<int64_t>(count);
}

double radiansFromCounts(int64_t count, uint32_t countsPerRevolution) {
  return double(count) * (revolution / double(countsPerRevolution));
}

} // namespace

DriveSensorModel::DriveSensorModel(SensorSchedule schedule, DifferentialDriveSettings drive,
                                   DriveSensorSettings settings)
    : schedule_(std::move(schedule)), drive_(drive), settings_(std::move(settings)) {
  validateDifferentialDrive(drive_);
  validateError(settings_.accelerometer);
  validateError(settings_.gyroscope);
  const auto& encoders = settings_.encoders;
  if (encoders.countsPerRevolution == 0)
    throw std::invalid_argument("Drive sensor encoder counts per revolution must be positive");
  validateError(encoders.biasRadians.left, encoders.noiseStdDevRadians.left, encoders.driftRadiansPerSecond.left);
  validateError(encoders.biasRadians.right, encoders.noiseStdDevRadians.right, encoders.driftRadiansPerSecond.right);
}

std::optional<DriveSensorSample> DriveSensorModel::observe(const DriveSensorTruth& truth) {
  if ((!previous_ && truth.tick != 0) ||
      (previous_ && (previous_->tick == std::numeric_limits<uint64_t>::max() ||
                     truth.tick != previous_->tick + 1)))
    throw std::invalid_argument("Drive sensor truth must start at tick zero and include every contiguous fixed tick");
  requireFinite(truth.base.position, "world position");
  requireFinite(truth.base.yaw, "yaw");
  requireFinite(truth.yawRateRadiansPerSecond, "yaw rate");
  requireFinite(truth.encoderRadians.left, "left encoder angle");
  requireFinite(truth.encoderRadians.right, "right encoder angle");

  const auto seconds = schedule_.seconds(truth.tick);
  const double frequency = schedule_.ticksPerSecond();
  glm::dvec3 velocity(0.0), acceleration(0.0);
  if (previous_) {
    velocity = (truth.base.position - previous_->position) * frequency;
    acceleration = (velocity - previous_->worldVelocity) * frequency;
    requireFinite(velocity, "world velocity");
    requireFinite(acceleration, "world acceleration");
  }
  const auto angle = std::remainder(truth.base.yaw, revolution);
  const auto cosine = std::cos(angle), sine = std::sin(angle);
  // Specific force is R_world_from_base^-1 * (a_world - gravity_world).
  // DrivePose has yaw only; a static level REP-103 base reads +9.81 on Z.
  const glm::dvec3 specificForce{cosine * acceleration.x + sine * acceleration.y,
                                -sine * acceleration.x + cosine * acceleration.y,
                                acceleration.z + 9.81};
  requireFinite(specificForce, "specific force");
  const auto tickKey = sensor_noise::mix(sensor_noise::mix(settings_.seed ^ driveNoiseDomain) ^ truth.tick);
  DriveSensorSample sample{.tick = truth.tick, .seconds = seconds};
  sample.specificForce = measured(specificForce, settings_.accelerometer, seconds, tickKey, 0);
  sample.angularVelocity = measured(glm::dvec3(0, 0, truth.yawRateRadiansPerSecond),
                                    settings_.gyroscope, seconds, tickKey, 3);

  const auto& error = settings_.encoders;
  const auto left = measured(truth.encoderRadians.left, error.biasRadians.left,
                            error.noiseStdDevRadians.left, error.driftRadiansPerSecond.left,
                            seconds, tickKey, 6);
  const auto right = measured(truth.encoderRadians.right, error.biasRadians.right,
                             error.noiseStdDevRadians.right, error.driftRadiansPerSecond.right,
                             seconds, tickKey, 7);
  sample.encoderCounts = {quantize(left, error.countsPerRevolution), quantize(right, error.countsPerRevolution)};
  sample.encoderRadians = {radiansFromCounts(sample.encoderCounts.left, error.countsPerRevolution),
                           radiansFromCounts(sample.encoderCounts.right, error.countsPerRevolution)};
  if (previous_) {
    // Both counts fit +/- (2^53-1); their signed difference fits int64_t.
    const DriveWheels travel{
        radiansFromCounts(sample.encoderCounts.left - previous_->counts.left, error.countsPerRevolution) * drive_.wheelRadius,
        radiansFromCounts(sample.encoderCounts.right - previous_->counts.right, error.countsPerRevolution) * drive_.wheelRadius};
    sample.odometry = integrateWheelTravel(previous_->odometry, drive_.trackWidth, travel);
  }

  // Even unpublished ticks are necessary for causal derivatives and encoder
  // integration. Publish only after the entire next history is valid.
  previous_ = History{truth.tick, truth.base.position, velocity, sample.encoderCounts, sample.odometry};
  if (schedule_.due(truth.tick))
    return sample;
  return std::nullopt;
}

} // namespace container::robotics
