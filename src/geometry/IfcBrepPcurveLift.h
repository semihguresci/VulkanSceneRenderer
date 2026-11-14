#pragma once

#include "IfcCurveReader.h"

namespace container::geometry::ifc::detail {

// Return native UV coordinates for the unchanged shared edge cache, including
// both endpoints. A closed edge may have two period-separated endpoint lifts.
// Throws when the pcurve cannot witness the whole edge in its authored sense.
[[nodiscard]] std::vector<glm::dvec2>
liftIfcBrepPcurve(const ParametricSurface &surface,
                  const ParametricCurve &nativeUv,
                  std::span<const glm::vec3> sharedPoints, bool edgeSameSense,
                  double units, std::stop_token stop = {});

} // namespace container::geometry::ifc::detail
