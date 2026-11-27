#include "Container/robotics/DifferentialDrive.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>
#include <stdexcept>
#include <string>

namespace container::robotics {
namespace {

constexpr double pi = std::numbers::pi;
constexpr double turn = 2 * pi;

void requireFinite(double value, const char* name) {
  if (!std::isfinite(value))
    throw std::invalid_argument(std::string("Differential drive ") + name + " must be finite");
}

void requirePositive(double value, const char* name) {
  requireFinite(value, name);
  if (value <= 0)
    throw std::invalid_argument(std::string("Differential drive ") + name + " must be positive");
}

// A normalized binary value can represent intermediate products/quotients
// beyond the double exponent range without overflowing before saturation.
struct Scaled {
  double mantissa{};
  int exponent{};
};

Scaled normalize(double mantissa, int exponent = 0) {
  if (mantissa == 0)
    return {};
  int adjustment;
  const double value = std::frexp(mantissa, &adjustment);
  return {value, exponent + adjustment};
}

Scaled product(double first, double second) {
  const auto a = normalize(first), b = normalize(second);
  return normalize(a.mantissa * b.mantissa, a.exponent + b.exponent);
}

Scaled quotient(Scaled numerator, double denominator) {
  const auto divisor = normalize(denominator);
  return normalize(numerator.mantissa / divisor.mantissa,
                   numerator.exponent - divisor.exponent);
}

Scaled sum(double first, double second) {
  const auto a = normalize(first), b = normalize(second);
  if (a.mantissa == 0)
    return b;
  if (b.mantissa == 0)
    return a;
  const int exponent = std::max(a.exponent, b.exponent);
  return normalize(std::scalbn(a.mantissa, a.exponent - exponent) +
                       std::scalbn(b.mantissa, b.exponent - exponent), exponent);
}

double materialize(Scaled value, const char* name) {
  const double result = std::scalbn(value.mantissa, value.exponent);
  requireFinite(result, name);
  return result;
}

double wrapYaw(double angle) {
  double result = std::remainder(angle, turn);
  if (result >= pi)
    result -= turn;
  return result == 0 ? 0.0 : result; // Canonical [-pi, pi), including +0.
}

double chordTravel(double metres, double halfTurn) {
  if (std::abs(halfTurn) < 1e-4) {
    const double square = halfTurn * halfTurn;
    const double sinc = 1 + square * (-1.0 / 6 + square * (1.0 / 120 - square / 5040));
    return metres * sinc;
  }
  const double sine = std::sin(std::remainder(halfTurn, turn));
  // Form s*sin(a)/a in exponent space: sin(a)/a alone can underflow even
  // when multiplication by a very large finite travel would be representable.
  return materialize(quotient(product(metres, sine), halfTurn), "arc displacement");
}

} // namespace

void validateDifferentialDrive(const DifferentialDriveSettings& settings) {
  requirePositive(settings.wheelRadius, "wheel radius");
  requirePositive(settings.trackWidth, "track width");
  requirePositive(settings.maxWheelRadiansPerSecond, "maximum wheel rate");
}

DriveWheels differentialWheelRates(const DifferentialDriveSettings& settings, DriveTwist command) {
  validateDifferentialDrive(settings);
  requireFinite(command.linear, "linear command");
  requireFinite(command.angular, "angular command");
  const auto linear = normalize(command.linear);
  auto angular = product(command.angular, settings.trackWidth);
  if (angular.mantissa != 0)
    --angular.exponent; // Half the track width, without underflowing it.
  if (linear.mantissa == 0 && angular.mantissa == 0)
    return {};
  const int exponent = linear.mantissa == 0 ? angular.exponent :
      angular.mantissa == 0 ? linear.exponent : std::max(linear.exponent, angular.exponent);
  const double v = linear.mantissa == 0 ? 0 :
      std::scalbn(linear.mantissa, linear.exponent - exponent);
  const double a = angular.mantissa == 0 ? 0 :
      std::scalbn(angular.mantissa, angular.exponent - exponent);
  const double left = v - a, right = v + a;
  const double maximum = std::max(std::abs(left), std::abs(right));
  const auto requested = quotient(normalize(maximum, exponent), settings.wheelRadius);
  const auto limit = normalize(settings.maxWheelRadiansPerSecond);
  const bool saturated = requested.exponent > limit.exponent ||
      (requested.exponent == limit.exponent && requested.mantissa > limit.mantissa);
  const double magnitude = saturated ? settings.maxWheelRadiansPerSecond :
      materialize(requested, "wheel rate");
  // One common factor preserves curvature, including reverse commands.
  return {(left / maximum) * magnitude, (right / maximum) * magnitude};
}

double differentialYawRate(const DifferentialDriveSettings& settings, DriveWheels rates) {
  validateDifferentialDrive(settings);
  requireFinite(rates.left, "left wheel rate");
  requireFinite(rates.right, "right wheel rate");
  const auto difference = sum(rates.right, -rates.left);
  const auto radius = normalize(settings.wheelRadius);
  const auto linear = normalize(difference.mantissa * radius.mantissa,
                                difference.exponent + radius.exponent);
  return materialize(quotient(linear, settings.trackWidth), "yaw rate");
}

DrivePose integrateWheelTravel(DrivePose initial, double trackWidth, DriveWheels metres) {
  requirePositive(trackWidth, "track width");
  requireFinite(initial.position.x, "pose X");
  requireFinite(initial.position.y, "pose Y");
  requireFinite(initial.position.z, "pose Z");
  requireFinite(initial.yaw, "pose yaw");
  requireFinite(metres.left, "left wheel travel");
  requireFinite(metres.right, "right wheel travel");
  const double distance = std::midpoint(metres.left, metres.right);
  const double yawChange = materialize(quotient(sum(metres.right, -metres.left), trackWidth),
                                        "yaw displacement");
  const double halfTurn = yawChange * .5;
  const double heading = wrapYaw(wrapYaw(initial.yaw) + std::remainder(halfTurn, turn));
  const double chord = chordTravel(distance, halfTurn);
  const double x = initial.position.x + chord * std::cos(heading);
  const double y = initial.position.y + chord * std::sin(heading);
  requireFinite(x, "integrated pose X");
  requireFinite(y, "integrated pose Y");
  initial.position.x = x;
  initial.position.y = y;
  initial.yaw = wrapYaw(wrapYaw(initial.yaw) + wrapYaw(yawChange));
  return initial;
}

} // namespace container::robotics
