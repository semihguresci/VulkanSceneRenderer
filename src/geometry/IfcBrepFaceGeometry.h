#pragma once

#include "IfcCurveReader.h"

namespace container::geometry::ifc::detail {
struct CurvedFaceTriangle {
  std::array<glm::vec3, 3> positions, normals;
};
struct CurvedFaceChartLoop {
  std::vector<glm::dvec2> vertices;
  glm::dvec2 closure;
};

// Project the unchanged shared samples in cache order, including the terminal
// sample of a closed edge. Each periodic lift is relative to this edge only.
// Throws if the samples do not identify a continuous, unambiguous chart.
[[nodiscard]] std::vector<glm::dvec2>
projectIfcBrepSplineEdge(const ParametricSurface &surface,
                         std::span<const glm::vec3> sharedPoints,
                         double metersPerUnit, std::stop_token stop = {});

// Find crease crossings inside one edge's original native sample intervals.
// Returned additions keep the source curve's exact positions and traversal
// order.
[[nodiscard]] std::vector<CurveSample>
sampleIfcBrepSplineEdgeCreases(const ParametricSurface &surface,
                               const ParametricCurve &source,
                               std::span<const CurveSample> samples,
                               double metersPerUnit, std::stop_token stop = {});

// Input boundaries contain the exact shared edge samples. Refinement only
// inserts interior vertices, leaving adjacent faces' boundary meshes identical.
// A validated polar seam partitions the face along the source samples and a
// complementary cut. For a trimmed seam the first boundary is the real rim.
[[nodiscard]] std::optional<std::vector<CurvedFaceTriangle>>
meshIfcCurvedFace(const ParametricSurface &surface,
                  const std::vector<std::vector<glm::vec3>> &boundaries,
                  bool explicitOuter, bool sameSense, double metersPerUnit,
                  std::string &error,
                  std::span<const glm::vec3> polarSeam = {},
                  std::span<const CurvedFaceChartLoop> authoredChart = {});
} // namespace container::geometry::ifc::detail
