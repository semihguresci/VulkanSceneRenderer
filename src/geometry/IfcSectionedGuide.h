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

enum class IfcSectionedGuideMode { Regular, AffinePlanes, Polyline };

// Section stations are native directrix parameters including the endpoints and
// all known frame/profile breaks. The returned breaks use that same domain.
// A shared basis uses the original guide parameter directly. Independent guides
// must cross the section planes once, transversely, in consistent order.
// AffinePlanes requires a polyline and affine plane origin and raw normal on
// each station interval. Polyline scans exact guide segments at each plane so
// remote segments may run parallel to it without an ambiguous intersection.
[[nodiscard]] IfcSectionedGuide resolveIfcSectionedGuide(
    uint32_t sourceId, ParametricCurve guide, bool sharedBasis,
    std::span<const double> stationBreaks, IfcSectionPlaneAt sectionPlane,
    double pointTolerance, std::stop_token stop = {},
    IfcSectionedGuideMode mode = IfcSectionedGuideMode::Regular);

} // namespace container::geometry::ifc::detail
