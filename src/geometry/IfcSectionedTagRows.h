#pragma once

#include "Container/geometry/ParametricCurve.h"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <glm/vec3.hpp>

namespace container::geometry::ifc::detail {

struct IfcSectionTagRow {
  uint32_t profileId;
  std::vector<std::string> tags;
  std::vector<glm::dvec3> points;
};

struct IfcSectionTagLayout {
  std::vector<std::vector<glm::dvec3>> rows;
  // The boolean is true when the tag names one column, even if it is absent
  // from some authored sections.
  std::map<std::string, std::pair<double, bool>> taggedColumns;
  std::vector<std::string> intermittentTags;
  // A missing column follows the stated retained column in that row.
  std::vector<std::vector<std::optional<size_t>>> retainedColumns;
  bool coincidentEdges = false;
};

// Resolve correspondence in the first authored row's orientation. Missing runs
// require an authored coincident tip and a unique retained anchor throughout
// their absence. Returned rows are piecewise linear with common ordinal knots.
[[nodiscard]] IfcSectionTagLayout
alignIfcSectionTagRows(std::span<const IfcSectionTagRow> sections,
                       double sourceTolerance, std::stop_token stop = {});

// Place each strictly increasing source parameter at its uniform ordinal on
// [0, 1]. The analytic source curve is retained between the tagged points.
[[nodiscard]] ParametricCurve
alignIfcIndexedSectionCurve(ParametricCurve source,
                            std::vector<double> parameters,
                            uint32_t profileId);

} // namespace container::geometry::ifc::detail
