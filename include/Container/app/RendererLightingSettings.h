#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>

namespace container::app {

enum class RendererLightingSetting {
  EnvironmentIntensity,
  DirectionalIntensity,
  BounceIntensity,
  EnvironmentYaw,
};

struct RendererLightingSettingMetadata {
  RendererLightingSetting setting;
  std::string_view name;
  // Empty for controls that have no command-line option.
  std::string_view cliOption;
  float minimum, maximum;
};

inline constexpr std::array kRendererLightingSettings{
    RendererLightingSettingMetadata{RendererLightingSetting::EnvironmentIntensity,
        "environmentIntensity", "--environment-intensity", 0.f, 16.f},
    RendererLightingSettingMetadata{RendererLightingSetting::DirectionalIntensity,
        "directionalIntensity", "--directional-intensity", 0.f, 16.f},
    RendererLightingSettingMetadata{RendererLightingSetting::BounceIntensity,
        "bounceIntensity", "--bounce-intensity", 0.f, 2.f},
    RendererLightingSettingMetadata{RendererLightingSetting::EnvironmentYaw,
        "environmentYaw", "--environment-yaw", -180.f, 180.f},
};

[[nodiscard]] constexpr const RendererLightingSettingMetadata& rendererLightingSetting(
    RendererLightingSetting setting) {
  for (const auto& metadata : kRendererLightingSettings)
    if (metadata.setting == setting) return metadata;
  throw std::invalid_argument("Unknown renderer lighting setting");
}

[[nodiscard]] inline bool validRendererLightingValue(
    RendererLightingSetting setting, double value) {
  const auto& metadata = rendererLightingSetting(setting);
  return std::isfinite(value) && value >= metadata.minimum && value <= metadata.maximum;
}

// Programmatic publication retains a defensive clamp. Explicit CLI/remote
// requests validate before mutation instead of silently changing user input.
[[nodiscard]] inline float clampRendererLightingValue(
    RendererLightingSetting setting, float value) {
  const auto& metadata = rendererLightingSetting(setting);
  return std::isfinite(value) ? std::clamp(value, metadata.minimum, metadata.maximum) : 0.f;
}

[[nodiscard]] inline float parseRendererLightingValue(
    RendererLightingSetting setting, std::string_view text) {
  const auto& metadata = rendererLightingSetting(setting);
  // Validate the authored double before narrowing. Otherwise an out-of-range
  // CLI value such as 16.0000001 could round into the accepted float endpoint.
  try {
    size_t consumed{};
    const double value = std::stod(std::string(text), &consumed);
    if (consumed == text.size() && validRendererLightingValue(setting, value))
      return static_cast<float>(value);
  } catch (const std::exception&) {
  }
  throw std::invalid_argument(std::string(metadata.cliOption.empty() ? metadata.name : metadata.cliOption) +
      " must be a finite number in its supported range");
}

} // namespace container::app
