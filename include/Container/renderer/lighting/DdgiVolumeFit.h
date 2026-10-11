#pragma once

#include "Container/renderer/lighting/DdgiSettings.h"

namespace container::renderer {

struct DdgiVolumeFitOptions {
  uint32_t maxProbes = 2048;
};

struct DdgiVolumeFitResult {
  DdgiSettings settings;
  // Actual inclusive probe-center coverage, including the outside padding.
  glm::vec3 boundsMin{};
  glm::vec3 boundsMax{};
  uint32_t probeCount = 0;
  // Exact CPU resource-plan payload; Vulkan allocation padding is excluded.
  uint64_t payloadBytes = 0;
};

// Fits finite, ordered world-space bounds in the renderer's +Y-up basis.
// Uses isotropic spacing, at least three probes per axis and one outside cell
// on nondegenerate axes. Flat axes are centered with nominal one-cell padding
// (within half a float precision step). A 0.001 world-unit minimum cell (1 mm
// when world units are metres) avoids claiming useful microscopic transport
// for point or very small bounds.
//
// Finds the finest float-resolvable spacing on a conservative precision grid
// that satisfies maxProbes, the payload budget and the trace-distance limit.
// Preserves all settings except volume, trace distance, biases and debug
// radius, which scale with the fitted cell. Device allocation limits remain the
// GPU owner's responsibility. Throws invalid_argument with an actionable reason
// for invalid bounds/settings or a fit outside supported coordinate/distance
// limits. The input settings are never modified, including on failure.
[[nodiscard]] DdgiVolumeFitResult
fitDdgiVolume(const DdgiSettings &current, glm::vec3 minimum, glm::vec3 maximum,
              DdgiVolumeFitOptions options = {});

} // namespace container::renderer
