#pragma once

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <functional>
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
};

struct LidarHit {
  double fraction{};
  std::string entityId;
};

// Return the nearest collision along origin + fraction * displacement, with
// fraction in [0, 1]. An empty result denotes a clear segment.
using LidarCast = std::function<std::optional<LidarHit>(glm::dvec3 origin, glm::dvec3 displacement)>;

enum class LidarReturnStatus { Valid, NoHit, BelowMinimum, Dropped, NoiseOutOfRange };

struct LidarReturn {
  std::optional<double> rangeMeters;
  LidarReturnStatus status{LidarReturnStatus::NoHit};
  std::string entityId;
  bool operator==(const LidarReturn&) const = default;
};

// The first scan occurs at firstTick. Each subsequent scan uses the first fixed
// tick at or after its ideal time. The rate cannot exceed one scan per tick.
class LidarSchedule {
public:
  LidarSchedule(uint32_t ticksPerSecond, uint32_t scansPerSecondMilliHz, uint64_t firstTick = 0);
  [[nodiscard]] bool due(uint64_t tick) const noexcept;
  // Inclusive count of due scans from the first tick through tick.
  [[nodiscard]] uint64_t scansThrough(uint64_t tick) const;
  [[nodiscard]] double seconds(uint64_t tick) const noexcept;
  [[nodiscard]] uint32_t ticksPerSecond() const noexcept { return ticksPerSecond_; }
  [[nodiscard]] uint32_t scansPerSecondMilliHz() const noexcept { return scanMilliHz_; }
  [[nodiscard]] uint64_t firstTick() const noexcept { return firstTick_; }

private:
  uint32_t ticksPerSecond_;
  uint32_t scanMilliHz_;
  uint64_t firstTick_;
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
