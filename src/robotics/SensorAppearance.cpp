#include "Container/robotics/SensorAppearance.h"

#include "SensorNoise.h"

#include <cmath>
#include <stdexcept>

namespace container::robotics {

void validateSensorAppearanceSettings(const SensorAppearanceSettings& settings) {
  const auto probability = [](double value) {
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
  };
  if (!probability(settings.lightingVariation))
    throw std::invalid_argument("appearance.lightingVariation must be finite and in [0,1]");
  if (!probability(settings.materialVariation))
    throw std::invalid_argument("appearance.materialVariation must be finite and in [0,1]");
}

SensorLightingGains sampleSensorLighting(const SensorAppearanceSettings& settings) noexcept {
  const auto key = sensor_noise::mix(settings.seed ^ 0x16a6c95a75a28b11ULL);
  const auto gain = [&](uint64_t channel) {
    return static_cast<float>(1.0 + settings.lightingVariation *
        (2.0 * sensor_noise::uniform(key, channel) - 1.0));
  };
  return {gain(0), gain(1)};
}

std::array<float, 3> sampleSensorMaterialTint(
    const SensorAppearanceSettings& settings, uint32_t sourceKey, bool authoredMaterial) noexcept {
  constexpr uint64_t authoredDomain = 0x83c4742d54d508cbULL;
  constexpr uint64_t fallbackDomain = 0x6c2af2b170ca9c5eULL;
  const auto domain = authoredMaterial ? authoredDomain : fallbackDomain;
  const auto key = sensor_noise::mix(sensor_noise::mix(settings.seed ^ domain) ^ sourceKey);
  const auto gain = [&](uint64_t channel) {
    return static_cast<float>(1.0 - settings.materialVariation * sensor_noise::uniform(key, channel));
  };
  return {gain(0), gain(1), gain(2)};
}

} // namespace container::robotics
