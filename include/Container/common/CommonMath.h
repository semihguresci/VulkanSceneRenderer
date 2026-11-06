#pragma once

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <limits>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

// Right-handed coordinate system throughout (glTF native).
// Projection helpers target Vulkan's clip/depth conventions:
//   - clip/NDC depth range [0, 1]
//   - reverse-Z mapping (near -> 1, far -> 0)
//   - no Y flip in the matrix; scene passes flip Y with a negative viewport

namespace container::math {

// Bound the spectral norm by sqrt(max absolute row sum of A^T*A). Unlike
// maximum column length, this remains conservative under parent/child shear.
[[nodiscard]] inline float conservativeSphereScale(const glm::mat4& transform) {
  const glm::dmat3 basis(transform);
  const auto gram=glm::transpose(basis)*basis;
  double bound=0;
  for(int row=0;row<3;++row) {
    double sum=0;
    for(int column=0;column<3;++column) sum+=std::abs(gram[column][row]);
    if(!std::isfinite(sum)) return std::numeric_limits<float>::infinity();
    bound=std::max(bound,sum);
  }
  if(bound==0) return 0;
  return std::nextafter(static_cast<float>(std::sqrt(bound)),std::numeric_limits<float>::infinity());
}

// Standard RH view matrix (camera looks down -Z).
[[nodiscard]] inline glm::mat4 lookAt(const glm::vec3& eye,
                                      const glm::vec3& center,
                                      const glm::vec3& up) {
  return glm::lookAt(eye, center, up);
}

// RH reverse-Z perspective for Vulkan zero-to-one depth.
[[nodiscard]] inline glm::mat4 perspectiveRH_ReverseZ(float fovRadians,
                                                      float aspectRatio,
                                                      float nearPlane,
                                                      float farPlane) {
  const float tanHalfFov = std::tan(fovRadians * 0.5f);

  glm::mat4 result(0.0f);
  result[0][0] = 1.0f / (aspectRatio * tanHalfFov);
  result[1][1] = 1.0f / tanHalfFov;
  result[2][2] = nearPlane / (farPlane - nearPlane);
  result[2][3] = -1.0f;
  result[3][2] = (farPlane * nearPlane) / (farPlane - nearPlane);
  return result;
}

// RH reverse-Z orthographic projection for Vulkan zero-to-one depth.
[[nodiscard]] inline glm::mat4 orthoRH_ReverseZ(float left, float right,
                                                float bottom, float top,
                                                float nearPlane,
                                                float farPlane) {
  glm::mat4 result(1.0f);
  result[0][0] = 2.0f / (right - left);
  result[1][1] = 2.0f / (top - bottom);
  result[2][2] = 1.0f / (farPlane - nearPlane);
  result[3][0] = -(right + left) / (right - left);
  result[3][1] = -(top + bottom) / (top - bottom);
  result[3][2] = farPlane / (farPlane - nearPlane);
  return result;
}

}  // namespace container::math
