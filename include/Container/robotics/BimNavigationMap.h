#pragma once

#include "Container/robotics/BimCollisionGeometry.h"
#include "Container/robotics/BimWalkability.h"

#include <glm/vec2.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace container::robotics {

enum class BimNavigationCell : uint8_t { Occupied = 0, Unknown = 127, Free = 255 };

struct BimNavigationMapSettings {
  // The lower left corner, in building-local REP-103 metres.
  glm::dvec2 originMeters{0.0};
  uint32_t width{};
  uint32_t height{};
  double resolutionMeters{0.1};
  double floorMeters{};
  double floorToleranceMeters{0.02};
  double robotRadiusMeters{0.25};
  double robotHeightMeters{1.6};
};

// Returns bottom-up row-major cells in the unrotated REP-103 map frame.
// Only a selected, supported floor band is available to the planner.
[[nodiscard]] std::vector<BimNavigationCell> rasterizeBimNavigationMap(
    const CookedBimCollision& geometry, std::span<const BimWalkableSurface> support,
    std::span<const std::string> openDoorIds, const BimNavigationMapSettings& settings);

} // namespace container::robotics
