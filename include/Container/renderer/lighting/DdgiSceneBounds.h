#pragma once

#include <glm/vec3.hpp>
#include <span>

namespace container::renderer {

struct RaySceneProviderSource;

struct DdgiSceneBounds {
  glm::vec3 minimum{};
  glm::vec3 maximum{};
  bool valid = false;
};

// Bounds only referenced triangle vertices, transformed by each current
// ObjectData.model. Callers supply semantic-visible triangle draws before
// screen/draw-budget culling; diagnostic, native point and curve geometry must
// not enter those draw lists. Unreferenced vertices/objects do not contribute.
// This explicit-fit CPU operation scans indices for each instance and does not
// use import-time bounds or potentially nonconservative transformed spheres.
// Draw firstIndex selects absolute provider vertex indices (no base vertex).
// Zero-index/zero-instance draws are ignored; no active geometry returns invalid
// bounds. Invalid referenced ranges, positions, affine/invertible transforms or
// nonfinite world coordinates throw invalid_argument without partial results.
[[nodiscard]] DdgiSceneBounds
computeDdgiSceneBounds(std::span<const RaySceneProviderSource> sources);

} // namespace container::renderer
