#pragma once

#include <array>

namespace container::renderer {

struct EnvironmentPresentation {
  float yawDegrees{0.0f};
  float backgroundBlur{0.0f};
  bool solidBackground{false};
  std::array<float, 3> backgroundColor{0.0f, 0.0f, 0.0f};
  bool operator==(const EnvironmentPresentation&) const = default;
};

} // namespace container::renderer
