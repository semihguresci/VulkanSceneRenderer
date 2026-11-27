#pragma once

#include "Container/robotics/BimSceneSource.h"

#include <nlohmann/json_fwd.hpp>

namespace container::robotics {

[[nodiscard]] nlohmann::json bimSceneSourceJson(const BimSceneSource& source);

} // namespace container::robotics
