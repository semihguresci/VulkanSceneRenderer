#include "Container/robotics/TwinTelemetry.h"

#include "TwinTelemetryLimits.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <numbers>
#include <stdexcept>
#include <tuple>
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

auto eventOrder(const TwinEvent& event) {
  return std::tuple{event.timestampNanoseconds, std::string_view(event.entityId), event.payload.index()};
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

void canonicalizeTwinRecording(TwinRecording& recording) {
  validateText(recording.sourceNamespace, maxIdentityBytes, "sourceNamespace");
  validateText(recording.clockId, maxLabelBytes, "clockId");
  validateSceneSource(recording.sceneSource);
  if (recording.events.empty() || recording.events.size() > maxEvents)
    invalid("events", "must contain 1..65,536 observations");

  size_t totalPlanPoints = 0;
  for (const auto& event : recording.events) {
    validateText(event.entityId, maxIdentityBytes, "event entityId");
    validateText(event.source, maxLabelBytes, "event source");
    if (event.payload.valueless_by_exception()) invalid("event payload", "is missing");
    if (const auto* pose = std::get_if<TwinRobotPose>(&event.payload)) {
      validatePoint(pose->position, "robot position");
      if (!std::isfinite(pose->yaw) || std::abs(pose->yaw) > maxYawRadians)
        invalid("robot yaw", "must be finite and in [-1000000000, 1000000000] radians");
    } else if (const auto* sensor = std::get_if<TwinSensorStatus>(&event.payload)) {
      validateText(sensor->state, maxLabelBytes, "sensor state");
    } else if (const auto* asset = std::get_if<TwinAssetState>(&event.payload)) {
      validateText(asset->state, maxLabelBytes, "asset state");
    } else {
      const auto& points = std::get<TwinPlannedPath>(event.payload).points;
      if (points.size() > maxPlanPoints || points.size() > maxTotalPlanPoints - totalPlanPoints)
        invalid("planned path", "exceeds the per-path or recording point limit");
      totalPlanPoints += points.size();
      for (const auto point : points) validatePoint(point, "planned path point");
    }
  }

  std::stable_sort(recording.events.begin(), recording.events.end(), [](const auto& a, const auto& b) {
    return eventOrder(a) < eventOrder(b);
  });
  const auto duplicate = std::ranges::adjacent_find(recording.events, [](const auto& a, const auto& b) {
    return eventOrder(a) == eventOrder(b);
  });
  if (duplicate != recording.events.end())
    invalid("events", "contain a duplicate kind and entity at timestamp " +
                      std::to_string(duplicate->timestampNanoseconds) + " for " + duplicate->entityId);
}

TwinReplay::TwinReplay(TwinRecording recording) : recording_(std::move(recording)) {
  canonicalizeTwinRecording(recording_);
  using TrackKey = std::pair<std::string_view, size_t>;
  std::map<TrackKey, std::vector<size_t>> tracks;
  for (size_t index = 0; index < recording_.events.size(); ++index) {
    const auto& event = recording_.events[index];
    tracks[{event.entityId, event.payload.index()}].push_back(index);
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
