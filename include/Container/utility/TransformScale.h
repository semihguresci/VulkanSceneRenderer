#pragma once

#include <algorithm>
#include <cmath>
#include <glm/vec3.hpp>

namespace container::ui {

// Scale editing limits magnitudes without changing an existing reflection.
inline float clampSignedScale(float value, float minimum = .001f,
                              float maximum = 1000.0f) {
  return std::copysign(std::clamp(std::abs(value), minimum, maximum), value);
}

inline glm::vec3 clampSignedScale(glm::vec3 value, float minimum = .001f,
                                  float maximum = 1000.0f) {
  for (int axis = 0; axis < 3; ++axis)
    value[axis] = clampSignedScale(value[axis], minimum, maximum);
  return value;
}

// A multiplicative gizmo drag retains the starting sign. Snap magnitudes so
// rounding a shrinking reflected axis through zero cannot flip its winding.
// Unchanged axes keep imported scales outside the interactive editing range.
inline glm::vec3 snapSignedScale(const glm::vec3 &base, glm::vec3 value,
                                 float step, float minimum = .001f,
                                 float maximum = 1000.0f) {
  for (int axis = 0; axis < 3; ++axis) {
    if (value[axis] == base[axis]) continue;
    float magnitude = std::abs(value[axis]);
    if (step > 0.0f)
      magnitude = std::abs(base[axis]) +
                  std::round((magnitude - std::abs(base[axis])) / step) * step;
    value[axis] =
        std::copysign(std::clamp(magnitude, minimum, maximum), base[axis]);
  }
  return value;
}

}  // namespace container::ui
