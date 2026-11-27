#pragma once

#include "Container/robotics/TwinTelemetry.h"

#include <cstddef>
#include <string_view>
#include <tuple>
#include <utility>

namespace container::robotics::twin_telemetry_validation {

// Key fields borrow strings owned by the event. Keep a copy of the strings
// when indexing separately owned or movable events.
using TrackKey = std::tuple<std::string_view, std::size_t, std::string_view>;

inline TrackKey trackKey(const TwinEvent& event) noexcept {
  std::string_view subId;
  if (const auto* task = std::get_if<TwinTaskStatus>(&event.payload))
    subId = task->taskId;
  else if (const auto* alarm = std::get_if<TwinOperatorAlarm>(&event.payload))
    subId = alarm->alarmId;
  return {event.entityId, event.payload.index(), subId};
}

inline auto eventOrder(const TwinEvent& event) noexcept {
  return std::pair{event.timestampNanoseconds, trackKey(event)};
}

void validateRecordingMetadata(const TwinRecording& recording);

struct EventGeometrySize {
  std::size_t planPoints{};
  std::size_t occupancyCells{};
};

// Returns validated geometry sizes for the independent recording-wide limits.
EventGeometrySize validateEvent(const TwinEvent& event);

} // namespace container::robotics::twin_telemetry_validation
