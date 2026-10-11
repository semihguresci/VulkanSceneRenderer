#pragma once

#include "Container/renderer/lighting/DdgiSceneBounds.h"
#include "Container/renderer/lighting/DdgiSettings.h"

namespace container::renderer {

struct DdgiDetailVolumePlacementOptions {
  uint32_t maxDetailProbes = 2048;
  // A separate CPU-work bound, in addition to the shared payload ceiling.
  uint32_t maxTotalProbes = 65536;
};

struct DdgiDetailVolumePlacementResult {
  DdgiSettings settings;
  uint32_t volumeIndex = 0;
  uint32_t addedProbeCount = 0;
  uint32_t totalProbeCount = 0;
  glm::vec3 boundsMin{};
  glm::vec3 boundsMax{};
  uint64_t payloadBytes = 0;
};

// Appends a deterministic, isotropic lattice around current transformed
// selection/room bounds. The primary and existing detail lattices retain
// their identity. Receiver bounds are padded by at least one cell and the
// configured blend band, including flat/point selections. All volumes share
// the existing payload and frame budgets. Does not enable DDGI or change its
// quality/bias controls; maxDistance grows when needed to cover the new
// volume's padded diagonal. Invalid/empty bounds, exhausted budgets, or
// unresolved float coordinates throw invalid_argument before changing input.
[[nodiscard]] DdgiDetailVolumePlacementResult
fitDdgiDetailVolume(const DdgiSettings &base, const DdgiSceneBounds &bounds,
                   DdgiDetailVolumePlacementOptions options = {});

} // namespace container::renderer
