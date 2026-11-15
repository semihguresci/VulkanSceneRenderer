#pragma once

#include "Container/geometry/DotBimLoader.h"

#include <span>
#include <stop_token>

namespace container::geometry {

// Appends independent native line lists and a thin triangle proxy used for
// picking/bounds. Counts delimit separate paths; no segment joins two paths.
// Invalid input leaves the model unchanged. Repeated endpoints are skipped.
[[nodiscard]] bool
appendPolylineGeometry(dotbim::Model &model, std::span<const glm::vec3> points,
                       std::span<const size_t> pathVertexCounts,
                       uint32_t meshId, float pickingRadius,
                       std::stop_token stop = {});

// Appends a model-space stroke with butt ends, round joins and point glyphs.
// The triangle range is used for both rendering and picking.
[[nodiscard]] bool appendMetricPolylineGeometry(
    dotbim::Model &model, std::span<const glm::vec3> points,
    std::span<const size_t> pathVertexCounts, std::span<const glm::vec3> dots,
    uint32_t meshId, double width, std::stop_token stop = {});

} // namespace container::geometry
