#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string_view>

namespace container::renderer {

enum class UpscaleMode : uint8_t { Native, Quality, Balanced, Performance };
enum class UpscaleBackend : uint8_t { Fsr, Dlss };

struct UpscaleSelection {
  UpscaleBackend backend{UpscaleBackend::Fsr};
  UpscaleMode mode{UpscaleMode::Native};
  bool operator==(const UpscaleSelection&) const = default;
};

constexpr std::string_view upscaleModeName(UpscaleMode mode) noexcept {
  switch (mode) {
    case UpscaleMode::Quality: return "quality";
    case UpscaleMode::Balanced: return "balanced";
    case UpscaleMode::Performance: return "performance";
    default: return "native";
  }
}

constexpr std::optional<UpscaleMode> parseUpscaleMode(std::string_view name) noexcept {
  for (auto mode : {UpscaleMode::Native, UpscaleMode::Quality, UpscaleMode::Balanced, UpscaleMode::Performance})
    if (upscaleModeName(mode) == name)
      return mode;
  return std::nullopt;
}

constexpr VkExtent2D fsrRenderExtent(UpscaleMode mode, VkExtent2D display) noexcept {
  // FSR 3.1 quality preset ratios are 1.5, 1.7, and 2.0. Floor each
  // dimension and keep the presentation aspect in the camera projection.
  uint32_t scale = 10;
  switch (mode) {
    case UpscaleMode::Quality: scale = 15; break;
    case UpscaleMode::Balanced: scale = 17; break;
    case UpscaleMode::Performance: scale = 20; break;
    default: break;
  }
  return {static_cast<uint32_t>(uint64_t{display.width} * 10 / scale),
          static_cast<uint32_t>(uint64_t{display.height} * 10 / scale)};
}

} // namespace container::renderer
