#include "Container/renderer/lighting/EditableLight.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace container::renderer {

namespace {

[[nodiscard]] bool isFiniteVec3(const glm::vec3 &value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

[[nodiscard]] float finiteOr(float value, float fallback) {
  return std::isfinite(value) ? value : fallback;
}

[[nodiscard]] float nonNegativeFinite(float value, float fallback = 0.0f) {
  return std::max(finiteOr(value, fallback), 0.0f);
}

[[nodiscard]] glm::vec3 nonNegativeFiniteColor(const glm::vec3 &color) {
  if (!isFiniteVec3(color)) {
    return glm::vec3(1.0f);
  }
  return glm::max(color, glm::vec3(0.0f));
}

[[nodiscard]] glm::vec3 normalizeOr(const glm::vec3 &value,
                                    const glm::vec3 &fallback) {
  const float len2 = glm::dot(value, value);
  if (!std::isfinite(len2) || len2 <= 1.0e-12f) {
    return fallback;
  }
  return value * (1.0f / std::sqrt(len2));
}

[[nodiscard]] float degreesFromCos(float cosValue) {
  const float clamped = std::clamp(finiteOr(cosValue, 1.0f), -1.0f, 1.0f);
  return glm::degrees(std::acos(clamped));
}

[[nodiscard]] float cosFromDegrees(float degrees) {
  const float clamped = std::clamp(finiteOr(degrees, 0.0f), 0.0f, 179.0f);
  return std::cos(glm::radians(clamped));
}

[[nodiscard]] std::string makeLabel(EditableLightSource source,
                                    EditableLightType type, uint32_t index) {
  std::string label = editableLightSourceLabel(source);
  label += " ";
  label += editableLightTypeLabel(type);
  if (type != EditableLightType::Directional) {
    label += " ";
    label += std::to_string(index);
  }
  return label;
}

[[nodiscard]] glm::vec3 fallbackTangentForDirection(const glm::vec3 &direction) {
  const glm::vec3 up =
      std::abs(direction.y) < 0.95f ? glm::vec3(0.0f, 1.0f, 0.0f)
                                    : glm::vec3(1.0f, 0.0f, 0.0f);
  return normalizeOr(glm::cross(up, direction), glm::vec3(1.0f, 0.0f, 0.0f));
}

} // namespace

bool operator==(const EditableLightId &lhs, const EditableLightId &rhs) {
  return lhs.type == rhs.type && lhs.source == rhs.source &&
         lhs.index == rhs.index;
}

bool operator!=(const EditableLightId &lhs, const EditableLightId &rhs) {
  return !(lhs == rhs);
}

bool isValidEditableLightId(const EditableLightId &id) {
  return id.index != kInvalidEditableLightIndex ||
         id.type == EditableLightType::Directional;
}

const char *editableLightTypeLabel(EditableLightType type) {
  switch (type) {
  case EditableLightType::Directional:
    return "Directional";
  case EditableLightType::Point:
    return "Point";
  case EditableLightType::Spot:
    return "Spot";
  case EditableLightType::Area:
    return "Area";
  }
  return "Light";
}

const char *editableLightSourceLabel(EditableLightSource source) {
  switch (source) {
  case EditableLightSource::Generated:
    return "Generated";
  case EditableLightSource::Imported:
    return "Imported";
  case EditableLightSource::Manual:
    return "Manual";
  }
  return "Source";
}

EditableLightEntity editableDirectionalLight(
    EditableLightSource source, const glm::vec3 &displayPosition,
    const container::gpu::LightingData &lightingData, bool selected) {
  EditableLightEntity entity{};
  entity.type = EditableLightType::Directional;
  entity.source = source;
  entity.id = {.type = entity.type, .source = source, .index = 0u};
  entity.label = makeLabel(source, entity.type, 0u);
  entity.position = isFiniteVec3(displayPosition) ? displayPosition
                                                  : glm::vec3(0.0f);
  entity.direction =
      normalizeOr(glm::vec3(lightingData.directionalDirection),
                  glm::vec3(0.0f, -1.0f, 0.0f));
  entity.color =
      nonNegativeFiniteColor(glm::vec3(lightingData.directionalColorIntensity));
  entity.intensity =
      nonNegativeFinite(lightingData.directionalColorIntensity.a, 1.0f);
  entity.range = 0.0f;
  entity.selected = selected;
  return entity;
}

EditableLightEntity editablePointLight(
    EditableLightSource source, uint32_t index,
    const container::gpu::PointLightData &light, bool selected) {
  const bool spot = light.coneOuterCosType.y >= 0.5f;
  EditableLightEntity entity{};
  entity.type = spot ? EditableLightType::Spot : EditableLightType::Point;
  entity.source = source;
  entity.id = {.type = entity.type, .source = source, .index = index};
  entity.label = makeLabel(source, entity.type, index);
  entity.position = isFiniteVec3(glm::vec3(light.positionRadius))
                        ? glm::vec3(light.positionRadius)
                        : glm::vec3(0.0f);
  entity.direction =
      normalizeOr(glm::vec3(light.directionInnerCos),
                  glm::vec3(0.0f, -1.0f, 0.0f));
  entity.color = nonNegativeFiniteColor(glm::vec3(light.colorIntensity));
  entity.intensity = nonNegativeFinite(light.colorIntensity.a, 1.0f);
  entity.range = nonNegativeFinite(light.positionRadius.w, 1.0f);
  entity.iesProfileAddress = light.iesProfileAddress;
  if (entity.iesProfileAddress) {
    entity.label = "IES " + entity.label;
    entity.tangent = glm::vec3(light.iesTangent);
  }
  if (spot) {
    entity.innerConeDegrees = degreesFromCos(light.directionInnerCos.w);
    entity.outerConeDegrees = degreesFromCos(light.coneOuterCosType.x);
  }
  entity.selected = selected;
  return entity;
}

EditableLightEntity editableAreaLight(
    EditableLightSource source, uint32_t index,
    const container::gpu::AreaLightData &light, bool selected) {
  EditableLightEntity entity{};
  entity.type = EditableLightType::Area;
  entity.source = source;
  entity.id = {.type = entity.type, .source = source, .index = index};
  entity.label = makeLabel(source, entity.type, index);
  entity.position = isFiniteVec3(glm::vec3(light.positionRange))
                        ? glm::vec3(light.positionRange)
                        : glm::vec3(0.0f);
  entity.direction =
      normalizeOr(glm::vec3(light.directionType),
                  glm::vec3(0.0f, -1.0f, 0.0f));
  entity.tangent =
      normalizeOr(glm::vec3(light.tangentHalfSize),
                  fallbackTangentForDirection(entity.direction));
  entity.bitangent =
      normalizeOr(glm::vec3(light.bitangentHalfSize),
                  normalizeOr(glm::cross(entity.direction, entity.tangent),
                              glm::vec3(0.0f, 0.0f, 1.0f)));
  entity.color = nonNegativeFiniteColor(glm::vec3(light.colorIntensity));
  entity.intensity = nonNegativeFinite(light.colorIntensity.a, 1.0f);
  entity.range = nonNegativeFinite(light.positionRange.w, 0.0f);
  entity.areaHalfSize = {
      std::max(finiteOr(light.tangentHalfSize.w, 0.5f), 0.001f),
      std::max(finiteOr(light.bitangentHalfSize.w, 0.5f), 0.001f)};
  entity.areaShape = light.directionType.w >=
                             (container::gpu::kAreaLightTypeDisk - 0.5f)
                         ? container::gpu::kAreaLightTypeDisk
                         : container::gpu::kAreaLightTypeRectangle;
  entity.selected = selected;
  return entity;
}

container::gpu::LightingData directionalLightDataFromEditable(
    const EditableLightEntity &entity,
    const container::gpu::LightingData &fallback) {
  container::gpu::LightingData data = fallback;
  data.directionalDirection =
      glm::vec4(normalizeOr(entity.direction,
                            glm::vec3(fallback.directionalDirection)),
                0.0f);
  data.directionalColorIntensity =
      glm::vec4(nonNegativeFiniteColor(entity.color),
                nonNegativeFinite(entity.intensity,
                                  fallback.directionalColorIntensity.a));
  return data;
}

container::gpu::PointLightData
pointLightDataFromEditable(const EditableLightEntity &entity) {
  container::gpu::PointLightData light{};
  light.positionRadius =
      glm::vec4(isFiniteVec3(entity.position) ? entity.position
                                              : glm::vec3(0.0f),
                nonNegativeFinite(entity.range, 1.0f));
  light.colorIntensity =
      glm::vec4(nonNegativeFiniteColor(entity.color),
                nonNegativeFinite(entity.intensity, 1.0f));
  const glm::vec3 direction =
      normalizeOr(entity.direction, glm::vec3(0.0f, -1.0f, 0.0f));

  if (entity.type == EditableLightType::Spot) {
    const float innerDegrees =
        std::clamp(finiteOr(entity.innerConeDegrees, 0.0f), 0.0f, 179.0f);
    const float outerDegrees =
        std::clamp(std::max(finiteOr(entity.outerConeDegrees, innerDegrees),
                            innerDegrees),
                   0.0f, 179.0f);
    light.directionInnerCos =
        glm::vec4(direction, cosFromDegrees(innerDegrees));
    light.coneOuterCosType =
        glm::vec4(cosFromDegrees(outerDegrees), container::gpu::kLightTypeSpot,
                  0.0f, 0.0f);
  } else {
    light.directionInnerCos = glm::vec4(direction, 1.0f);
    light.coneOuterCosType =
        glm::vec4(0.0f, container::gpu::kLightTypePoint, 0.0f, 0.0f);
    if (entity.iesProfileAddress) {
      light.iesTangent = glm::vec4(entity.tangent, 1.0f);
      light.iesProfileAddress = entity.iesProfileAddress;
    }
  }
  return light;
}

container::gpu::AreaLightData
areaLightDataFromEditable(const EditableLightEntity &entity) {
  container::gpu::AreaLightData light{};
  const glm::vec3 direction =
      normalizeOr(entity.direction, glm::vec3(0.0f, -1.0f, 0.0f));
  glm::vec3 tangent =
      normalizeOr(entity.tangent, fallbackTangentForDirection(direction));
  tangent = normalizeOr(tangent - direction * glm::dot(tangent, direction),
                        fallbackTangentForDirection(direction));
  const glm::vec3 bitangent =
      normalizeOr(glm::cross(direction, tangent), glm::vec3(0.0f, 0.0f, 1.0f));
  const glm::vec2 halfSize = glm::max(
      glm::vec2(std::isfinite(entity.areaHalfSize.x) ? entity.areaHalfSize.x
                                                     : 0.5f,
                std::isfinite(entity.areaHalfSize.y) ? entity.areaHalfSize.y
                                                     : 0.5f),
      glm::vec2(0.001f));

  light.positionRange =
      glm::vec4(isFiniteVec3(entity.position) ? entity.position
                                              : glm::vec3(0.0f),
                nonNegativeFinite(entity.range, 0.0f));
  light.colorIntensity =
      glm::vec4(nonNegativeFiniteColor(entity.color),
                nonNegativeFinite(entity.intensity, 1.0f));
  light.directionType =
      glm::vec4(direction,
                entity.areaShape >= (container::gpu::kAreaLightTypeDisk - 0.5f)
                    ? container::gpu::kAreaLightTypeDisk
                    : container::gpu::kAreaLightTypeRectangle);
  light.tangentHalfSize = glm::vec4(tangent, halfSize.x);
  light.bitangentHalfSize = glm::vec4(bitangent, halfSize.y);
  return light;
}

} // namespace container::renderer
