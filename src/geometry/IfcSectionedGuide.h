#pragma once

#include "Container/geometry/ParametricCurve.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

namespace container::geometry::ifc::detail {

struct IfcSectionPlane {
  glm::dvec3 origin, normal;
};

using IfcSectionPlaneAt = std::function<std::optional<IfcSectionPlane>(double)>;

struct IfcSectionedGuide {
  uint32_t sourceId;
  std::vector<double> breaks;
  ParametricCurve::Evaluator point;
};

// Section stations are native directrix parameters including the endpoints and
// all known frame/profile breaks. The returned breaks use that same domain.
// A shared basis uses the original guide parameter directly. Independent guides
// must cross the section planes once, transversely, in consistent order.
// The piecewise-linear mode requires guide segments linear between native knots
// and an affine plane origin and raw normal on each station interval.
[[nodiscard]] IfcSectionedGuide resolveIfcSectionedGuide(
    uint32_t sourceId, ParametricCurve guide, bool sharedBasis,
    std::span<const double> stationBreaks, IfcSectionPlaneAt sectionPlane,
    double pointTolerance, std::stop_token stop = {},
    bool piecewiseLinear = false);

} // namespace container::geometry::ifc::detail
