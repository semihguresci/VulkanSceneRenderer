#include "Container/renderer/deferred/DeferredLightingDescriptorPlanner.h"

namespace container::renderer {

DeferredLightingDescriptorPlan buildDeferredLightingDescriptorPlan(
    const DeferredLightingDescriptorPlanInputs &inputs) {
  DeferredLightingDescriptorPlan plan{};
  plan.directionalLightingDescriptorSets = {
      inputs.lightingDescriptorSets[0], inputs.lightingDescriptorSets[1],
      inputs.sceneDescriptorSet};
  plan.pointLightingDescriptorSets = plan.directionalLightingDescriptorSets;
  plan.tiledLightingDescriptorSets = {
      inputs.frameLightingDescriptorSet, inputs.tiledDescriptorSet,
      inputs.sceneDescriptorSet, inputs.lightingDescriptorSets[1]};
  return plan;
}

} // namespace container::renderer
