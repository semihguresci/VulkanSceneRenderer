#pragma once
#include "Container/renderer/lighting/DdgiGpuData.h"
#include <array>
#include <span>
#include <vector>

namespace container::renderer {
struct DdgiVolumeBlend {
  std::array<float, kMaxDdgiVolumes> weights{};
};
// CPU representation of the exact packed GPU lattices. No synthetic lattice
// is used for allocation planning, so unrelated origins do not affect indices.
class DdgiVolumeSet {
public:
  explicit DdgiVolumeSet(const DdgiSettings &settings);
  [[nodiscard]] uint32_t probeCount() const noexcept { return probeCount_; }
  [[nodiscard]] uint32_t volumeCount() const noexcept {
    return uint32_t(volumes_.size());
  }
  [[nodiscard]] uint32_t probeOffset(uint32_t volume) const;
  [[nodiscard]] const DdgiProbeVolume &volume(uint32_t index) const;
  [[nodiscard]] glm::vec3 probePosition(uint32_t packedIndex) const;
  [[nodiscard]] std::vector<glm::vec3> positions() const;
  [[nodiscard]] std::array<DdgiGpuVolume, kMaxDdgiVolumes>
  gpuVolumes(const DdgiSettings &settings) const;
  // Optional availability models whether each lookup found valid facing
  // support. Occluded black is available and must not restore sky fallback.
  [[nodiscard]] DdgiVolumeBlend
  blend(glm::vec3 world, std::span<const bool> available = {}) const;
private:
  std::vector<DdgiProbeVolume> volumes_;
  std::vector<uint32_t> offsets_, coarseToFine_;
  uint32_t probeCount_ = 0;
  float blendCells_ = 1.f;
};
[[nodiscard]] DdgiResourcePlan planDdgiVolumeResources(
    const DdgiSettings &settings, const DdgiResourceLimits &limits = {});
} // namespace container::renderer
