#pragma once

#include "Container/utility/SceneData.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

namespace container::renderer {

inline constexpr uint32_t kMaxLocalShadowOmniPointBudget =
    container::gpu::kMaxShadowedLocalLightLayers /
    container::gpu::kLocalShadowPointFaceCount;

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
  for (container::gpu::PointLightData &light : lights) {
    if (light.colorIntensity.a <= 0.0f) {
      continue;
    }
    if (!hasFiniteLocalShadowRange(light.positionRadius.w)) {
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

    // z/w are intentionally reserved as local-shadow metadata while keeping
    // PointLightData at 64 bytes for existing clustered-light paths.
    light.coneOuterCosType.z = static_cast<float>(result.usedLayerCount + 1u);
    light.coneOuterCosType.w = static_cast<float>(layerCount);
    result.usedLayerCount += layerCount;
    if (isSpot) {
      ++result.assignedSpotCount;
    } else {
      ++result.assignedOmniPointCount;
    }
  }

  return result;
}

} // namespace container::renderer
