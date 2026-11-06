#pragma once

#include "Container/common/CommonVulkan.h"

#include <cmath>
#include <cstdint>

namespace container::gpu { struct CameraData; }

namespace container::renderer {

enum class VolumetricFogMode : uint32_t { Off, Low, High, Transmittance, Scattering };

struct VolumetricFogSettings {
  VolumetricFogMode mode{VolumetricFogMode::Off};
  float density{0.035f};
  float albedo{0.9f};
  float anisotropy{0.2f};
  float distance{60.0f};

  bool operator==(const VolumetricFogSettings&) const = default;

  [[nodiscard]] bool valid() const {
    return mode <= VolumetricFogMode::Scattering && std::isfinite(density) && density >= 0.0f && density <= 4.0f &&
        std::isfinite(albedo) && albedo >= 0.0f && albedo <= 1.0f && std::isfinite(anisotropy) &&
        anisotropy >= -0.9f && anisotropy <= 0.9f && std::isfinite(distance) && distance >= 0.5f && distance <= 5000.0f;
  }

  [[nodiscard]] bool enabled() const { return valid() && mode != VolumetricFogMode::Off && density > 0.0f; }
};

struct VolumetricFogFrame {
  VolumetricFogSettings settings;
  const gpu::CameraData* cameraData{};
  VkDescriptorSet lightSet{VK_NULL_HANDLE};
  uint64_t historyRevision{};
};

} // namespace container::renderer
