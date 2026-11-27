#pragma once

#include <array>
#include <cstdint>

namespace container::robotics {

struct SensorAppearanceSettings {
  uint64_t seed{};
  double lightingVariation{};
  double materialVariation{};
};

struct SensorLightingGains {
  float directional{1.0f};
  float environment{1.0f};
};

void validateSensorAppearanceSettings(const SensorAppearanceSettings& settings);
[[nodiscard]] SensorLightingGains sampleSensorLighting(const SensorAppearanceSettings& settings) noexcept;

// Keep the tint fixed over the recording and independent of traversal order.
// A source material index and a packed fallback color occupy distinct domains.
[[nodiscard]] std::array<float, 3> sampleSensorMaterialTint(
    const SensorAppearanceSettings& settings, uint32_t sourceKey, bool authoredMaterial) noexcept;

} // namespace container::robotics
