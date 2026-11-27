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
  const auto* task = std::get_if<TwinTaskStatus>(&event.payload);
  return {event.entityId, event.payload.index(), task ? std::string_view(task->taskId) : std::string_view{}};
}

inline auto eventOrder(const TwinEvent& event) noexcept {
  return std::pair{event.timestampNanoseconds, trackKey(event)};
}

void validateRecordingMetadata(const TwinRecording& recording);
// Returns the number of validated path points for the recording-wide budget.
std::size_t validateEvent(const TwinEvent& event);

} // namespace container::robotics::twin_telemetry_validation
