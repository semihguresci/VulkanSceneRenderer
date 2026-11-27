#include "Container/robotics/TwinIngest.h"

#include "Container/robotics/RoboticsScene.h"
#include "TwinTelemetryLimits.h"
#include "TwinTelemetryValidation.h"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace container::robotics {
namespace {

using twin_telemetry_limits::maxEvents;
using twin_telemetry_limits::maxTotalPlanPoints;
using twin_telemetry_validation::eventOrder;
using twin_telemetry_validation::trackKey;
using twin_telemetry_validation::validateEvent;
using twin_telemetry_validation::validateRecordingMetadata;

} // namespace

std::string_view twinIngestCodeName(TwinIngestCode code) noexcept {
  using enum TwinIngestCode;
  switch (code) {
    case Accepted: return "accepted";
    case AcceptedOutOfOrder: return "accepted-out-of-order";
    case Duplicate: return "duplicate";
    case ClockMismatch: return "clock-mismatch";
    case ReceiptTimeRegression: return "receipt-time-regression";
    case FutureObservation: return "future-observation";
    case StaleObservation: return "stale-observation";
    case InvalidEvent: return "invalid-event";
    case UnknownAsset: return "unknown-asset";
    case AmbiguousAsset: return "ambiguous-asset";
    case Conflict: return "conflict";
    case LimitExceeded: return "limit-exceeded";
  }
  return "invalid-code";
}

TwinIngest::TwinIngest(const RoboticsScene& scene, std::vector<util::ContentFingerprint> layers,
                       std::string clockId, uint64_t staleAfterNanoseconds)
    : metadata_{.sourceNamespace = scene.sourceNamespace,
                .sceneSource = makeBimSceneSource(std::move(layers), scene.frame),
                .clockId = std::move(clockId), .staleAfterNanoseconds = staleAfterNanoseconds} {
  validateRecordingMetadata(metadata_);
  for (const auto& entity : scene.entities) {
    if (entity.id.empty() || !guidByEntity_.try_emplace(entity.id, entity.guid).second)
      throw std::invalid_argument("Twin ingest requires nonempty, unique semantic BIM entity IDs");
    if (!entity.guid.empty()) {
      auto [entry, inserted] = entityByGuid_.try_emplace(entity.guid, entity.id);
      if (!inserted)
        entry->second.clear();
    }
  }
}

TwinIngestReceipt TwinIngest::ingest(TwinIngestMessage message) {
  using enum TwinIngestCode;
  const auto receiptTime = message.receivedTimestampNanoseconds;
  auto& event = message.event;
  const auto timestamp = event.timestampNanoseconds;
  if (message.clockId != metadata_.clockId)
    return {ClockMismatch, "Observation clock differs from the ingest session clock"};
  if (receiptWatermark_ && receiptTime < *receiptWatermark_)
    return {ReceiptTimeRegression, "Receipt timestamp precedes the last matching-clock receipt"};
  receiptWatermark_ = receiptTime;
  if (timestamp > receiptTime)
    return {FutureObservation, "Observation timestamp is later than its receipt"};
  if (receiptTime - timestamp > metadata_.staleAfterNanoseconds)
    return {StaleObservation, "Observation age exceeds the ingest freshness window"};

  std::size_t points = 0;
  try {
    points = validateEvent(event);
  } catch (const std::invalid_argument& error) {
    return {InvalidEvent, error.what()};
  }

  std::optional<std::string> guid;
  if (std::holds_alternative<TwinAssetState>(event.payload)) {
    if (message.assetReference == TwinAssetReference::IfcGuid) {
      const auto asset = entityByGuid_.find(event.entityId);
      if (asset == entityByGuid_.end())
        return {UnknownAsset, "No BIM occurrence uses authored IFC GUID '" + event.entityId + "'"};
      if (asset->second.empty())
        return {AmbiguousAsset, "Use an occurrence entity ID for shared IFC GUID '" + event.entityId + "'"};
      event.entityId = asset->second;
      try {
        (void)validateEvent(event);
      } catch (const std::invalid_argument& error) {
        return {InvalidEvent, error.what()};
      }
    } else if (message.assetReference != TwinAssetReference::EntityId) {
      return {InvalidEvent, "Unknown BIM asset reference kind"};
    }
    const auto asset = guidByEntity_.find(event.entityId);
    if (asset == guidByEntity_.end())
      return {UnknownAsset, "No BIM occurrence has entity ID '" + event.entityId + "'"};
    if (!asset->second.empty())
      guid = asset->second;
  } else if (message.assetReference != TwinAssetReference::EntityId) {
    return {InvalidEvent, "IFC GUID addressing applies only to BIM asset-state events"};
  }

  const auto key = trackKey(event);
  auto track = tracks_.find(key);
  if (track != tracks_.end()) {
    const auto sameTime = track->second.find(timestamp);
    if (sameTime != track->second.end()) {
      if (sameTime->second == event)
        return {Duplicate, "Identical observation already recorded", event.entityId, std::move(guid)};
      return {Conflict, "A different observation already exists for this track and timestamp",
              event.entityId, std::move(guid)};
    }
  }
  if (eventCount_ == maxEvents || points > maxTotalPlanPoints - planPoints_)
    return {LimitExceeded, "The ingest session has reached its observation or planned-point limit",
            event.entityId, std::move(guid)};
  const bool older = track != tracks_.end() && timestamp < track->second.rbegin()->first;
  TwinIngestReceipt receipt{older ? AcceptedOutOfOrder : Accepted,
      older ? "Accepted into history while retaining the newer track observation" : "Observation accepted",
      event.entityId, std::move(guid)};
  if (track == tracks_.end()) {
    TrackKey ownedKey(key);
    TrackEvents events;
    events.emplace(timestamp, std::move(event));
    tracks_.emplace(std::move(ownedKey), std::move(events));
  } else {
    track->second.emplace(timestamp, std::move(event));
  }
  ++eventCount_;
  planPoints_ += points;
  return receipt;
}

TwinRecording TwinIngest::recording() const {
  auto result = metadata_;
  result.events.reserve(eventCount_);
  for (const auto& [key, track] : tracks_)
    for (const auto& [time, event] : track)
      result.events.push_back(event);
  std::ranges::sort(result.events, [](const auto& a, const auto& b) {
    return eventOrder(a) < eventOrder(b);
  });
  return result;
}

TwinReplaySample TwinIngest::latest(uint64_t nowNanoseconds) const {
  TwinReplaySample sample{.timestampNanoseconds = nowNanoseconds};
  sample.tracks.reserve(tracks_.size());
  for (const auto& [key, track] : tracks_) {
    const auto next = track.upper_bound(nowNanoseconds);
    if (next == track.begin())
      continue;
    const auto& [time, event] = *std::prev(next);
    sample.tracks.push_back({.event = event, .stale = nowNanoseconds - time > metadata_.staleAfterNanoseconds});
  }
  return sample;
}

} // namespace container::robotics
