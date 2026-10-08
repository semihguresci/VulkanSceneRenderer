#pragma once

#include <algorithm>
#include <cstdint>

namespace container::renderer {
enum class RayShadowMode : uint32_t { Raster = 0, Hard = 1, Soft = 2 };
struct RayShadowSettings {
  RayShadowMode mode{RayShadowMode::Raster};
  uint32_t areaSamples{8};
  uint32_t localLightBudget{8};
  bool denoise{true};
  uint32_t debugLayer{
      0}; // 0 = shaded; 1..9 = visibility; 10 = configured sampling budget
};
inline RayShadowSettings sanitizeRayShadowSettings(RayShadowSettings settings) {
  if (static_cast<uint32_t>(settings.mode) > 2)
    settings.mode = RayShadowMode::Raster;
  settings.areaSamples = std::clamp(settings.areaSamples, 1u, 32u);
  settings.localLightBudget = std::min(settings.localLightBudget, 8u);
  settings.debugLayer = std::min(settings.debugLayer, 10u);
  return settings;
}
} // namespace container::renderer
