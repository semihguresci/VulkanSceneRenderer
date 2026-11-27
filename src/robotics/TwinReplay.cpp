#include "Container/robotics/TwinTelemetry.h"
#include "Container/robotics/SensorCamera.h"

#include "TwinTelemetryLimits.h"
#include "TwinTelemetryValidation.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <numbers>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace container::robotics {
namespace {

using namespace twin_telemetry_limits;
constexpr double fullTurn = 2 * std::numbers::pi;

[[noreturn]] void invalid(std::string_view field, std::string_view reason) {
  throw std::invalid_argument("Twin recording " + std::string(field) + " " + std::string(reason));
}

void validateText(std::string_view text, size_t maxBytes, std::string_view field) {
  if (text.empty() || text.size() > maxBytes || text.find_first_not_of(' ') == std::string_view::npos ||
      std::ranges::any_of(text, [](unsigned char byte) { return byte < 32 || byte == 127; }))
    invalid(field, "must be nonempty bounded text without control characters");
}

void validatePoint(glm::dvec3 point, std::string_view field) {
  const auto coordinate = [](double value) { return std::isfinite(value) && std::abs(value) <= maxPositionMeters; };
  if (!coordinate(point.x) || !coordinate(point.y) || !coordinate(point.z))
    invalid(field, "must have finite coordinates in [-100000000, 100000000] metres");
}

void validateYaw(double yaw, std::string_view field) {
  if (!std::isfinite(yaw) || std::abs(yaw) > maxYawRadians)
    invalid(field, "must be finite and in [-1000000000, 1000000000] radians");
}

void validateOccupancyGrid(const TwinOccupancyGrid& grid) {
  validatePoint(grid.origin, "occupancy grid origin");
  validateYaw(grid.yaw, "occupancy grid yaw");
  if (!std::isfinite(grid.resolutionMeters) || grid.resolutionMeters <= 0.0)
    invalid("occupancy grid resolutionMeters", "must be finite and positive");
  if (!grid.width || grid.width > maxOccupancyDimension || !grid.height || grid.height > maxOccupancyDimension)
    invalid("occupancy grid dimensions", "must each be in [1, 128]");
  if (grid.cells.size() != std::size_t(grid.width) * grid.height ||
      !std::ranges::all_of(grid.cells, [](char cell) { return cell == '.' || cell == '#' || cell == '?'; }))
    invalid("occupancy grid cells", "must contain width * height characters from '.#?'");
  const auto x = glm::dvec3(std::cos(grid.yaw), std::sin(grid.yaw), 0.0) * (grid.resolutionMeters * grid.width);
  const auto y = glm::dvec3(-std::sin(grid.yaw), std::cos(grid.yaw), 0.0) * (grid.resolutionMeters * grid.height);
  validatePoint(grid.origin + x, "occupancy grid corner");
  validatePoint(grid.origin + y, "occupancy grid corner");
  validatePoint(grid.origin + x + y, "occupancy grid corner");
}

void validateSceneSource(const BimSceneSource& source) {
  if (!std::isfinite(source.metersPerUnscaledRendererUnit) || source.metersPerUnscaledRendererUnit <= 0)
    invalid("sceneSource.metersPerUnscaledRendererUnit", "must be finite and positive");
  if (source.layers.empty() || source.layers.size() > maxSceneLayers)
    invalid("sceneSource.layers", "must contain 1..256 ordered source fingerprints");
  for (const auto& layer : source.layers) {
    if (layer.digest.size() != 64 || !std::ranges::all_of(layer.digest, [](char digit) {
          return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f');
        }) || layer.byteSize == 0)
      invalid("sceneSource.layers", "require lowercase SHA-256 digests and positive byte sizes");
  }
}

glm::dvec3 lerpPosition(glm::dvec3 a, glm::dvec3 b, double fraction) {
  return {std::lerp(a.x, b.x, fraction), std::lerp(a.y, b.y, fraction), std::lerp(a.z, b.z, fraction)};
}

double length(glm::dvec3 vector) { return std::hypot(vector.x, vector.y, vector.z); }

double distanceToPath(glm::dvec3 position, const std::vector<glm::dvec3>& points) {
  double distance = length(position - points.front());
  for (size_t index = 1; index < points.size(); ++index) {
    const auto start = points[index - 1];
    const auto delta = points[index] - start;
    const auto span = length(delta);
    if (span == 0) continue;
    const auto direction = delta / span;
    const auto offset = position - start;
    const auto along = std::clamp(glm::dot(offset, direction), 0.0, span);
    distance = std::min(distance, length(offset - along * direction));
  }
  return distance;
}

} // namespace

std::array<glm::dvec3, 8> twinFrustumCorners(const TwinSensorFrustum& frustum) {
  validateSensorCameraSettings({.width = frustum.width, .height = frustum.height,
                                .fx = frustum.fx, .fy = frustum.fy, .cx = frustum.cx, .cy = frustum.cy,
                                .nearMeters = frustum.nearMeters, .farMeters = frustum.farMeters});
  validateSensorCameraFrame({.worldFromCamera = frustum.worldFromCamera});
  validatePoint(glm::dvec3(frustum.worldFromCamera[3]), "sensor frustum origin");

  const double width = frustum.width;
  const double height = frustum.height;
  const std::array<double, 4> imageX{0, width, width, 0};
  const std::array<double, 4> imageY{0, 0, height, height};
  const std::array<double, 2> depths{frustum.nearMeters, frustum.farMeters};
  std::array<glm::dvec3, 8> corners{};
  for (size_t plane = 0; plane < depths.size(); ++plane) {
    const auto depth = depths[plane];
    for (size_t corner = 0; corner < imageX.size(); ++corner) {
      // Pixels increase downward while camera Y increases upward.
      const auto x = (imageX[corner] - frustum.cx) / frustum.fx * depth;
      const auto y = (frustum.cy - imageY[corner]) / frustum.fy * depth;
      const auto point = glm::dvec3(frustum.worldFromCamera * glm::dvec4(x, y, -depth, 1.0));
      validatePoint(point, "sensor frustum corner");
      corners[plane * imageX.size() + corner] = point;
    }
  }
  return corners;
}

namespace twin_telemetry_validation {

void validateRecordingMetadata(const TwinRecording& recording) {
  validateText(recording.sourceNamespace, maxIdentityBytes, "sourceNamespace");
  validateText(recording.clockId, maxLabelBytes, "clockId");
  validateSceneSource(recording.sceneSource);
}

EventGeometrySize validateEvent(const TwinEvent& event) {
  validateText(event.entityId, maxIdentityBytes, "event entityId");
  validateText(event.source, maxLabelBytes, "event source");
  if (event.payload.valueless_by_exception()) invalid("event payload", "is missing");
  return std::visit([]<class Payload>(const Payload& payload) -> EventGeometrySize {
    if constexpr (std::is_same_v<Payload, TwinRobotPose>) {
      validatePoint(payload.position, "robot position");
      validateYaw(payload.yaw, "robot yaw");
    } else if constexpr (std::is_same_v<Payload, TwinPlannedPath>) {
      if (payload.points.size() > maxPlanPoints)
        invalid("planned path", "exceeds the per-path point limit");
      for (const auto point : payload.points) validatePoint(point, "planned path point");
      return {.planPoints = payload.points.size()};
    } else if constexpr (std::is_same_v<Payload, TwinSensorStatus>) {
      validateText(payload.state, maxLabelBytes, "sensor state");
    } else if constexpr (std::is_same_v<Payload, TwinAssetState>) {
      validateText(payload.state, maxLabelBytes, "asset state");
    } else if constexpr (std::is_same_v<Payload, TwinRobotHealth>) {
      validateText(payload.state, maxLabelBytes, "robot health state");
      if (payload.batteryPercent && (!std::isfinite(*payload.batteryPercent) ||
          *payload.batteryPercent < 0.0 || *payload.batteryPercent > 100.0))
        invalid("robot health batteryPercent", "must be finite and in [0, 100]");
    } else if constexpr (std::is_same_v<Payload, TwinTaskStatus>) {
      validateText(payload.taskId, maxIdentityBytes, "taskId");
      validateText(payload.state, maxLabelBytes, "task state");
      if (payload.progress && (!std::isfinite(*payload.progress) ||
          *payload.progress < 0.0 || *payload.progress > 1.0))
        invalid("task progress", "must be finite and in [0, 1]");
    } else if constexpr (std::is_same_v<Payload, TwinOperatorAlarm>) {
      validateText(payload.alarmId, maxIdentityBytes, "alarmId");
      validateText(payload.message, maxLabelBytes, "alarm message");
      if (payload.severity != "info" && payload.severity != "warning" && payload.severity != "critical")
        invalid("alarm severity", "must be info, warning, or critical");
      validatePoint(payload.position, "alarm position");
    } else if constexpr (std::is_same_v<Payload, TwinSensorFrustum>) {
      (void)twinFrustumCorners(payload);
    } else {
      static_assert(std::is_same_v<Payload, TwinOccupancyGrid>, "Unhandled twin telemetry payload");
      validateOccupancyGrid(payload);
      return {.occupancyCells = payload.cells.size()};
    }
    return {};
  }, event.payload);
}

} // namespace twin_telemetry_validation

void canonicalizeTwinRecording(TwinRecording& recording) {
  using namespace twin_telemetry_validation;
  validateRecordingMetadata(recording);
  if (recording.events.empty() || recording.events.size() > maxEvents)
    invalid("events", "must contain 1..65,536 observations");

  EventGeometrySize total;
  for (const auto& event : recording.events) {
    const auto geometry = validateEvent(event);
    if (geometry.planPoints > maxTotalPlanPoints - total.planPoints)
      invalid("planned path", "exceeds the recording point limit");
    if (geometry.occupancyCells > maxTotalOccupancyCells - total.occupancyCells)
      invalid("occupancy grid", "exceeds the recording cell limit");
    total.planPoints += geometry.planPoints;
    total.occupancyCells += geometry.occupancyCells;
  }

  std::stable_sort(recording.events.begin(), recording.events.end(), [](const auto& a, const auto& b) {
    return eventOrder(a) < eventOrder(b);
  });
  const auto duplicate = std::ranges::adjacent_find(recording.events, [](const auto& a, const auto& b) {
    return eventOrder(a) == eventOrder(b);
  });
  if (duplicate != recording.events.end()) {
    auto context = "contain a duplicate track at timestamp " + std::to_string(duplicate->timestampNanoseconds) +
                   " for " + duplicate->entityId;
    if (const auto* task = std::get_if<TwinTaskStatus>(&duplicate->payload))
      context += " task " + task->taskId;
    else if (const auto* alarm = std::get_if<TwinOperatorAlarm>(&duplicate->payload))
      context += " alarm " + alarm->alarmId;
    invalid("events", context);
  }
}

TwinReplay::TwinReplay(TwinRecording recording) : recording_(std::move(recording)) {
  canonicalizeTwinRecording(recording_);
  using namespace twin_telemetry_validation;
  std::map<TrackKey, std::vector<size_t>> tracks;
  for (size_t index = 0; index < recording_.events.size(); ++index) {
    const auto& event = recording_.events[index];
    tracks[trackKey(event)].push_back(index);
  }
  tracks_.reserve(tracks.size());
  for (auto& [key, events] : tracks) tracks_.push_back(std::move(events));
}

uint64_t TwinReplay::firstTimestamp() const noexcept {
  return recording_.events.empty() ? 0 : recording_.events.front().timestampNanoseconds;
}

uint64_t TwinReplay::lastTimestamp() const noexcept {
  return recording_.events.empty() ? 0 : recording_.events.back().timestampNanoseconds;
}

TwinReplaySample TwinReplay::sample(uint64_t timestampNanoseconds) const {
  TwinReplaySample result{.timestampNanoseconds = timestampNanoseconds};
  result.tracks.reserve(tracks_.size());
  std::map<std::string_view, const TwinPlannedPath*> freshPlans;
  for (const auto& indexes : tracks_) {
    const auto next = std::ranges::upper_bound(indexes, timestampNanoseconds, {}, [&](size_t index) {
      return recording_.events[index].timestampNanoseconds;
    });
    if (next == indexes.begin()) continue;
    const auto& prior = recording_.events[*std::prev(next)];
    const auto age = timestampNanoseconds - prior.timestampNanoseconds;
    TwinTrackSample track{.event = prior, .stale = age > recording_.staleAfterNanoseconds};
    if (auto* pose = std::get_if<TwinRobotPose>(&track.event.payload)) {
      const auto count = std::min(maxObservedPoints, static_cast<size_t>(next - indexes.begin()));
      track.observedPath.reserve(count);
      for (auto point = next - static_cast<std::ptrdiff_t>(count); point != next; ++point)
        track.observedPath.push_back(std::get<TwinRobotPose>(recording_.events[*point].payload).position);
      if (age != 0 && next != indexes.end()) {
        const auto& later = recording_.events[*next];
        const auto gap = later.timestampNanoseconds - prior.timestampNanoseconds;
        if (gap <= recording_.staleAfterNanoseconds) {
          const auto& laterPose = std::get<TwinRobotPose>(later.payload);
          const auto fraction = static_cast<double>(age) / static_cast<double>(gap);
          pose->position = lerpPosition(pose->position, laterPose.position, fraction);
          const auto yaw = std::remainder(pose->yaw, fullTurn);
          const auto delta = std::remainder(std::remainder(laterPose.yaw, fullTurn) - yaw, fullTurn);
          pose->yaw = std::remainder(yaw + delta * fraction, fullTurn);
        }
      }
    } else if (const auto* plan = std::get_if<TwinPlannedPath>(&prior.payload);
               plan && !track.stale && !plan->points.empty()) {
      freshPlans.emplace(prior.entityId, plan);
    }
    result.tracks.push_back(std::move(track));
  }

  for (auto& track : result.tracks) {
    const auto* pose = std::get_if<TwinRobotPose>(&track.event.payload);
    if (!pose || track.stale) continue;
    if (const auto plan = freshPlans.find(track.event.entityId); plan != freshPlans.end())
      track.distanceToPlanMeters = distanceToPath(pose->position, plan->second->points);
  }
  return result;
}

} // namespace container::robotics
