#pragma once

#include "Container/robotics/RoboticsScene.h"

#include <array>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace container::robotics::room_geometry {

struct Bounds {
  glm::dvec3 minimum, maximum;
};

struct Volume {
  Bounds bounds;
  std::vector<std::array<glm::dvec3, 3>> triangles;
  // Prepared once by loadVolume. Keep the volume immutable during probing.
  bool usableInterior{};
  std::vector<size_t> interiorFaceIndices;
};

// No renderer/GPU data is accessed. Every mesh part must have a usable metric
// transform and range; incomplete/invalid/budget-exceeding data returns nullopt.
// Open meshes can still supply bounds for doors/openings, but cannot certify
// room interiors in interiorIntervals().
[[nodiscard]] std::optional<Volume> loadVolume(
    const geometry::dotbim::Model&, const SemanticEntity&);

// Signed metre distances on the complete line, not just its forward ray.
// Only closed, consistently oriented boundaries certify an interior. Exact
// duplicate faces and shared planar face hits are collapsed. Nonmanifold,
// coplanar/tangent or numerically ambiguous intersections return no intervals.
// Direction must be finite, normalized and horizontal (+Z is up).
[[nodiscard]] std::vector<std::pair<double, double>> interiorIntervals(
    const Volume&, glm::dvec3 origin, glm::dvec3 normalizedHorizontalDirection);

} // namespace container::robotics::room_geometry
