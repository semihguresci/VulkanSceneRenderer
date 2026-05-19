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

}  // namespace container::renderer
