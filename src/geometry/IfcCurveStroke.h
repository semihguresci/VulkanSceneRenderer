#pragma once

#include "IfcStepTypes.h"

#include <cstddef>
#include <optional>
#include <span>
#include <stop_token>
#include <unordered_map>
#include <vector>

#include <glm/vec3.hpp>

namespace container::geometry::ifc::detail {

struct IfcCurveStroke {
  double width = 0;
  // Alternating visible and invisible lengths in IFC model units. A visible
  // length of zero denotes a point glyph.
  std::vector<double> pattern;
  bool widthFromLayer = false;
  bool patternFromLayer = false;
  bool colorFromLayer = false;
};

struct IfcCurveStrokePaths {
  std::vector<glm::vec3> points;
  std::vector<size_t> counts;
  std::vector<glm::vec3> dots;
};

// Mark omitted or explicit 'by layer' fields for the caller to inherit.
// Empty explicit fields keep the importing application's stroke defaults.
[[nodiscard]] IfcCurveStroke
readIfcCurveStroke(const std::unordered_map<uint32_t, Entity> &entities,
                   const Entity &curveStyle);

// Cuts the sampled curve by model-space distance. Phase is continuous across
// source vertices and restarts for each distinct path. Invalid or overly
// dense patterns return no replacement so the caller can keep the solid path.
[[nodiscard]] std::optional<IfcCurveStrokePaths>
splitIfcCurveStroke(std::span<const glm::vec3> points,
                    std::span<const size_t> counts,
                    std::span<const double> pattern, std::stop_token stop = {});

} // namespace container::geometry::ifc::detail
