#include "Container/renderer/deferred/DeferredLightingDescriptorPlanner.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using container::renderer::DeferredLightingDescriptorPlanInputs;
using container::renderer::buildDeferredLightingDescriptorPlan;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

DeferredLightingDescriptorPlanInputs descriptorInputs() {
  return {.lightingDescriptorSets = {fakeHandle<VkDescriptorSet>(0x10),
                                     fakeHandle<VkDescriptorSet>(0x20)},
          .frameLightingDescriptorSet = fakeHandle<VkDescriptorSet>(0x30),
          .tiledDescriptorSet = fakeHandle<VkDescriptorSet>(0x40),
          .sceneDescriptorSet = fakeHandle<VkDescriptorSet>(0x50)};
}

} // namespace

TEST(DeferredLightingDescriptorPlannerTests,
     DirectionalAndPointLightingUseLightingLightSceneOrder) {
  const auto inputs = descriptorInputs();

  const auto plan = buildDeferredLightingDescriptorPlan(inputs);

  EXPECT_EQ(plan.directionalLightingDescriptorSets[0],
            inputs.lightingDescriptorSets[0]);
  EXPECT_EQ(plan.directionalLightingDescriptorSets[1],
            inputs.lightingDescriptorSets[1]);
  EXPECT_EQ(plan.directionalLightingDescriptorSets[2],
            inputs.sceneDescriptorSet);
  EXPECT_EQ(plan.pointLightingDescriptorSets,
            plan.directionalLightingDescriptorSets);
}

TEST(DeferredLightingDescriptorPlannerTests,
     TiledLightingUsesFrameTiledSceneOrder) {
  const auto inputs = descriptorInputs();

  const auto plan = buildDeferredLightingDescriptorPlan(inputs);

  EXPECT_EQ(plan.tiledLightingDescriptorSets[0],
            inputs.frameLightingDescriptorSet);
  EXPECT_EQ(plan.tiledLightingDescriptorSets[1], inputs.tiledDescriptorSet);
  EXPECT_EQ(plan.tiledLightingDescriptorSets[2], inputs.sceneDescriptorSet);
  EXPECT_EQ(plan.tiledLightingDescriptorSets[3],
            inputs.lightingDescriptorSets[1]);
}

TEST(DeferredLightingDescriptorPlannerTests, NullHandlesArePropagated) {
  const DeferredLightingDescriptorPlanInputs inputs{};

  const auto plan = buildDeferredLightingDescriptorPlan(inputs);

  for (VkDescriptorSet descriptorSet :
       plan.directionalLightingDescriptorSets) {
    EXPECT_EQ(descriptorSet, VK_NULL_HANDLE);
  }
  for (VkDescriptorSet descriptorSet : plan.tiledLightingDescriptorSets) {
    EXPECT_EQ(descriptorSet, VK_NULL_HANDLE);
  }
}
