#pragma once

#include <glm/mat4x4.hpp>

#include <cstdint>

namespace container::gpu {

// CPU publication of instance transforms; material and temporal identities stay
// owned by the scene provider. No graphics API or physics backend is required.
struct ObjectTransformUpdate {
  uint32_t objectIndex{0};
  glm::mat4 model{1.0f};
};

} // namespace container::gpu
