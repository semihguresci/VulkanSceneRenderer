#pragma once

namespace container::renderer {

inline constexpr float kMinReferenceFireflyClamp = 1e-6f;
inline constexpr float kMaxReferenceFireflyClamp = 1e20f;

[[nodiscard]] constexpr bool validReferenceFireflyClamp(float value) {
  return value == 0.0f ||
      (value >= kMinReferenceFireflyClamp && value <= kMaxReferenceFireflyClamp);
}

} // namespace container::renderer
