#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace container::renderer {

// Scene-linear volume preview controls. No OpenVDB/Vulkan types are
// exposed, so application configuration and UI can share the same state.
struct OpenVdbVolumeSettings {
  bool enabled{true};
  float densityScale{1.0f};
  std::array<float, 3> scatteringColor{1.0f, 1.0f, 1.0f};
  float anisotropy{0.0f};
  std::array<float, 3> emissionColor{1.0f, 1.0f, 1.0f};
  float emissionScale{0.0f};
  float stepScale{1.0f};
  float temperatureScale{1.0f};
  bool receiveShadows{true};
  // Optional local light-path return approximation. Additional orders are
  // isotropic; this does not solve spatial light diffusion between samples.
  float multipleScatteringStrength{0.0f};
  uint32_t multipleScatteringOrders{2}; // Additional orders, in [1,4].

  bool operator==(const OpenVdbVolumeSettings&) const = default;
};

[[nodiscard]] inline OpenVdbVolumeSettings sanitizeOpenVdbVolumeSettings(
    OpenVdbVolumeSettings settings) noexcept {
  const auto bounded = [](float value, float fallback, float minimum, float maximum) {
    return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
  };
  settings.densityScale = bounded(settings.densityScale, 1.0f, 0.0f, 10000.0f);
  settings.anisotropy = bounded(settings.anisotropy, 0.0f, -0.95f, 0.95f);
  settings.emissionScale = bounded(settings.emissionScale, 0.0f, 0.0f, 10000.0f);
  settings.stepScale = bounded(settings.stepScale, 1.0f, 0.25f, 4.0f);
  settings.temperatureScale = bounded(settings.temperatureScale, 1.0f, 0.0f, 100000.0f);
  settings.multipleScatteringStrength = bounded(settings.multipleScatteringStrength, 0.0f, 0.0f, 1.0f);
  settings.multipleScatteringOrders = std::clamp(settings.multipleScatteringOrders, 1u, 4u);
  for (int axis = 0; axis < 3; ++axis) {
    settings.scatteringColor[axis] = bounded(settings.scatteringColor[axis], 1.0f, 0.0f, 1.0f);
    settings.emissionColor[axis] = bounded(settings.emissionColor[axis], 1.0f, 0.0f, 10000.0f);
  }
  return settings;
}

} // namespace container::renderer
