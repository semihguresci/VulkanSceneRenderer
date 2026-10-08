#pragma once

#include "Container/common/CommonMath.h"

#include <cstddef>
#include <cstdint>

namespace container::renderer {

// Push constants shared by deferred point lighting and light-gizmo rendering.
struct LightPushConstants {
  glm::vec4 positionRadius{0.0f, 0.0f, 0.0f, 1.0f};
  glm::vec4 colorIntensity{1.0f, 1.0f, 1.0f, 1.0f};
  glm::vec4 directionInnerCos{0.0f, 0.0f, -1.0f, 1.0f};
  glm::vec4 coneOuterCosType{0.0f, 0.0f, 0.0f, 0.0f};
  uint32_t contactVisibilityEnabled{0};
  uint32_t localShadowEnabled{0};
  float bounceIntensity{1.0f};
  uint32_t padding2{0}; // point-light SSBO index, or gizmo pick ID
};
static_assert(sizeof(LightPushConstants) == 80,
              "LightPushConstants size mismatch with "
              "shaders/push_constants_common.slang LightPushConstants.");
static_assert(offsetof(LightPushConstants, positionRadius) == 0,
              "LightPushConstants.positionRadius offset");
static_assert(offsetof(LightPushConstants, colorIntensity) == 16,
              "LightPushConstants.colorIntensity offset");
static_assert(offsetof(LightPushConstants, directionInnerCos) == 32,
              "LightPushConstants.directionInnerCos offset");
static_assert(offsetof(LightPushConstants, coneOuterCosType) == 48,
              "LightPushConstants.coneOuterCosType offset");
static_assert(offsetof(LightPushConstants, contactVisibilityEnabled) == 64,
              "LightPushConstants.contactVisibilityEnabled offset");
static_assert(offsetof(LightPushConstants, localShadowEnabled) == 68,
              "LightPushConstants.localShadowEnabled offset");
static_assert(offsetof(LightPushConstants, bounceIntensity) == 72,
              "LightPushConstants.bounceIntensity offset");

} // namespace container::renderer
