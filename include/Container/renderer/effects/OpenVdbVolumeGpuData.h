#pragma once

#include "Container/common/CommonMath.h"

#include <cstdint>

namespace container::renderer {

inline constexpr uint32_t kMaxOpenVdbVolumes = 4;

// Matches MediumData in openvdb_volume.slang. Texture descriptors are statically
// indexed. Native density/temperature grids share one packed buffer per medium.
struct alignas(16) OpenVdbMediumGpuData {
  glm::vec4 minimumDensity{};
  glm::vec4 maximumStep{};
  glm::vec4 scatteringAnisotropy{};
  glm::vec4 emission{};
  glm::vec4 temperatureControls{}; // x: Kelvin multiplier; y: native import scale; z: higher-order strength
  glm::uvec4 flags{}; // x: enabled=1, temperature=2, sparse=4, shadows=16; y: additional scattering orders; z: temp offset
};
static_assert(sizeof(OpenVdbMediumGpuData) == 96);

} // namespace container::renderer
