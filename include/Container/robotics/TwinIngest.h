#pragma once

#include "Container/robotics/TwinTelemetry.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace container::robotics {

struct RoboticsScene;

enum class TwinAssetReference { EntityId, IfcGuid };

enum class TwinIngestCode {
  Accepted,
  AcceptedOutOfOrder,
  Duplicate,
  ClockMismatch,
  ReceiptTimeRegression,
  FutureObservation,
  StaleObservation,
  InvalidEvent,
  UnknownAsset,
  AmbiguousAsset,
  Conflict,
  LimitExceeded,
};

[[nodiscard]] std::string_view twinIngestCodeName(TwinIngestCode code) noexcept;

struct TwinIngestMessage {
  std::string clockId;
  uint64_t receivedTimestampNanoseconds{};
  // On BIM asset-state events this selects what event.entityId denotes.
  TwinAssetReference assetReference{TwinAssetReference::EntityId};
  TwinEvent event;
};

struct TwinIngestReceipt {
  TwinIngestCode code{TwinIngestCode::InvalidEvent};
  std::string diagnostic;
  std::optional<std::string> entityId;
  std::optional<std::string> ifcGuid;
};

// A single-owner, transport-independent session. It copies the semantic BIM
// identity catalog and records observations without changing source geometry.
class TwinIngest {
public:
  TwinIngest(const RoboticsScene& scene, std::vector<util::ContentFingerprint> layers,
             std::string clockId, uint64_t staleAfterNanoseconds = 2'000'000'000);

  // The receipt watermark advances for each matching-clock, monotonic envelope,
  // including rejected observations. Freshness is checked before idempotency.
  [[nodiscard]] TwinIngestReceipt ingest(TwinIngestMessage message);

  // Canonically ordered snapshot of accepted history. An empty snapshot has
  // valid session metadata but carries no observations to write or replay.
  [[nodiscard]] TwinRecording recording() const;

  // Last accepted observation at or before now on each independent track.
  // Uses observation time, does not interpolate, and leaves trails/plan
  // deviation empty. A query never changes receipt ordering or freshness.
  [[nodiscard]] TwinReplaySample latest(uint64_t nowNanoseconds) const;

private:
  using TrackKey = std::tuple<std::string, std::size_t, std::string>;
  using TrackEvents = std::map<uint64_t, TwinEvent>;

  TwinRecording metadata_;
  std::map<std::string, std::string, std::less<>> guidByEntity_;
  // An empty value marks an authored GUID shared by multiple occurrences.
  std::map<std::string, std::string, std::less<>> entityByGuid_;
  std::map<TrackKey, TrackEvents, std::less<>> tracks_;
  std::optional<uint64_t> receiptWatermark_;
  std::size_t eventCount_{};
  std::size_t planPoints_{};
  std::size_t occupancyCells_{};
};

} // namespace container::robotics
