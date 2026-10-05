#pragma once

#include "Container/geometry/ParametricCurve.h"
#include <array>
#include <string>

namespace container::geometry {
// Circular fillets preserve the source polyline's segment-index parameter
// domain. Removed corner intervals map monotonically onto their fillet arcs.
[[nodiscard]] std::optional<ParametricCurve>
makeFilletedPolyline(std::vector<glm::dvec3> points, double radius,
                     std::string &error);
// Pure conversion in double source coordinates. Closed sweeps join the seam
// without caps; hollow sweeps preserve an oppositely oriented inner wall.
[[nodiscard]] std::vector<std::array<glm::dvec3, 3>>
buildSweptDisk(const ParametricCurve &, double first, double last,
               double radius, double innerRadius, double chordError,
               std::string &error);
} // namespace container::geometry
