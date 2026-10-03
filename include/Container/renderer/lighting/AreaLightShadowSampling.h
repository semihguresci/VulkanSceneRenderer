#pragma once

#include "Container/utility/SceneData.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace container::renderer {

inline constexpr uint32_t kMaxAreaShadowSamples = 4u;

inline std::array<glm::vec3, 6> localShadowCubeDirections() {
  return {{{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0},
           {0, 0, 1}, {0, 0, -1}}};
}

inline std::array<glm::vec3, 6> localShadowCubeUps() {
  return {{{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
           {0, -1, 0}, {0, -1, 0}}};
}

// A complete cube per emitter sample avoids missing grazing/off-axis rays.
inline uint32_t areaShadowSampleCount(uint32_t remainingLayers,
                                      uint32_t remainingLights = 1u) {
  const uint32_t availableCubes = remainingLayers / container::gpu::kLocalShadowPointFaceCount;
  if (availableCubes == 0u) return 0u;
  return std::clamp(availableCubes / std::max(remainingLights, 1u), 1u,
                    kMaxAreaShadowSamples);
}

// Only the CPU chooses sample positions; shaders read their uploaded origins.
inline glm::vec2 areaShadowSampleOffset(uint32_t sample, uint32_t count,
                                       bool disk) {
  if (count <= 1u) return glm::vec2(0.0f);
  if (count == 2u) {
    const float x = sample == 0u ? -0.5f : 0.5f;
    return {x, x};
  }
  if (count == 3u) {
    const float angle = 6.28318530718f * static_cast<float>(sample) / 3.0f;
    return glm::vec2(std::cos(angle), std::sin(angle)) * 0.70710678118f;
  }
  const float radius = disk ? 0.5f : 0.57735026919f;
  return glm::vec2((sample & 1u) ? radius : -radius,
                   (sample & 2u) ? radius : -radius);
}

inline glm::vec3 areaShadowSamplePosition(const container::gpu::AreaLightData& light,
                                         uint32_t sample, uint32_t count) {
  const auto normalize = [](glm::vec3 value, glm::vec3 fallback) {
    const float lengthSq = glm::dot(value, value);
    return std::isfinite(lengthSq) && lengthSq >= 1e-8f
               ? value / std::sqrt(lengthSq) : fallback;
  };
  const glm::vec3 normal = normalize(glm::vec3(light.directionType), {0, 0, -1});
  const glm::vec3 up = std::abs(normal.y) < 0.999f ? glm::vec3(0, 1, 0)
                                                 : glm::vec3(1, 0, 0);
  const glm::vec3 tangent = normalize(
      glm::vec3(light.tangentHalfSize) - normal * glm::dot(glm::vec3(light.tangentHalfSize), normal),
      normalize(glm::cross(up, normal), {1, 0, 0}));
  const glm::vec3 hint(light.bitangentHalfSize);
  const glm::vec3 bitangent = normalize(
      hint - normal * glm::dot(hint, normal) - tangent * glm::dot(hint, tangent),
      normalize(glm::cross(normal, tangent), {0, 1, 0}));
  const bool disk = std::abs(light.directionType.w - container::gpu::kAreaLightTypeDisk) < 0.5f;
  const glm::vec2 size(std::max(light.tangentHalfSize.w, 1e-3f),
                       std::max(disk ? light.tangentHalfSize.w : light.bitangentHalfSize.w, 1e-3f));
  const glm::vec2 offset = areaShadowSampleOffset(sample, count, disk) * size;
  return glm::vec3(light.positionRange) + tangent * offset.x + bitangent * offset.y;
}

} // namespace container::renderer
