#pragma once

#include "IfcCurveReader.h"

namespace container::geometry::ifc::detail {
struct CurvedFaceTriangle {
  std::array<glm::vec3, 3> positions, normals;
};
// Input boundaries contain the exact shared edge samples. Refinement only
// inserts interior vertices, leaving adjacent faces' boundary meshes identical.
[[nodiscard]] std::optional<std::vector<CurvedFaceTriangle>>
meshIfcCurvedFace(const ParametricSurface &surface,
                  const std::vector<std::vector<glm::vec3>> &boundaries,
                  bool explicitOuter, bool sameSense, double metersPerUnit,
                  std::string &error);
} // namespace container::geometry::ifc::detail
