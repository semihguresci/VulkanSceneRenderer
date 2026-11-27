#pragma once

#include "Container/robotics/LidarQuery.h"
#include "Container/robotics/SensorSchedule.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace container::robotics {

struct LidarPose {
  glm::dvec3 position{0.0};
  glm::dquat rotation{1.0, 0.0, 0.0, 0.0};
};

// Angles in radians, distances in metres. Sensor axes follow REP-103:
// +X forward, +Y left, +Z up. Elevation channels retain their authored order.
struct RaycastLidarSettings {
  uint32_t azimuthSamples{360};
  double azimuthStart{-std::numbers::pi};
  double azimuthSpan{2.0 * std::numbers::pi};
  std::vector<double> elevationAngles{0.0};
  double minRangeMeters{0.05};
  double maxRangeMeters{25.0};
  double noiseStdDevMeters{};
  double dropoutProbability{};
  uint64_t seed{};
  LidarPose baseFromSensor;
  // Range error standard deviation = noiseStdDevMeters + ideal range × noiseStdDevPerMeter.
  double noiseStdDevPerMeter{};
  // Round noisy ranges to the nearest bin in metres. Zero leaves ranges continuous.
  double rangeResolutionMeters{};
  bool recordIdealRanges{};
};

enum class LidarReturnStatus { Valid, NoHit, BelowMinimum, Dropped, NoiseOutOfRange };

struct LidarReturn {
  std::optional<double> rangeMeters;
  LidarReturnStatus status{LidarReturnStatus::NoHit};
  std::string entityId;
  // Unmodified collider distance, including near and rejected returns when requested.
  std::optional<double> idealRangeMeters;
  bool operator==(const LidarReturn&) const = default;
};

// The first scan occurs at firstTick. Each subsequent scan uses the first fixed
// tick at or after its ideal time. The rate cannot exceed one scan per tick.
class LidarSchedule {
public:
  LidarSchedule(uint32_t ticksPerSecond, uint32_t scansPerSecondMilliHz, uint64_t firstTick = 0)
      : timing_(ticksPerSecond, scansPerSecondMilliHz, firstTick) {}
  [[nodiscard]] bool due(uint64_t tick) const noexcept { return timing_.due(tick); }
  // Inclusive count of due scans from the first tick through tick.
  [[nodiscard]] uint64_t scansThrough(uint64_t tick) const { return timing_.samplesThrough(tick); }
  [[nodiscard]] double seconds(uint64_t tick) const noexcept { return timing_.seconds(tick); }
  [[nodiscard]] uint32_t ticksPerSecond() const noexcept { return timing_.ticksPerSecond(); }
  [[nodiscard]] uint32_t scansPerSecondMilliHz() const noexcept { return timing_.samplesPerSecondMilliHz(); }
  [[nodiscard]] uint64_t firstTick() const noexcept { return timing_.firstTick(); }

private:
  friend class RaycastLidar;
  SensorSchedule timing_;
};

struct LidarScan {
  uint64_t tick{};
  double seconds{};
  LidarPose worldFromSensor;
  std::vector<LidarReturn> returns;
};

class RaycastLidar {
public:
  explicit RaycastLidar(RaycastLidarSettings settings = {});
  [[nodiscard]] const RaycastLidarSettings& settings() const noexcept { return settings_; }
  // Returns and directions are elevation-major. A full revolution omits its
  // repeated endpoint. Partial sweeps with multiple samples include both ends.
  [[nodiscard]] std::span<const glm::dvec3> directions() const noexcept { return directions_; }
  [[nodiscard]] LidarScan scan(const LidarPose& worldFromBase, const LidarSchedule& schedule,
                              uint64_t tick, const LidarCast& cast) const;

private:
  RaycastLidarSettings settings_;
  std::vector<glm::dvec3> directions_;
};

} // namespace container::robotics
