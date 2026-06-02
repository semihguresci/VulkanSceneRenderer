#include "Container/renderer/shadow/ShadowCascadeDepthPlanner.h"

#include <algorithm>
#include <cmath>

namespace container::renderer {

namespace {

[[nodiscard]] bool finiteVec3(const glm::vec3& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

[[nodiscard]] bool validBounds(const glm::vec3& minBounds,
                               const glm::vec3& maxBounds) {
  return finiteVec3(minBounds) && finiteVec3(maxBounds) &&
         minBounds.x <= maxBounds.x && minBounds.y <= maxBounds.y &&
         minBounds.z <= maxBounds.z;
}

[[nodiscard]] float quantizeStableCascadeRadius(float radius) {
  if (!std::isfinite(radius) || radius <= 0.0f) {
    return radius;
  }

  constexpr int kRadiusQuantizationBits = 8;
  constexpr int kMinStepExponent = -18;
  const int radiusExponent = std::ilogb(radius);
  const int stepExponent =
      std::max(radiusExponent - kRadiusQuantizationBits, kMinStepExponent);
  const float radiusStep = std::ldexp(1.0f, stepExponent);
  if (!std::isfinite(radiusStep) || radiusStep <= 0.0f) {
    return radius;
  }

  return std::ceil(radius / radiusStep) * radiusStep;
}

}  // namespace

ShadowCascadeDepthPlan buildShadowCascadeDepthPlan(
    const ShadowCascadeDepthPlanInputs& inputs) {
  ShadowCascadeDepthPlan plan{};
  plan.receiverMinBounds = inputs.receiverMinBounds;
  plan.receiverMaxBounds = inputs.receiverMaxBounds;
  plan.lightDistance = std::max(inputs.lightDistance, 0.0f);

  const float minNearPlane = std::max(inputs.minNearPlane, 0.01f);
  if (!validBounds(inputs.receiverMinBounds, inputs.receiverMaxBounds)) {
    plan.casterMinBounds = inputs.receiverMinBounds;
    plan.casterMaxBounds = inputs.receiverMaxBounds;
    plan.nearPlane = minNearPlane;
    plan.farPlane = minNearPlane + 0.01f;
    plan.depthRange = plan.farPlane - plan.nearPlane;
    return plan;
  }

  const float receiverDepth = std::max(
      inputs.receiverMaxBounds.z - inputs.receiverMinBounds.z, 0.01f);
  const float guard = std::max(
      {std::max(inputs.texelSize, 0.0f) * 2.0f, receiverDepth * 0.005f,
       minNearPlane});

  plan.casterMinBounds = inputs.receiverMinBounds;
  plan.casterMaxBounds = inputs.receiverMaxBounds;
  if (inputs.hasFiniteCasterBounds &&
      validBounds(inputs.casterMinBounds, inputs.casterMaxBounds)) {
    plan.casterMinBounds.z =
        std::min(plan.casterMinBounds.z, inputs.casterMinBounds.z - guard);
    plan.casterMaxBounds.z =
        std::max(plan.casterMaxBounds.z, inputs.casterMaxBounds.z + guard);
  } else {
    const float fallbackDepth = std::max(inputs.fallbackCasterDepth, guard);
    plan.casterMaxBounds.z += fallbackDepth + guard;
  }

  if (plan.casterMaxBounds.z > -minNearPlane) {
    plan.lightDistanceIncrease = plan.casterMaxBounds.z + minNearPlane;
    plan.lightDistance += plan.lightDistanceIncrease;
    plan.receiverMinBounds.z -= plan.lightDistanceIncrease;
    plan.receiverMaxBounds.z -= plan.lightDistanceIncrease;
    plan.casterMinBounds.z -= plan.lightDistanceIncrease;
    plan.casterMaxBounds.z -= plan.lightDistanceIncrease;
  }

  plan.nearPlane = std::max(minNearPlane, -plan.casterMaxBounds.z);
  plan.farPlane = std::max(plan.nearPlane + minNearPlane,
                           -plan.casterMinBounds.z);
  plan.depthRange = plan.farPlane - plan.nearPlane;
  return plan;
}

float expandShadowCascadeRadiusForFilterGuard(float receiverRadius,
                                              uint32_t shadowMapResolution,
                                              float guardTexels) {
  const float radius = std::max(receiverRadius, 0.0f);
  if (!std::isfinite(radius) || !std::isfinite(guardTexels) ||
      shadowMapResolution == 0u || guardTexels <= 0.0f) {
    return radius;
  }

  const float resolution = static_cast<float>(shadowMapResolution);
  const float clampedGuardTexels =
      std::min(std::max(guardTexels, 0.0f), resolution * 0.25f);
  const float scaleDenominator =
      1.0f - (2.0f * clampedGuardTexels / resolution);
  if (scaleDenominator <= 1.0e-4f) {
    return radius;
  }

  return quantizeStableCascadeRadius(radius / scaleDenominator);
}

}  // namespace container::renderer
