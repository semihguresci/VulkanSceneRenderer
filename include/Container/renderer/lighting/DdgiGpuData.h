#pragma once
#include "Container/renderer/lighting/DdgiSettings.h"
#include <array>
#include <glm/vec4.hpp>

namespace container::renderer {
// Shared C++/Slang ABI. Volumes retain configuration order; countsOffset.w
// addresses their first packed position/state/atlas tile.
struct alignas(16) DdgiGpuVolume {
  glm::vec4 originBlend{};
  glm::vec4 spacingNormalBias{};
  glm::uvec4 countsOffset{};
  glm::vec4 bias{};
};
struct alignas(16) DdgiGpuData {
  glm::vec4 originNormalBias{}, spacingViewBias{};
  glm::uvec4 counts{}, irradianceLayout{}, visibilityLayout{}, extents{};
  glm::vec4 trace{};
  glm::uvec4 frame{};
  // intensity, visibility bias, volume count (0 legacy single), transport flags
  glm::vec4 intensityBias{};
  std::array<DdgiGpuVolume, kMaxDdgiVolumes> volumes{};
};
static_assert(sizeof(DdgiGpuVolume) == 64);
static_assert(sizeof(DdgiGpuData) == 400);
static_assert(offsetof(DdgiGpuData, volumes) == 144);
} // namespace container::renderer
