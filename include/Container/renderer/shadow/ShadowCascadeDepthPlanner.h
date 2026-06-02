#pragma once

#include <cstdint>

#include <glm/vec3.hpp>

namespace container::renderer {

struct ShadowCasterSceneBounds {
  glm::vec3 minBounds{0.0f};
  glm::vec3 maxBounds{0.0f};
};

struct ShadowCascadeDepthPlanInputs {
  glm::vec3 receiverMinBounds{0.0f};
  glm::vec3 receiverMaxBounds{0.0f};
  glm::vec3 casterMinBounds{0.0f};
  glm::vec3 casterMaxBounds{0.0f};
  bool hasFiniteCasterBounds{false};
  float texelSize{0.0f};
  float lightDistance{0.0f};
  float fallbackCasterDepth{1.0f};
  float minNearPlane{0.01f};
};

struct ShadowCascadeDepthPlan {
  glm::vec3 receiverMinBounds{0.0f};
  glm::vec3 receiverMaxBounds{0.0f};
  glm::vec3 casterMinBounds{0.0f};
  glm::vec3 casterMaxBounds{0.0f};
  float lightDistance{0.0f};
  float lightDistanceIncrease{0.0f};
  float nearPlane{0.01f};
  float farPlane{0.02f};
  float depthRange{0.01f};
};

[[nodiscard]] ShadowCascadeDepthPlan buildShadowCascadeDepthPlan(
    const ShadowCascadeDepthPlanInputs& inputs);

[[nodiscard]] float expandShadowCascadeRadiusForFilterGuard(
    float receiverRadius,
    uint32_t shadowMapResolution,
    float guardTexels);

}  // namespace container::renderer
