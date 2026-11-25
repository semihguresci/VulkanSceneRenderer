#pragma once

#include <cstdint>
#include <span>
#include <stop_token>
#include <vector>

#include "Container/geometry/Vertex.h"

namespace container::geometry {

enum class TriangleLodWeldPolicy {
  PreserveAttributes,
  IgnoreGeneratedTangents,
};

struct TriangleLod {
  // Identical vertex buffer and index domain to the full-detail draw.
  std::vector<uint32_t> indices;
  // Conservative cumulative vertex displacement in source model units.
  float maxError{};
};

// Generates up to two successively coarser triangle ranges. Call once per
// material/primitive domain. An empty result leaves the original draw intact
// for unsuitable topology, malformed input, or ranges outside the work budget.
// IgnoreGeneratedTangents is for importers that generate unused per-face
// tangents, and may be used only when the draw does not use them for shading.
[[nodiscard]] std::vector<TriangleLod> buildTriangleLods(
    std::span<const Vertex> vertices, std::span<const uint32_t> indices,
    std::stop_token stop = {},
    TriangleLodWeldPolicy weld = TriangleLodWeldPolicy::PreserveAttributes);

} // namespace container::geometry
