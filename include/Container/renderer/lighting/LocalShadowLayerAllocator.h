#pragma once

#include "Container/renderer/shadow/ShadowCascadeDepthPlanner.h"
#include "Container/utility/SceneData.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>

namespace container::renderer {

inline constexpr uint32_t kMaxLocalShadowOmniPointBudget =
    container::gpu::kMaxShadowedLocalLightLayers /
    container::gpu::kLocalShadowPointFaceCount;
inline constexpr float kMaxIesRasterShadowDistance = 5000.0f;

struct LocalShadowLayerAllocationSettings {
  uint32_t omniPointBudget{kMaxLocalShadowOmniPointBudget};
  uint32_t layerBudget{container::gpu::kMaxShadowedLocalLightLayers};
};

struct LocalShadowLayerAllocationResult {
  uint32_t assignedOmniPointCount{0};
  uint32_t assignedSpotCount{0};
  uint32_t usedLayerCount{0};
};

[[nodiscard]] inline bool hasFiniteLocalShadowRange(float range) {
  return std::isfinite(range) && range > 0.0f;
}

[[nodiscard]] inline bool isUnboundedIesShadowLight(
    const container::gpu::PointLightData &light) {
  return light.iesProfileAddress != 0 && light.positionRadius.w == 0.0f &&
         std::isfinite(light.positionRadius.x) &&
         std::isfinite(light.positionRadius.y) &&
         std::isfinite(light.positionRadius.z);
}

[[nodiscard]] inline float localPointShadowFilterRange(
    const container::gpu::PointLightData &light,
    const ShadowCasterSceneBounds *casterBounds) {
  if (hasFiniteLocalShadowRange(light.positionRadius.w))
    return light.positionRadius.w;
  if (!isUnboundedIesShadowLight(light)) return 0.0f;
  if (!casterBounds) return kMaxIesRasterShadowDistance;

  std::array<double, 3> farthestDelta{};
  for (glm::length_t axis = 0; axis < 3; ++axis) {
    const double position = light.positionRadius[axis];
    const double min = casterBounds->minBounds[axis];
    const double max = casterBounds->maxBounds[axis];
    if (!std::isfinite(position) || !std::isfinite(min) || !std::isfinite(max) ||
        min > max)
      return kMaxIesRasterShadowDistance;
    farthestDelta[axis] = std::max(std::abs(min - position), std::abs(max - position));
  }
  // Scale the raster filter to the caster geometry, which may be much closer
  // to a measured source than the fog receivers behind it.
  const double coverage = 1.1 * std::hypot(farthestDelta[0], farthestDelta[1], farthestDelta[2]);
  return static_cast<float>(std::min(coverage, double(kMaxIesRasterShadowDistance)));
}

[[nodiscard]] inline bool isLocalShadowSpotLight(
    const container::gpu::PointLightData &light) {
  return light.coneOuterCosType.y >= 0.5f;
}

inline void clearLocalShadowLayerMetadata(
    std::span<container::gpu::PointLightData> lights) {
  for (container::gpu::PointLightData &light : lights) {
    light.coneOuterCosType.z = 0.0f;
    light.coneOuterCosType.w = 0.0f;
  }
}

inline LocalShadowLayerAllocationResult assignLocalShadowLayerMetadata(
    std::span<container::gpu::PointLightData> lights,
    LocalShadowLayerAllocationSettings settings) {
  clearLocalShadowLayerMetadata(lights);

  settings.omniPointBudget =
      std::min(settings.omniPointBudget, kMaxLocalShadowOmniPointBudget);
  settings.layerBudget =
      std::min(settings.layerBudget,
               container::gpu::kMaxShadowedLocalLightLayers);
  if (settings.layerBudget == 0u) {
    return {};
  }

  LocalShadowLayerAllocationResult result{};
  // Keep authored finite lights in their original order and give measured
  // lights any remaining capacity. IES attenuation remains unbounded.
  for (const bool unboundedIes : std::array{false, true}) {
    for (container::gpu::PointLightData &light : lights) {
      if (light.colorIntensity.a <= 0.0f ||
          !(unboundedIes ? isUnboundedIesShadowLight(light)
                         : hasFiniteLocalShadowRange(light.positionRadius.w))) {
        continue;
      }

      const bool isSpot = isLocalShadowSpotLight(light);
      if (!isSpot && result.assignedOmniPointCount >= settings.omniPointBudget) {
        continue;
      }

      const uint32_t layerCount =
          isSpot ? container::gpu::kLocalShadowSpotLayerCount
                 : container::gpu::kLocalShadowPointFaceCount;
      if (result.usedLayerCount + layerCount > settings.layerBudget) {
        continue;
      }

      // z/w are reserved as local-shadow metadata.
      light.coneOuterCosType.z = static_cast<float>(result.usedLayerCount + 1u);
      light.coneOuterCosType.w = static_cast<float>(layerCount);
      result.usedLayerCount += layerCount;
      if (isSpot) {
        ++result.assignedSpotCount;
      } else {
        ++result.assignedOmniPointCount;
      }
    }
  }

  return result;
}

} // namespace container::renderer
