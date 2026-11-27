#pragma once

#include "Container/robotics/BimSceneSource.h"

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace container::robotics {

// Building-local REP-103 metres and a yaw about +Z in radians.
struct TwinRobotPose {
  glm::dvec3 position{0.0};
  double yaw{};
  bool operator==(const TwinRobotPose&) const = default;
};

struct TwinSensorStatus {
  std::string state;
  bool operator==(const TwinSensorStatus&) const = default;
};
struct TwinAssetState {
  std::string state;
  bool operator==(const TwinAssetState&) const = default;
};
struct TwinPlannedPath {
  std::vector<glm::dvec3> points;
  bool operator==(const TwinPlannedPath&) const = default;
};
struct TwinRobotHealth {
  std::string state;
  std::optional<double> batteryPercent;
  bool operator==(const TwinRobotHealth&) const = default;
};
struct TwinTaskStatus {
  std::string taskId;
  std::string state;
  std::optional<double> progress;
  bool operator==(const TwinTaskStatus&) const = default;
};
using TwinPayload = std::variant<TwinRobotPose, TwinSensorStatus, TwinAssetState, TwinPlannedPath,
                                 TwinRobotHealth, TwinTaskStatus>;

struct TwinEvent {
  uint64_t timestampNanoseconds{};
  std::string entityId;
  std::string source;
  TwinPayload payload;
  bool operator==(const TwinEvent&) const = default;
};

struct TwinRecording {
  std::string sourceNamespace;
  BimSceneSource sceneSource;
  // All timestamps must already use this same clock domain.
  std::string clockId;
  uint64_t staleAfterNanoseconds{2'000'000'000};
  std::vector<TwinEvent> events;
};

struct TwinTrackSample {
  // Holds the latest event at or before the sample time. Its robot pose may
  // be interpolated, but timestamp and source still identify the prior event.
  TwinEvent event;
  bool stale{};
  // Robot pose tracks retain the observed trail through this time.
  std::vector<glm::dvec3> observedPath;
  std::optional<double> distanceToPlanMeters;
};

struct TwinReplaySample {
  uint64_t timestampNanoseconds{};
  std::vector<TwinTrackSample> tracks;
};

// Validate payloads and sort in canonical order. Reject duplicate timestamps
// on an entity and payload track, with each task ID on its own track.
void canonicalizeTwinRecording(TwinRecording& recording);

class TwinReplay {
public:
  explicit TwinReplay(TwinRecording recording);

  [[nodiscard]] const TwinRecording& recording() const noexcept { return recording_; }
  [[nodiscard]] uint64_t firstTimestamp() const noexcept;
  [[nodiscard]] uint64_t lastTimestamp() const noexcept;
  [[nodiscard]] TwinReplaySample sample(uint64_t timestampNanoseconds) const;

private:
  TwinRecording recording_;
  std::vector<std::vector<std::size_t>> tracks_;
};

[[nodiscard]] TwinRecording parseTwinRecording(std::string_view json);
[[nodiscard]] TwinRecording loadTwinRecording(const std::filesystem::path& path);
[[nodiscard]] std::string serializeTwinRecording(const TwinRecording& recording);

} // namespace container::robotics
