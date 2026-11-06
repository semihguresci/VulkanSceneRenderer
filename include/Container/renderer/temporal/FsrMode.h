#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string_view>

namespace container::renderer {

enum class FsrMode : uint8_t { Native, Quality, Balanced, Performance };

constexpr std::string_view fsrModeName(FsrMode mode) noexcept {
  switch (mode) {
    case FsrMode::Quality: return "quality";
    case FsrMode::Balanced: return "balanced";
    case FsrMode::Performance: return "performance";
    default: return "native";
  }
}

constexpr std::optional<FsrMode> parseFsrMode(std::string_view name) noexcept {
  for (auto mode : {FsrMode::Native, FsrMode::Quality, FsrMode::Balanced, FsrMode::Performance})
    if (fsrModeName(mode) == name)
      return mode;
  return std::nullopt;
}

constexpr VkExtent2D fsrRenderExtent(FsrMode mode, VkExtent2D display) noexcept {
  // FSR 3.1 quality preset ratios are 1.5, 1.7, and 2.0. Floor each
  // dimension and keep the presentation aspect in the camera projection.
  uint32_t scale = 10;
  switch (mode) {
    case FsrMode::Quality: scale = 15; break;
    case FsrMode::Balanced: scale = 17; break;
    case FsrMode::Performance: scale = 20; break;
    default: break;
  }
  return {static_cast<uint32_t>(uint64_t{display.width} * 10 / scale),
          static_cast<uint32_t>(uint64_t{display.height} * 10 / scale)};
}

} // namespace container::renderer
