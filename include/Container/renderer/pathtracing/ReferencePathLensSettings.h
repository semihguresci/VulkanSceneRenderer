#pragma once

#include <cmath>

namespace container::renderer {

struct ReferencePathLensSettings {
  static constexpr float maxApertureRadius = 1e10f;
  static constexpr float minFocusDistance = 1e-6f;
  static constexpr float maxFocusDistance = 1e20f;

  float apertureRadius{};
  float focusDistance{10.0f};

  [[nodiscard]] bool valid() const {
    return std::isfinite(apertureRadius) && apertureRadius >= 0.0f && apertureRadius <= maxApertureRadius &&
        std::isfinite(focusDistance) && focusDistance >= minFocusDistance && focusDistance <= maxFocusDistance;
  }
};

} // namespace container::renderer
