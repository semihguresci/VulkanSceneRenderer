#include "Container/robotics/DifferentialDriveActuation.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <numbers>
#include <numeric>
#include <stdexcept>
#include <string>

namespace container::robotics {
namespace {

constexpr double turn = 2 * std::numbers::pi;
using Vector = std::array<double, 3>;
using Matrix = std::array<Vector, 3>;

void finite(double value, const char* name) {
  if (!std::isfinite(value))
    throw std::invalid_argument(std::string("Differential drive ") + name + " must be finite");
}

void finite(DriveWheels value, const char* name) {
  finite(value.left, name);
  finite(value.right, name);
}

void nonnegative(double value, const char* name) {
  finite(value, name);
  if (value < 0)
    throw std::invalid_argument(std::string("Differential drive ") + name + " cannot be negative");
}

void variances(DriveWheels value) {
  nonnegative(value.left, "left wheel variance");
  nonnegative(value.right, "right wheel variance");
}

// All factors are finite and denominators nonzero. Exponent arithmetic avoids
// overflowing mu*g before dividing by a large wheel radius, for example.
// An infinite result can safely mean an unlimited internal slew allowance.
double ratio(std::initializer_list<double> numerators, std::initializer_list<double> denominators = {}) {
  double mantissa = 1;
  int exponent = 0;
  for (const auto value : numerators) {
    if (value == 0) return 0;
    int shift;
    mantissa *= std::frexp(value, &shift);
    exponent += shift;
  }
  for (const auto value : denominators) {
    int shift;
    mantissa /= std::frexp(value, &shift);
    exponent -= shift;
  }
  return std::scalbn(mantissa, exponent);
}

double approach(double previous, double target, double allowance) {
  if (target > previous)
    return target - previous <= allowance ? target : std::min(target, previous + allowance);
  if (target < previous)
    return previous - target <= allowance ? target : std::max(target, previous - allowance);
  return target;
}

Matrix covarianceFactor(const PlanarOdometryCovariance& previous) {
  for (const auto value : previous)
    finite(value, "odometry covariance");
  Matrix factor{};
  Matrix normalized{};
  Vector deviations{};
  constexpr double tolerance = 1e-12;
  for (size_t row = 0; row < 3; ++row) {
    if (previous[row * 3 + row] < 0)
      throw std::invalid_argument("Odometry covariance diagonal cannot be negative");
    deviations[row] = std::sqrt(previous[row * 3 + row]);
  }
  // Factor the correlation matrix rather than scaling the whole covariance
  // by its largest entry. This retains tiny valid coordinate variances and
  // cannot hide an invalid small block beside a large unrelated variance.
  for (size_t row = 0; row < 3; ++row) {
    for (size_t column = 0; column < 3; ++column) {
      if (deviations[row] == 0 || deviations[column] == 0) {
        if (previous[row * 3 + column] != 0 || previous[column * 3 + row] != 0)
          throw std::invalid_argument("Odometry covariance must be positive semidefinite");
        continue;
      }
      const auto first = ratio({previous[row * 3 + column]}, {deviations[row], deviations[column]});
      const auto second = ratio({previous[column * 3 + row]}, {deviations[row], deviations[column]});
      finite(first, "covariance correlation");
      finite(second, "covariance correlation");
      if (std::abs(first) > 1 + tolerance || std::abs(second) > 1 + tolerance)
        throw std::invalid_argument("Odometry covariance must be positive semidefinite");
      if (std::abs(first - second) > tolerance)
        throw std::invalid_argument("Odometry covariance must be symmetric");
      normalized[row][column] = std::midpoint(first, second);
    }
  }
  // Semidefinite Cholesky. Tiny negative residuals from roundoff are projected
  // to zero; a zero pivot must have a correspondingly zero residual column.
  for (size_t row = 0; row < 3; ++row) {
    for (size_t column = 0; column <= row; ++column) {
      auto residual = normalized[row][column];
      for (size_t k = 0; k < column; ++k)
        residual = std::fma(-factor[row][k], factor[column][k], residual);
      finite(residual, "covariance factor");
      if (row == column) {
        if (residual < -tolerance)
          throw std::invalid_argument("Odometry covariance must be positive semidefinite");
        factor[row][column] = std::sqrt(std::max(0.0, residual));
      } else if (factor[column][column] == 0) {
        if (std::abs(residual) > tolerance)
          throw std::invalid_argument("Odometry covariance must be positive semidefinite");
      } else {
        factor[row][column] = residual / factor[column][column];
        finite(factor[row][column], "covariance factor");
      }
    }
  }
  for (size_t row = 0; row < 3; ++row)
    for (auto& value : factor[row]) {
      value *= deviations[row];
      finite(value, "covariance factor");
    }
  return factor;
}

Vector wheelNoiseColumn(double variance, double sign, double distance, double halfTurn,
                        double trackWidth, double cosine, double sine) {
  if (variance == 0) return {};
  const auto deviation = std::sqrt(variance);
  double common, radial, tangent;
  if (std::abs(halfTurn) < 1e-3) {
    const auto square = halfTurn * halfTurn;
    const auto sinc = 1 + square * (-1.0 / 6 + square * (1.0 / 120 - square / 5040));
    const auto derivative = halfTurn * (-1.0 / 3 + square * (1.0 / 30 +
                                      square * (-1.0 / 840 + square / 45360)));
    common = ratio({deviation, .5, sinc});
    radial = ratio({distance, deviation, .5, derivative}, {trackWidth});
    tangent = ratio({distance, deviation, .5, sinc}, {trackWidth});
  } else {
    const auto angle = std::remainder(halfTurn, turn);
    const auto sinTurn = std::sin(angle), cosTurn = std::cos(angle);
    common = ratio({deviation, .5, sinTurn}, {halfTurn});
    radial = ratio({distance, deviation, .5, cosTurn - sinTurn / halfTurn}, {trackWidth, halfTurn});
    tangent = ratio({distance, deviation, .5, sinTurn}, {trackWidth, halfTurn});
  }
  // d(s*sinc(a)*[cos(theta+a),sin(theta+a)])/d(left,right),
  // with ds/dwheel=1/2 and da/dwheel=+/-1/(2*trackWidth).
  Vector column{
      common * cosine + sign * std::fma(radial, cosine, -tangent * sine),
      common * sine + sign * std::fma(radial, sine, tangent * cosine),
      ratio({sign, deviation}, {trackWidth})};
  for (const auto value : column) finite(value, "wheel covariance Jacobian");
  return column;
}

} // namespace

void validateDriveActuators(const DifferentialDriveActuatorSettings& settings,
                            DriveWheels wheelTravelVariancePerMeter) {
  nonnegative(settings.maxWheelAccelerationRadiansPerSecondSquared, "maximum wheel acceleration");
  if (settings.tractionCoefficient)
    nonnegative(*settings.tractionCoefficient, "traction coefficient");
  finite(settings.slipRatio, "wheel slip ratio");
  if (settings.slipRatio.left < 0 || settings.slipRatio.left > 1 ||
      settings.slipRatio.right < 0 || settings.slipRatio.right > 1)
    throw std::invalid_argument("Differential drive wheel slip ratios must be in [0,1]");
  variances(wheelTravelVariancePerMeter);
}

DriveWheels limitDifferentialWheelRates(const DifferentialDriveSettings& drive, DriveWheels request) {
  validateDifferentialDrive(drive);
  finite(request, "requested wheel rates");
  const auto magnitude = std::max(std::abs(request.left), std::abs(request.right));
  if (magnitude <= drive.maxWheelRadiansPerSecond) return request;
  const auto scaled = [&](double value) {
    return value == 0 ? value : ratio({value, drive.maxWheelRadiansPerSecond}, {magnitude});
  };
  return {scaled(request.left), scaled(request.right)};
}

DifferentialDriveRates applyDriveActuators(const DifferentialDriveSettings& drive,
    const DifferentialDriveActuatorSettings& settings, DifferentialDriveRates previous,
    DriveWheels target, double seconds) {
  target = limitDifferentialWheelRates(drive, target);
  validateDriveActuators(settings, {});
  finite(previous.shaft, "previous shaft rates");
  finite(previous.ground, "previous ground rates");
  finite(seconds, "actuator duration");
  if (seconds <= 0)
    throw std::invalid_argument("Differential drive actuator duration must be positive");
  DifferentialDriveRates result{.shaft = target};
  if (settings.maxWheelAccelerationRadiansPerSecondSquared > 0) {
    const auto allowance = ratio({settings.maxWheelAccelerationRadiansPerSecondSquared, seconds});
    result.shaft = {approach(previous.shaft.left, target.left, allowance),
                    approach(previous.shaft.right, target.right, allowance)};
  }
  result.ground = {result.shaft.left * (1 - settings.slipRatio.left),
                   result.shaft.right * (1 - settings.slipRatio.right)};
  if (settings.tractionCoefficient) {
    const auto allowance = ratio({*settings.tractionCoefficient, 9.81, seconds}, {drive.wheelRadius});
    result.ground = {approach(previous.ground.left, result.ground.left, allowance),
                     approach(previous.ground.right, result.ground.right, allowance)};
  }
  finite(result.shaft, "shaft rates");
  finite(result.ground, "ground rates");
  return result;
}

PlanarOdometryCovariance propagatePlanarOdometryCovariance(
    const PlanarOdometryCovariance& previous, DrivePose prior, double trackWidth,
    DriveWheels measuredMetres, DriveWheels wheelVarianceMetresSquared) {
  finite(prior.position.x, "prior pose X");
  finite(prior.position.y, "prior pose Y");
  finite(prior.position.z, "prior pose Z");
  finite(prior.yaw, "prior pose yaw");
  finite(measuredMetres, "wheel travel");
  variances(wheelVarianceMetresSquared);
  // Also validates the track and displacement range using the same robust
  // continuous SE(2) integration as the odometry state.
  const auto displacement = integrateWheelTravel({.yaw = prior.yaw}, trackWidth, measuredMetres).position;
  const auto factor = covarianceFactor(previous);
  if (measuredMetres.left == 0 && measuredMetres.right == 0 &&
      wheelVarianceMetresSquared.left == 0 && wheelVarianceMetresSquared.right == 0) {
    auto result = previous;
    for (size_t row = 0; row < 3; ++row)
      for (size_t column = 0; column < row; ++column)
        result[row * 3 + column] = result[column * 3 + row] =
            std::midpoint(previous[row * 3 + column], previous[column * 3 + row]);
    return result;
  }
  std::array<Vector, 5> columns{};
  for (size_t column = 0; column < 3; ++column) {
    columns[column] = {std::fma(-displacement.y, factor[2][column], factor[0][column]),
                        std::fma(displacement.x, factor[2][column], factor[1][column]),
                        factor[2][column]};
    for (const auto value : columns[column]) finite(value, "propagated covariance factor");
  }
  if (wheelVarianceMetresSquared.left > 0 || wheelVarianceMetresSquared.right > 0) {
    const DifferentialDriveSettings metric{.wheelRadius = 1, .trackWidth = trackWidth,
                                           .maxWheelRadiansPerSecond = 1};
    const auto halfTurn = differentialYawRate(metric, measuredMetres) * .5;
    const auto angle = std::remainder(std::remainder(prior.yaw, turn) +
                                       std::remainder(halfTurn, turn), turn);
    const auto distance = std::midpoint(measuredMetres.left, measuredMetres.right);
    columns[3] = wheelNoiseColumn(wheelVarianceMetresSquared.left, -1, distance, halfTurn,
                                  trackWidth, std::cos(angle), std::sin(angle));
    columns[4] = wheelNoiseColumn(wheelVarianceMetresSquared.right, 1, distance, halfTurn,
                                  trackWidth, std::cos(angle), std::sin(angle));
  }
  // F*P*F^T + G*diag(wheelVariance)*G^T, formed as a Gram matrix to retain
  // symmetry and positive semidefiniteness even for zero or rank-one inputs.
  PlanarOdometryCovariance result{};
  for (size_t row = 0; row < 3; ++row)
    for (size_t column = 0; column <= row; ++column) {
      double value = 0;
      for (const auto& vector : columns)
        value = std::fma(vector[row], vector[column], value);
      finite(value, "propagated odometry covariance");
      result[row * 3 + column] = result[column * 3 + row] = value;
    }
  return result;
}

} // namespace container::robotics
