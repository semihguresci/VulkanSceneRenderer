#pragma once

#include "Container/robotics/TwinTelemetry.h"

#include <nlohmann/json_fwd.hpp>

namespace container::robotics::twin_json {

[[nodiscard]] BimSceneSource parseSceneSource(const nlohmann::json& source);
[[nodiscard]] nlohmann::json point(const glm::dvec3& value);
[[nodiscard]] nlohmann::json event(const TwinEvent& value);

} // namespace container::robotics::twin_json
