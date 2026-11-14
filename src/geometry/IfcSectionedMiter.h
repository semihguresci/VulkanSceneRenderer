#pragma once

#include "Container/geometry/ParametricCurve.h"

#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

#include <glm/vec2.hpp>

namespace container::geometry::ifc::detail {

struct IfcSectionMiterFrame {
  glm::dvec3 origin, lateral, up, normal;
};

struct IfcSectionMiterCorner {
  double parameter;
  glm::dvec3 incoming, outgoing;
  std::optional<double> incomingProbe, outgoingProbe;
};

using IfcSectionMiterFrameAt =
    std::function<std::optional<IfcSectionMiterFrame>(double)>;
using IfcSectionMiterProfileAt =
    std::function<std::optional<glm::dvec2>(double, double)>;
using IfcSectionMiterEvaluator =
    std::function<std::optional<glm::dvec3>(double, double)>;

struct IfcSectionMiterResult {
  IfcSectionMiterEvaluator point;
  std::vector<double> breaks;
  IfcSectionMiterEvaluator meshNormal;
};

// Section and directrix breaks use the native U parameter. Profile breaks
// use normalized V. Callbacks must own any retained data.
[[nodiscard]] IfcSectionMiterResult buildIfcSectionedMiter(
    const ParametricCurve &directrix, std::span<const double> stationBreaks,
    std::span<const IfcSectionMiterCorner> corners,
    IfcSectionMiterFrameAt sectionFrame, IfcSectionMiterProfileAt profilePoint,
    std::span<const double> authoredStations,
    std::span<const double> profileBreaks, double tolerance, double chordError,
    std::stop_token stop = {}, bool allowDefaultFrameTransport = false);

} // namespace container::geometry::ifc::detail
