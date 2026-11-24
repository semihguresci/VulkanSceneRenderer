#pragma once

#include "Container/common/CommonVulkan.h"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

namespace container::gpu { struct CameraData; }

namespace container::renderer {

enum class VolumetricFogMode : uint32_t { Off, Low, High, Transmittance, Scattering, Froxels };

[[nodiscard]] constexpr std::optional<VolumetricFogMode> parseVolumetricFogMode(std::string_view name) {
  using Mode = VolumetricFogMode;
  if (name == "off") return Mode::Off;
  if (name == "low") return Mode::Low;
  if (name == "high") return Mode::High;
  if (name == "transmittance") return Mode::Transmittance;
  if (name == "scattering") return Mode::Scattering;
  if (name == "froxels") return Mode::Froxels;
  return std::nullopt;
}

struct VolumetricFogSettings {
  static constexpr float kMaxDensity = 4.0f;
  static constexpr float kMinAnisotropy = -0.9f;
  static constexpr float kMaxAnisotropy = 0.9f;
  static constexpr float kMinDistance = 0.5f;
  static constexpr float kMaxDistance = 5000.0f;

  VolumetricFogMode mode{VolumetricFogMode::Off};
  float density{0.035f};
  float albedo{0.9f};
  float anisotropy{0.2f};
  float distance{60.0f};

  bool operator==(const VolumetricFogSettings&) const = default;

  [[nodiscard]] bool valid() const {
    return mode <= VolumetricFogMode::Froxels && std::isfinite(density) && density >= 0.0f && density <= kMaxDensity &&
        std::isfinite(albedo) && albedo >= 0.0f && albedo <= 1.0f && std::isfinite(anisotropy) &&
        anisotropy >= kMinAnisotropy && anisotropy <= kMaxAnisotropy &&
        std::isfinite(distance) && distance >= kMinDistance && distance <= kMaxDistance;
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
