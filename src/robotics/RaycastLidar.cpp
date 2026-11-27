#include "Container/robotics/RaycastLidar.h"
#include "SensorNoise.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace container::robotics {
namespace {

constexpr double pi = std::numbers::pi;
constexpr double revolution = 2 * pi;
constexpr size_t maxBeams = 65'536;

bool finite(glm::dvec3 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

LidarPose normalizedPose(const LidarPose& pose) {
  const auto& rotation = pose.rotation;
  if (!finite(pose.position) || !std::isfinite(rotation.w) ||
      !finite(glm::dvec3(rotation.x, rotation.y, rotation.z)))
    throw std::invalid_argument("LiDAR position and quaternion must be finite");
  const auto scale = std::max({std::abs(rotation.w), std::abs(rotation.x),
                               std::abs(rotation.y), std::abs(rotation.z)});
  if (scale == 0)
    throw std::invalid_argument("LiDAR quaternion must be nonzero");
  return {pose.position, glm::normalize(rotation / scale)};
}

LidarPose compose(const LidarPose& base, const LidarPose& mounting) {
  const auto parent = normalizedPose(base);
  return normalizedPose({parent.position + parent.rotation * mounting.position,
                         parent.rotation * mounting.rotation});
}

} // namespace

RaycastLidar::RaycastLidar(RaycastLidarSettings settings) : settings_(std::move(settings)) {
  const auto& value = settings_;
  if (value.azimuthSamples == 0 || value.azimuthSamples > maxBeams ||
      value.elevationAngles.empty() || value.elevationAngles.size() > maxBeams / value.azimuthSamples)
    throw std::invalid_argument("LiDAR organized scan must contain 1..65536 beams");
  if (!std::isfinite(value.azimuthStart) || !std::isfinite(value.azimuthSpan) ||
      value.azimuthSpan < 0 || value.azimuthSpan > revolution ||
      (value.azimuthSpan == 0 && value.azimuthSamples != 1))
    throw std::invalid_argument("LiDAR azimuth span must be in [0,2*pi], with zero span only for one bearing");
  for (const auto elevation : value.elevationAngles)
    if (!std::isfinite(elevation) || elevation < -pi * .5 || elevation > pi * .5)
      throw std::invalid_argument("LiDAR elevations must be finite angles in [-pi/2,pi/2]");
  if (!std::isfinite(value.minRangeMeters) || !std::isfinite(value.maxRangeMeters) ||
      value.minRangeMeters < 0 || value.maxRangeMeters <= value.minRangeMeters)
    throw std::invalid_argument("LiDAR ranges require finite 0 <= minimum < maximum");
  if (!std::isfinite(value.noiseStdDevMeters) || value.noiseStdDevMeters < 0 ||
      !std::isfinite(value.dropoutProbability) || value.dropoutProbability < 0 || value.dropoutProbability > 1)
    throw std::invalid_argument("LiDAR noise must be nonnegative and dropout probability in [0,1]");
  if (!std::isfinite(value.noiseStdDevPerMeter) || value.noiseStdDevPerMeter < 0 ||
      !std::isfinite(value.noiseStdDevMeters + value.noiseStdDevPerMeter * value.maxRangeMeters))
    throw std::invalid_argument("LiDAR noiseStdDevPerMeter must be nonnegative with finite noise across the range interval");
  if (!std::isfinite(value.rangeResolutionMeters) || value.rangeResolutionMeters < 0)
    throw std::invalid_argument("LiDAR rangeResolutionMeters must be finite and nonnegative");
  settings_.baseFromSensor = normalizedPose(value.baseFromSensor);

  directions_.reserve(value.elevationAngles.size() * value.azimuthSamples);
  const auto start = std::remainder(value.azimuthStart, revolution);
  const auto intervals = value.azimuthSpan == revolution ? value.azimuthSamples : value.azimuthSamples - 1;
  for (const auto elevation : value.elevationAngles) {
    const auto horizontal = std::cos(elevation);
    for (uint32_t bearing = 0; bearing < value.azimuthSamples; ++bearing) {
      const double fraction = value.azimuthSamples == 1 ? 0.0 : double(bearing) / intervals;
      const auto azimuth = start + value.azimuthSpan * fraction;
      directions_.push_back({horizontal * std::cos(azimuth), horizontal * std::sin(azimuth),
                             std::sin(elevation)});
    }
  }
}

LidarScan RaycastLidar::scan(const LidarPose& worldFromBase, const LidarSchedule& schedule,
                             uint64_t tick, const LidarCast& cast) const {
  const auto ordinal = schedule.timing_.sampleIndex(tick);
  if (!cast)
    throw std::invalid_argument("LiDAR scan requires a raycast callback");
  LidarScan result{.tick = tick, .seconds = schedule.seconds(tick),
                   .worldFromSensor = compose(worldFromBase, settings_.baseFromSensor)};
  result.returns.reserve(directions_.size());
  const auto scanKey = sensor_noise::mix(sensor_noise::mix(settings_.seed) ^ ordinal);
  for (size_t beam = 0; beam < directions_.size(); ++beam) {
    const auto direction = glm::normalize(result.worldFromSensor.rotation * directions_[beam]);
    const auto displacement = direction * settings_.maxRangeMeters;
    if (!finite(displacement) || !finite(result.worldFromSensor.position + displacement))
      throw std::invalid_argument("LiDAR ray endpoint exceeds the finite numeric range");
    auto hit = cast(result.worldFromSensor.position, displacement);
    if (!hit) {
      result.returns.emplace_back();
      continue;
    }
    if (!std::isfinite(hit->fraction) || hit->fraction < 0 || hit->fraction > 1)
      throw std::invalid_argument("LiDAR raycast callback returned a fraction outside [0,1]");
    LidarReturn measurement{.entityId = std::move(hit->entityId)};
    const auto distance = hit->fraction * settings_.maxRangeMeters;
    if (settings_.recordIdealRanges)
      measurement.idealRangeMeters = distance;
    if (distance < settings_.minRangeMeters) {
      measurement.status = LidarReturnStatus::BelowMinimum;
    } else {
      const auto key = sensor_noise::mix(scanKey ^ uint64_t(beam));
      if (settings_.dropoutProbability > 0 && sensor_noise::uniform(key, 0) < settings_.dropoutProbability) {
        measurement.status = LidarReturnStatus::Dropped;
      } else {
        const auto standardDeviation = settings_.noiseStdDevMeters + settings_.noiseStdDevPerMeter * distance;
        auto range = distance + (standardDeviation == 0 ? 0 : standardDeviation * sensor_noise::normal(key));
        // Remainder gives the nearest bin with ties to even without a potentially overflowing quotient.
        if (settings_.rangeResolutionMeters > 0 && std::isfinite(range))
          range -= std::remainder(range, settings_.rangeResolutionMeters);
        if (!std::isfinite(range) || range < settings_.minRangeMeters || range > settings_.maxRangeMeters) {
          measurement.status = LidarReturnStatus::NoiseOutOfRange;
        } else {
          measurement.status = LidarReturnStatus::Valid;
          measurement.rangeMeters = range;
        }
      }
    }
    result.returns.push_back(std::move(measurement));
  }
  return result;
}

} // namespace container::robotics
