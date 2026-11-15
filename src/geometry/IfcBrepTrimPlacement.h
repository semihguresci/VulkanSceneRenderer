#pragma once

#include "IfcBrepFaceGeometry.h"
#include <cstdint>

namespace container::geometry::ifc::detail {

// Loops use their native lift and exclude the duplicate terminal sample. The
// first loop is the outer bound. An authored anchor fixes its entire lift.
// Returns the unique additive chart offset of each loop, or throws if the
// loops cannot be placed unambiguously within the search and geometry budgets.
[[nodiscard]] std::vector<glm::dvec2>
placeIfcBrepPeriodicTrimLoops(std::span<const CurvedFaceChartLoop> loops,
                              std::span<const uint8_t> anchored,
                              glm::dvec2 periods, std::stop_token stop = {});

} // namespace container::geometry::ifc::detail
