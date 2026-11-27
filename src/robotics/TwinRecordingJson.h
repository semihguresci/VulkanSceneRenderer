#pragma once

#include "Container/robotics/TwinIngest.h"
#include "Container/robotics/TwinTelemetry.h"

#include <nlohmann/json_fwd.hpp>

namespace container::robotics::twin_json {

inline constexpr std::size_t maxIngestLineBytes = 512 * 1024;

[[nodiscard]] BimSceneSource parseSceneSource(const nlohmann::json& source);
[[nodiscard]] TwinRobotContact parseRobotContact(const nlohmann::json& contact, std::string_view context);
[[nodiscard]] nlohmann::json point(const glm::dvec3& value);
[[nodiscard]] nlohmann::json event(const TwinEvent& value);
[[nodiscard]] uint32_t recordingVersion(const TwinRecording& value);
[[nodiscard]] TwinIngestMessage parseIngestMessage(std::string_view line);

} // namespace container::robotics::twin_json
