#include "Container/robotics/RaycastLidar.h"

#include <algorithm>
#include <cmath>
#include <limits>
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

uint64_t denominator(uint32_t ticksPerSecond) {
  return uint64_t(ticksPerSecond) * 1000;
}

uint64_t periodsThrough(uint64_t elapsed, uint64_t numerator, uint64_t divisor) {
  // numerator <= divisor <= 1e9. The quotient term and sum are <= elapsed,
  // and the remainder product is < 1e18, including at UINT64_MAX ticks.
  return (elapsed / divisor) * numerator + ((elapsed % divisor) * numerator) / divisor;
}

uint64_t mix(uint64_t value) {
  // SplitMix64's fixed unsigned operations do not depend on STL distributions.
  value += 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

double uniform(uint64_t key, uint64_t channel) {
  return static_cast<double>(mix(key + channel * 0x9e3779b97f4a7c15ULL) >> 11) * 0x1.0p-53;
}

double normal(uint64_t key) {
  // Explicit Box-Muller mapping; 1-U is in (0,1], so log never sees zero.
  return std::sqrt(-2 * std::log(1 - uniform(key, 1))) *
         std::cos(revolution * uniform(key, 2));
}

} // namespace

LidarSchedule::LidarSchedule(uint32_t ticksPerSecond, uint32_t scansPerSecondMilliHz,
                             uint64_t firstTick)
    : ticksPerSecond_(ticksPerSecond), scanMilliHz_(scansPerSecondMilliHz), firstTick_(firstTick) {
  if (ticksPerSecond == 0 || ticksPerSecond > 1'000'000 || scansPerSecondMilliHz == 0 ||
      scansPerSecondMilliHz > denominator(ticksPerSecond))
    throw std::invalid_argument("LiDAR schedule requires 1..1000000 tick Hz and at most one scan per tick");
}

bool LidarSchedule::due(uint64_t tick) const noexcept {
  if (tick < firstTick_)
    return false;
  const auto elapsed = tick - firstTick_;
  if (elapsed == 0)
    return true;
  const auto divisor = denominator(ticksPerSecond_);
  // floor(t*n/d) advances precisely at ceil(k*d/n). Work with the remainder
  // instead of multiplying the potentially enormous absolute tick by n.
  return ((elapsed % divisor) * scanMilliHz_) % divisor < scanMilliHz_;
}

uint64_t LidarSchedule::scansThrough(uint64_t tick) const {
  if (tick < firstTick_)
    return 0;
  const auto count = periodsThrough(tick - firstTick_, scanMilliHz_, denominator(ticksPerSecond_));
  if (count == std::numeric_limits<uint64_t>::max())
    throw std::overflow_error("LiDAR inclusive scan count exceeds the integer range");
  return count + 1;
}

double LidarSchedule::seconds(uint64_t tick) const noexcept {
  return static_cast<double>(tick) / ticksPerSecond_;
}

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
  if (!schedule.due(tick))
    throw std::invalid_argument("LiDAR scan requested outside its fixed-tick schedule");
  if (!cast)
    throw std::invalid_argument("LiDAR scan requires a raycast callback");
  LidarScan result{.tick = tick, .seconds = schedule.seconds(tick),
                   .worldFromSensor = compose(worldFromBase, settings_.baseFromSensor)};
  result.returns.reserve(directions_.size());
  const auto ordinal = periodsThrough(tick - schedule.firstTick(), schedule.scansPerSecondMilliHz(),
                                      denominator(schedule.ticksPerSecond()));
  const auto scanKey = mix(mix(settings_.seed) ^ ordinal);
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
    if (distance < settings_.minRangeMeters) {
      measurement.status = LidarReturnStatus::BelowMinimum;
    } else {
      const auto key = mix(scanKey ^ uint64_t(beam));
      if (settings_.dropoutProbability > 0 && uniform(key, 0) < settings_.dropoutProbability) {
        measurement.status = LidarReturnStatus::Dropped;
      } else {
        const auto range = distance + (settings_.noiseStdDevMeters == 0 ? 0 :
                                       settings_.noiseStdDevMeters * normal(key));
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
