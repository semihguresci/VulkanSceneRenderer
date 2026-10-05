#pragma once

#include "Container/geometry/DotBimLoader.h"

#include <span>

namespace container::geometry {

// Appends independent native line lists and a thin triangle proxy used for
// picking/bounds. Counts delimit separate paths; no segment joins two paths.
// Invalid input leaves the model unchanged. Repeated endpoints are skipped.
[[nodiscard]] bool
appendPolylineGeometry(dotbim::Model &model, std::span<const glm::vec3> points,
                       std::span<const size_t> pathVertexCounts,
                       uint32_t meshId, float pickingRadius);

} // namespace container::geometry
