#pragma once

#include "Container/common/CommonMath.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace container::renderer {

// Estimates the largest screen footprint of a material use without occlusion
// readback. Conservative bounds keep off-centre and grazing surfaces stable.
class TextureMipDemandView {
 public:
  TextureMipDemandView(const glm::mat4& view, const glm::mat4& projection,
                       glm::uvec2 extent) : view_(view) {
    const auto row = [&projection](int index) {
      return glm::vec4(projection[0][index], projection[1][index],
                       projection[2][index], projection[3][index]);
    };
    const auto x = row(0), y = row(1), z = row(2), w = row(3);
    // Vulkan clip coordinates use 0 <= z <= w, including reverse-Z.
    planes_ = {w + x, w - x, w + y, w - y, z, w - z};
    for (auto& plane : planes_) {
      const float length = glm::length(glm::vec3(plane));
      if (length > 0.0f)
        plane /= length;
    }
    perspective_ = projection[2][3] != 0.0f;
    nearDepth_ = (projection[3][2] - projection[3][3]) /
                 (projection[2][2] - projection[2][3]);
    focalPixels_ = 0.5f * std::max(
        std::abs(projection[0][0]) * static_cast<float>(extent.x),
        std::abs(projection[1][1]) * static_cast<float>(extent.y));
  }

  [[nodiscard]] float projectedPixels(glm::vec4 worldSphere) const {
    const float radius = worldSphere.w;
    const auto center = view_ * glm::vec4(glm::vec3(worldSphere), 1.0f);
    for (const auto& plane : planes_) {
      if (glm::dot(center, plane) < -radius)
        return 0.0f;
    }
    if (!std::isfinite(radius) || radius <= 0.0f)
      return std::numeric_limits<float>::infinity();
    if (!perspective_)
      return 2.0f * radius * focalPixels_;

    const float closest = -center.z - radius;
    if (!std::isfinite(closest) || closest <= std::max(nearDepth_, 0.0f))
      return std::numeric_limits<float>::infinity();
    const float offAxis = (glm::length(glm::vec2(center)) + radius) / closest;
    return 2.0f * radius * focalPixels_ * std::hypot(1.0f, offAxis) / closest;
  }

 private:
  glm::mat4 view_;
  std::array<glm::vec4, 6> planes_{};
  float focalPixels_{};
  float nearDepth_{};
  bool perspective_{};
};

[[nodiscard]] inline uint32_t textureMipForFootprint(
    float pixels, glm::uvec2 extent, glm::vec2 uvRepeat, uint32_t fallbackFirstMip,
    uint32_t previousDemand) {
  if (!(pixels > 0.0f))
    return fallbackFirstMip;
  if (!std::isfinite(pixels))
    return 0;
  // Less than one repeat needs a finer source mip. The shorter texel axis
  // bounds the result so wide textures do not lose all detail along their height.
  const auto texels = glm::vec2(extent) * glm::clamp(uvRepeat, 0.125f, 16.0f);
  const float ratio = std::min(texels.x, texels.y) / std::max(pixels, 1.0f);
  const auto level = [fallbackFirstMip](float texelRatio) {
    if (!(texelRatio > 1.0f))
      return 0u;
    return static_cast<uint32_t>(std::clamp(std::floor(std::log2(texelRatio)),
                                             0.0f, static_cast<float>(fallbackFirstMip)));
  };
  previousDemand = std::min(previousDemand, fallbackFirstMip);
  const uint32_t mip = level(ratio);
  return mip < previousDemand ? mip : std::max(previousDemand, level(ratio * 0.88f));
}

}  // namespace container::renderer
