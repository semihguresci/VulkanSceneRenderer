#include "Container/renderer/deferred/DeferredLightGizmoPlanner.h"
#include "Container/renderer/lighting/LightGizmoIconAtlas.h"

#include <algorithm>
#include <cmath>

namespace container::renderer {

namespace {

constexpr float kLightGizmoCoverageAlpha = 0.36f;
constexpr float kLightGizmoSelectedCoverageAlpha = 0.62f;
constexpr float kLightGizmoLegacyCoverageAlpha = 0.30f;
constexpr float kLightGizmoDirectionalArrowHeadLengthScale = 0.24f;
constexpr float kLightGizmoDirectionalArrowHalfWidthScale = 0.13f;
constexpr float kLightGizmoIconDistanceScale = 0.0075f;
constexpr float kLightGizmoCoverageDisplayScale = 0.5f;

[[nodiscard]] float gizmoExtentForPosition(const glm::vec3 &worldPosition,
                                           const glm::vec3 &cameraPosition,
                                           float radiusBias) {
  const float dist =
      glm::max(glm::length(worldPosition - cameraPosition), 0.1f);
  return std::clamp(dist * kLightGizmoIconDistanceScale + radiusBias,
                    kLightGizmoMinIconExtent, kLightGizmoMaxIconExtent);
}

[[nodiscard]] glm::vec3 normalizedDisplayColor(const glm::vec3 &color,
                                               const glm::vec3 &accent,
                                               float authoredHueWeight) {
  glm::vec3 normalized{
      std::isfinite(color.r) && color.r > 0.0f ? color.r : 0.0f,
      std::isfinite(color.g) && color.g > 0.0f ? color.g : 0.0f,
      std::isfinite(color.b) && color.b > 0.0f ? color.b : 0.0f,
  };
  const float maxChannel = std::max({normalized.r, normalized.g, normalized.b});
  if (maxChannel <= 0.0001f) {
    return accent;
  }
  normalized /= maxChannel;
  const glm::vec3 authoredHue =
      glm::clamp(normalized, glm::vec3(0.28f), glm::vec3(1.0f));
  return glm::clamp(glm::mix(accent, authoredHue, authoredHueWeight),
                    glm::vec3(0.0f), glm::vec3(1.0f));
}

[[nodiscard]] glm::vec3 tintForLightType(EditableLightType type) {
  switch (type) {
  case EditableLightType::Directional:
    return {1.0f, 0.76f, 0.22f};
  case EditableLightType::Spot:
    return {0.68f, 0.56f, 1.0f};
  case EditableLightType::Area:
    return {1.0f, 0.46f, 0.70f};
  case EditableLightType::Point:
    return {0.45f, 0.86f, 1.0f};
  }
  return {0.45f, 0.86f, 1.0f};
}

[[nodiscard]] float authoredHueWeightForLightType(EditableLightType type) {
  switch (type) {
  case EditableLightType::Directional:
    return 0.08f;
  case EditableLightType::Spot:
    return 0.12f;
  case EditableLightType::Area:
    return 0.10f;
  case EditableLightType::Point:
    return 0.22f;
  }
  return 0.22f;
}

[[nodiscard]] glm::vec3 selectedDisplayColor(const glm::vec3 &color) {
  return glm::mix(color, glm::vec3(1.0f), 0.35f);
}

[[nodiscard]] glm::vec3 normalizeOr(const glm::vec3 &value,
                                    const glm::vec3 &fallback) {
  const float len2 = glm::dot(value, value);
  if (!std::isfinite(len2) || len2 <= 1.0e-8f) {
    return fallback;
  }
  return value * (1.0f / std::sqrt(len2));
}

[[nodiscard]] glm::vec3 orthogonalTangentFor(const glm::vec3 &direction,
                                             const glm::vec3 &preferred) {
  const glm::vec3 normal = normalizeOr(direction, glm::vec3(0.0f, -1.0f, 0.0f));
  glm::vec3 tangent = preferred - normal * glm::dot(preferred, normal);
  if (glm::dot(tangent, tangent) <= 1.0e-8f) {
    const glm::vec3 seed = std::abs(normal.y) > 0.9f
                               ? glm::vec3(1.0f, 0.0f, 0.0f)
                               : glm::vec3(0.0f, 1.0f, 0.0f);
    tangent = glm::cross(seed, normal);
  }
  return normalizeOr(tangent, glm::vec3(1.0f, 0.0f, 0.0f));
}

[[nodiscard]] float radiusBiasForLightType(EditableLightType type,
                                           float sceneWorldRadius) {
  switch (type) {
  case EditableLightType::Directional:
    return std::clamp(sceneWorldRadius * 0.004f, 0.0125f, 0.175f);
  case EditableLightType::Area:
    return std::clamp(sceneWorldRadius * 0.0035f, 0.0125f, 0.15f);
  case EditableLightType::Spot:
  case EditableLightType::Point:
    return std::clamp(sceneWorldRadius * 0.003f, 0.01f, 0.125f);
  }
  return 0.01f;
}

[[nodiscard]] float coverageRangeForDisplay(float range,
                                            float sceneWorldRadius) {
  if (!std::isfinite(range) || range <= 0.0f) {
    return 0.0f;
  }
  const float sceneCap = std::clamp(
      sceneWorldRadius * kLightGizmoCoverageSceneRadiusScale, 0.175f, 2.5f);
  return std::min(range, sceneCap);
}

[[nodiscard]] glm::vec2
coverageAreaHalfSizeForDisplay(const glm::vec2 &areaHalfSize,
                               float sceneWorldRadius) {
  const float cap = std::clamp(sceneWorldRadius * 0.11f, 0.125f, 2.0f);
  return {
      std::min(std::max(areaHalfSize.x * kLightGizmoCoverageDisplayScale,
                        0.0f),
               cap),
      std::min(std::max(areaHalfSize.y * kLightGizmoCoverageDisplayScale,
                        0.0f),
               cap),
  };
}

[[nodiscard]] float
directionalCoverageLengthForDisplay(float sceneWorldRadius) {
  const float safeRadius =
      std::isfinite(sceneWorldRadius) && sceneWorldRadius > 0.0f
          ? sceneWorldRadius
          : 1.0f;
  return std::clamp(safeRadius * kLightGizmoDirectionalCoverageSceneRadiusScale,
                    0.225f, 2.25f);
}

void appendCoverage(DeferredLightGizmoPlan &plan,
                    const EditableLightEntity &light,
                    const glm::vec3 &displayColor, float sceneWorldRadius,
                    bool areaEmitterDebug) {
  if (plan.coveragePushConstantCount >= plan.coveragePushConstants.size()) {
    return;
  }

  const glm::vec3 direction =
      normalizeOr(light.direction, glm::vec3(0.0f, -1.0f, 0.0f));
  const glm::vec3 tangent = orthogonalTangentFor(direction, light.tangent);
  LightPushConstants coverage{};
  coverage.positionRadius =
      glm::vec4(light.position, std::max(light.range, 0.0f));
  coverage.colorIntensity =
      glm::vec4(displayColor, light.selected ? kLightGizmoSelectedCoverageAlpha
                                             : kLightGizmoCoverageAlpha);
  coverage.directionInnerCos = glm::vec4(direction, 0.0f);
  coverage.coneOuterCosType = glm::vec4(tangent, 0.0f);
  coverage.contactVisibilityEnabled = 1u;

  switch (light.type) {
  case EditableLightType::Point:
    coverage.positionRadius.w =
        coverageRangeForDisplay(light.range, sceneWorldRadius);
    if (coverage.positionRadius.w <= 0.0f) {
      return;
    }
    coverage.localShadowEnabled = kLightGizmoCoveragePoint;
    break;
  case EditableLightType::Spot: {
    coverage.positionRadius.w =
        coverageRangeForDisplay(light.range, sceneWorldRadius);
    if (coverage.positionRadius.w <= 0.0f) {
      return;
    }
    const float outerDegrees = std::clamp(
        std::isfinite(light.outerConeDegrees) ? light.outerConeDegrees : 45.0f,
        1.0f, 89.0f);
    coverage.localShadowEnabled = kLightGizmoCoverageSpot;
    coverage.coneOuterCosType.w =
        coverage.positionRadius.w * std::tan(glm::radians(outerDegrees));
    break;
  }
  case EditableLightType::Area: {
    const glm::vec2 areaHalfSize = areaEmitterDebug
        ? glm::max(light.areaHalfSize, glm::vec2(0.001f))
        : coverageAreaHalfSizeForDisplay(light.areaHalfSize, sceneWorldRadius);
    coverage.localShadowEnabled = kLightGizmoCoverageArea;
    coverage.positionRadius.w = 0.0f;
    coverage.directionInnerCos.w = areaHalfSize.x;
    coverage.coneOuterCosType.w = areaHalfSize.y;
    // Coverage uses this otherwise-unused scalar as the authored emitter type.
    coverage.bounceIntensity = light.areaShape;
    if (std::abs(light.areaShape - container::gpu::kAreaLightTypeDisk) < 0.5f)
      coverage.coneOuterCosType.w = areaHalfSize.x;
    if (coverage.directionInnerCos.w <= 0.0f ||
        coverage.coneOuterCosType.w <= 0.0f) {
      return;
    }
    break;
  }
  case EditableLightType::Directional: {
    const float arrowLength =
        directionalCoverageLengthForDisplay(sceneWorldRadius);
    coverage.localShadowEnabled = kLightGizmoCoverageDirectional;
    coverage.positionRadius.w = arrowLength;
    coverage.directionInnerCos.w =
        arrowLength * kLightGizmoDirectionalArrowHalfWidthScale;
    coverage.coneOuterCosType.w =
        arrowLength * kLightGizmoDirectionalArrowHeadLengthScale;
    break;
  }
  }

  plan.coveragePushConstants[plan.coveragePushConstantCount++] = coverage;
}

void appendPointCoverage(DeferredLightGizmoPlan &plan,
                         const glm::vec3 &position,
                         const glm::vec3 &displayColor, float radius,
                         float sceneWorldRadius) {
  if (plan.coveragePushConstantCount >= plan.coveragePushConstants.size() ||
      !std::isfinite(radius) || radius <= 0.0f) {
    return;
  }

  LightPushConstants coverage{};
  coverage.positionRadius =
      glm::vec4(position, coverageRangeForDisplay(radius, sceneWorldRadius));
  if (coverage.positionRadius.w <= 0.0f) {
    return;
  }
  coverage.colorIntensity =
      glm::vec4(displayColor, kLightGizmoLegacyCoverageAlpha);
  coverage.contactVisibilityEnabled = 1u;
  coverage.localShadowEnabled = kLightGizmoCoveragePoint;
  plan.coveragePushConstants[plan.coveragePushConstantCount++] = coverage;
}

void appendGizmo(DeferredLightGizmoPlan &plan, const EditableLightEntity &light,
                 const DeferredLightGizmoPlanInputs &inputs) {
  if (plan.pushConstantCount >= plan.pushConstants.size()) {
    return;
  }

  const glm::vec3 color =
      normalizedDisplayColor(light.color, tintForLightType(light.type),
                             authoredHueWeightForLightType(light.type));
  const glm::vec3 displayColor =
      light.selected ? selectedDisplayColor(color) : color;
  const float extent = gizmoExtentForPosition(
      light.position, inputs.cameraPosition,
      radiusBiasForLightType(light.type, inputs.sceneWorldRadius));
  const float displayExtent =
      std::clamp(light.selected ? extent * 1.35f : extent,
                 kLightGizmoMinIconExtent, kLightGizmoMaxIconExtent);

  LightPushConstants &pushConstants =
      plan.pushConstants[plan.pushConstantCount++];
  pushConstants.positionRadius = glm::vec4(light.position, displayExtent);
  pushConstants.colorIntensity = glm::vec4(displayColor, 1.0f);
  pushConstants.directionInnerCos = glm::vec4(light.direction, 1.0f);
  pushConstants.coneOuterCosType = glm::vec4(
      static_cast<float>(lightGizmoIconLayerForType(light.type)),
      light.type == EditableLightType::Spot ? container::gpu::kLightTypeSpot
                                            : container::gpu::kLightTypePoint,
      0.0f, 0.0f);
  pushConstants.padding2 = light.editable ? encodeEditableLightPickId(light.id)
                                          : container::gpu::kPickIdNone;
  appendCoverage(plan, light, displayColor, inputs.sceneWorldRadius,
                 inputs.areaEmitterDebug);
}

} // namespace

uint32_t encodeEditableLightPickId(EditableLightId id) {
  const uint32_t type = static_cast<uint32_t>(id.type);
  const uint32_t source = static_cast<uint32_t>(id.source);
  if (!isValidEditableLightId(id) ||
      type > static_cast<uint32_t>(EditableLightType::Area) ||
      source > static_cast<uint32_t>(EditableLightSource::Manual) ||
      id.index >= kEditableLightPickIndexMask) {
    return container::gpu::kPickIdNone;
  }

  return container::gpu::kPickIdLightMask |
         (type << kEditableLightPickTypeShift) |
         (source << kEditableLightPickSourceShift) | (id.index + 1u);
}

std::optional<EditableLightId> decodeEditableLightPickId(uint32_t pickId) {
  if ((pickId & container::gpu::kPickIdLightMask) == 0u ||
      (pickId & container::gpu::kPickIdBimMask) != 0u) {
    return std::nullopt;
  }

  const uint32_t payload = pickId & container::gpu::kPickIdObjectMask;
  const uint32_t encodedIndex = payload & kEditableLightPickIndexMask;
  if (encodedIndex == 0u) {
    return std::nullopt;
  }

  const uint32_t typeBits = (payload >> kEditableLightPickTypeShift) & 0x3u;
  const uint32_t sourceBits = (payload >> kEditableLightPickSourceShift) & 0x3u;
  if (typeBits > static_cast<uint32_t>(EditableLightType::Area) ||
      sourceBits > static_cast<uint32_t>(EditableLightSource::Manual)) {
    return std::nullopt;
  }

  EditableLightId id{.type = static_cast<EditableLightType>(typeBits),
                     .source = static_cast<EditableLightSource>(sourceBits),
                     .index = encodedIndex - 1u};
  return isValidEditableLightId(id) ? std::optional<EditableLightId>{id}
                                    : std::nullopt;
}

DeferredLightGizmoPlan
buildDeferredLightGizmoPlan(const DeferredLightGizmoPlanInputs &inputs) {
  DeferredLightGizmoPlan plan{};

  if (!inputs.editableLights.empty()) {
    for (const EditableLightEntity &light : inputs.editableLights) {
      appendGizmo(plan, light, inputs);
    }
    return plan;
  }

  const glm::vec3 directionalColor = normalizedDisplayColor(
      inputs.directionalColor, tintForLightType(EditableLightType::Directional),
      authoredHueWeightForLightType(EditableLightType::Directional));
  const glm::vec3 directionalPosition =
      inputs.sceneCenter -
      inputs.directionalDirection * (inputs.sceneWorldRadius * 1.15f);
  const float directionalExtent = gizmoExtentForPosition(
      directionalPosition, inputs.cameraPosition,
      radiusBiasForLightType(EditableLightType::Directional,
                             inputs.sceneWorldRadius));

  LightPushConstants &directionalGizmo =
      plan.pushConstants[plan.pushConstantCount++];
  directionalGizmo.positionRadius =
      glm::vec4(directionalPosition, directionalExtent);
  directionalGizmo.colorIntensity = glm::vec4(directionalColor, 1.0f);
  directionalGizmo.coneOuterCosType =
      glm::vec4(static_cast<float>(
                    lightGizmoIconLayerForType(EditableLightType::Directional)),
                container::gpu::kLightTypePoint, 0.0f, 0.0f);
  EditableLightEntity legacyDirectional{};
  legacyDirectional.type = EditableLightType::Directional;
  legacyDirectional.position = directionalPosition;
  legacyDirectional.direction = inputs.directionalDirection;
  legacyDirectional.tangent = {1.0f, 0.0f, 0.0f};
  appendCoverage(plan, legacyDirectional, directionalColor,
                 inputs.sceneWorldRadius, inputs.areaEmitterDebug);

  const uint32_t pointCount =
      std::min<uint32_t>(static_cast<uint32_t>(inputs.pointLights.size()),
                         kMaxDeferredLightGizmos);
  for (uint32_t i = 0u; i < pointCount; ++i) {
    const container::gpu::PointLightData &pointLight = inputs.pointLights[i];
    const glm::vec3 pointColor = normalizedDisplayColor(
        glm::vec3(pointLight.colorIntensity),
        tintForLightType(EditableLightType::Point),
        authoredHueWeightForLightType(EditableLightType::Point));
    const glm::vec3 pointPosition = glm::vec3(pointLight.positionRadius);
    const float pointExtent = gizmoExtentForPosition(
        pointPosition, inputs.cameraPosition,
        std::clamp(inputs.sceneWorldRadius * kLightGizmoIconDistanceScale,
                   0.02f, 0.375f));

    LightPushConstants &pointGizmo =
        plan.pushConstants[plan.pushConstantCount++];
    pointGizmo.positionRadius = glm::vec4(pointPosition, pointExtent);
    pointGizmo.colorIntensity = glm::vec4(pointColor, 1.0f);
    pointGizmo.coneOuterCosType =
        glm::vec4(static_cast<float>(
                      lightGizmoIconLayerForType(EditableLightType::Point)),
                  container::gpu::kLightTypePoint, 0.0f, 0.0f);
    appendPointCoverage(plan, pointPosition, pointColor,
                        pointLight.positionRadius.w, inputs.sceneWorldRadius);
  }

  return plan;
}

} // namespace container::renderer
