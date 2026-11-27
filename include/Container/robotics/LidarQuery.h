#pragma once

#include <glm/vec3.hpp>

#include <functional>
#include <optional>
#include <string>

namespace container::robotics {

struct LidarHit {
  double fraction{};
  std::string entityId;
};

// Return the nearest collision along origin + fraction * displacement, with
// fraction in [0, 1]. An empty result denotes a clear segment.
using LidarCast = std::function<std::optional<LidarHit>(glm::dvec3 origin, glm::dvec3 displacement)>;

} // namespace container::robotics
