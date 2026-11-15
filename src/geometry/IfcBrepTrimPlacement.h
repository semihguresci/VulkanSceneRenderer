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

struct PeriodicSplineBandCut {
  size_t firstStart, secondStart;
  glm::dvec2 secondOffset;
};

// The rails omit their duplicate terminal samples and close at their first
// points shifted by opposite windings. The returned second offset places the
// base cut endpoint. The matched cut uses that endpoint plus the winding.
[[nodiscard]] PeriodicSplineBandCut cutIfcBrepPeriodicSplineBand(
    std::span<const glm::dvec2> first, std::span<const glm::dvec2> second,
    glm::dvec2 winding, glm::dvec2 periods, bool sameSense);

} // namespace container::geometry::ifc::detail
