#pragma once

#include "IfcBrepFaceGeometry.h"

#include <span>
#include <stop_token>

namespace container::geometry::ifc::detail {

[[nodiscard]] std::optional<std::vector<CurvedFaceTriangle>>
meshIfcSweptSurfaces(std::span<const ParametricSurface> charts,
                     bool bareExtrusion, double metersPerUnit,
                     std::stop_token stop, std::string &error);

} // namespace container::geometry::ifc::detail
