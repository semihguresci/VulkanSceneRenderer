#pragma once

#include <cstdint>
#include <glm/vec3.hpp>

namespace container::renderer {

enum class ReferenceGroundMode : uint32_t { Off, Surface, ShadowMatte };

struct ReferenceGroundSettings {
  static constexpr float maxHeight = 1e20f;
  ReferenceGroundMode mode{};
  float height{};
  glm::vec3 color{0.65f};
  float metallic{};
  float roughness{1.0f};

  [[nodiscard]] bool valid() const {
    const auto unit = [](float value) { return value >= 0.0f && value <= 1.0f; };
    return mode <= ReferenceGroundMode::ShadowMatte &&
        height >= -maxHeight && height <= maxHeight &&
        unit(color.r) && unit(color.g) && unit(color.b) &&
        unit(metallic) && unit(roughness);
  }
};

[[nodiscard]] constexpr const char* referenceGroundModeName(ReferenceGroundMode mode) {
  switch (mode) {
    case ReferenceGroundMode::Surface: return "surface";
    case ReferenceGroundMode::ShadowMatte: return "shadow-matte";
    default: return "off";
  }
}

} // namespace container::renderer
