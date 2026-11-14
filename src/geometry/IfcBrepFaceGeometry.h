#pragma once

#include "IfcCurveReader.h"

namespace container::geometry::ifc::detail {
struct CurvedFaceTriangle {
  std::array<glm::vec3, 3> positions, normals;
};
// Input boundaries contain the exact shared edge samples. Refinement only
// inserts interior vertices, leaving adjacent faces' boundary meshes identical.
// A validated spherical seam partitions the face along the source samples and
// a complementary cut. For a trimmed seam the first boundary is the real rim.
[[nodiscard]] std::optional<std::vector<CurvedFaceTriangle>>
meshIfcCurvedFace(const ParametricSurface &surface,
                  const std::vector<std::vector<glm::vec3>> &boundaries,
                  bool explicitOuter, bool sameSense, double metersPerUnit,
                  std::string &error,
                  std::span<const glm::vec3> sphericalSeam = {});
} // namespace container::geometry::ifc::detail
