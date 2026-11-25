#pragma once

#include <Container/common/CommonMath.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

namespace container::geometry {

struct GeometryLodView {
  glm::mat4 view{1.0f};
  glm::mat4 projection{1.0f};
  glm::uvec2 viewport{0};
};

struct GeometryLodRange {
  uint32_t firstIndex{0};
  uint32_t indexCount{0};
  float maxError{0.0f};
};

// Levels are ordered from fine to coarse, excluding the canonical draw at 0.
[[nodiscard]] inline uint32_t selectGeometryLod(
    std::span<const GeometryLodRange> levels, const GeometryLodView& view,
    glm::vec4 worldSphere, const glm::mat4& model, float pixelLimit,
    uint32_t previous) {
  if (levels.empty() || !std::isfinite(pixelLimit) || pixelLimit <= 0.0f ||
      view.viewport.x == 0 || view.viewport.y == 0 ||
      !std::isfinite(worldSphere.w) || worldSphere.w <= 0.0f)
    return 0;

  const glm::vec4 center = view.view * glm::vec4(glm::vec3(worldSphere), 1.0f);
  const float radius = worldSphere.w;
  const float closestDepth = -center.z - radius;
  const auto& projection = view.projection;
  const float nearPlane = (projection[3][2] - projection[3][3]) /
                          (projection[2][2] - projection[2][3]);
  if (!std::isfinite(closestDepth) || !std::isfinite(nearPlane) ||
      closestDepth <= std::max(nearPlane, 0.0f))
    return 0;

  const float focalPixels = 0.5f * std::max(
      std::abs(projection[0][0]) * static_cast<float>(view.viewport.x),
      std::abs(projection[1][1]) * static_cast<float>(view.viewport.y));
  float pixelsPerUnit = focalPixels;
  if (projection[2][3] != 0.0f) {
    // Bound the perspective derivative over the sphere, including the
    // screen displacement of a depth change away from the optical axis.
    const float offAxis = (glm::length(glm::vec2(center)) + radius) / closestDepth;
    pixelsPerUnit *= std::hypot(1.0f, offAxis) / closestDepth;
  }
  pixelsPerUnit *= math::conservativeSphereScale(model);
  if (!std::isfinite(pixelsPerUnit) || pixelsPerUnit <= 0.0f)
    return 0;

  uint32_t chosen = 0;
  for (const auto& level : levels) {
    // A coarser level needs additional headroom. Once reached, it stays
    // active until the configured error limit is exceeded on approach.
    const float limit = chosen < previous ? pixelLimit : pixelLimit * 0.8f;
    const float error = level.maxError * pixelsPerUnit;
    if (!std::isfinite(error) || error < 0.0f || error > limit)
      break;
    ++chosen;
  }
  return chosen;
}

} // namespace container::geometry
