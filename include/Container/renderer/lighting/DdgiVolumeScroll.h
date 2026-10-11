#pragma once

#include "Container/renderer/lighting/DdgiProbeVolume.h"

#include <glm/vec3.hpp>
#include <glm/ext/vector_int3_sized.hpp>
#include <limits>
#include <vector>

namespace container::renderer {

inline constexpr uint32_t kDdgiNoPreviousProbe = std::numeric_limits<uint32_t>::max();

struct DdgiVolumeScrollPlan {
  DdgiProbeVolumeDesc previous{};
  DdgiProbeVolumeDesc target{};
  // New lattice coordinate c corresponds to old coordinate c + cellShift.
  glm::i64vec3 cellShift{};
  std::vector<uint32_t> previousIndexForNewProbe;
  std::vector<uint32_t> exposedProbeIndices;
  bool changed = false;
  bool clearsHistory = false;
};

// Center-follows the camera by whole cells, retaining its origin within half
// a cell of motion. Half-cell ties round away from zero symmetrically. X-fast
// mappings preserve only nodes whose actual float positions remain identical;
// newly exposed or rounding-shifted nodes use kDdgiNoPreviousProbe and must
// reset both atlas tiles and state. A teleport with no overlap clears history.
// maxProbes bounds CPU allocations/work before any vectors are allocated.
// Nonfinite cameras, unresolvable targets, or shifts outside int64 throw
// invalid_argument without changing the input lattice.
[[nodiscard]] DdgiVolumeScrollPlan
planDdgiVolumeScroll(const DdgiProbeVolumeDesc &previous,
                     glm::vec3 cameraWorldPosition, uint32_t maxProbes = 65536);

} // namespace container::renderer
