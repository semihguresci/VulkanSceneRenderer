#include "Container/renderer/deferred/DeferredRasterDepthReadOnlyTransitionRecorder.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using container::renderer::DeferredRasterDepthReadOnlyTransitionInputs;
using container::renderer::buildDeferredRasterDepthReadOnlyTransitionPlan;
using container::renderer::recordDeferredRasterDepthReadOnlyTransitionCommands;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

DeferredRasterDepthReadOnlyTransitionInputs readyInputs() {
  return {.depthStencilImage = fakeHandle<VkImage>(0x1)};
}

} // namespace

TEST(DeferredRasterDepthReadOnlyTransitionRecorderTests,
     NullDepthImageProducesNoWork) {
  auto inputs = readyInputs();
  inputs.depthStencilImage = VK_NULL_HANDLE;

  const auto plan = buildDeferredRasterDepthReadOnlyTransitionPlan(inputs);

  EXPECT_EQ(plan.stepCount, 0u);
}

TEST(DeferredRasterDepthReadOnlyTransitionRecorderTests,
     ValidDepthImageBuildsDepthReadOnlyTransition) {
  const auto inputs = readyInputs();

  const auto plan = buildDeferredRasterDepthReadOnlyTransitionPlan(inputs);

  ASSERT_EQ(plan.stepCount, 1u);
  const auto &step = plan.steps[0];
  EXPECT_EQ(step.srcStageMask,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
  EXPECT_EQ(step.dstStageMask, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
  EXPECT_EQ(step.barrier.srcAccessMask,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
  EXPECT_EQ(step.barrier.dstAccessMask,
            VK_ACCESS_SHADER_READ_BIT |
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
  EXPECT_EQ(step.barrier.oldLayout,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
  EXPECT_EQ(step.barrier.newLayout,
            VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL);
  EXPECT_EQ(step.barrier.image, inputs.depthStencilImage);
  EXPECT_EQ(step.barrier.subresourceRange.aspectMask,
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
  EXPECT_EQ(step.barrier.subresourceRange.levelCount, 1u);
  EXPECT_EQ(step.barrier.subresourceRange.layerCount, 1u);
  EXPECT_EQ(step.barrier.srcQueueFamilyIndex, VK_QUEUE_FAMILY_IGNORED);
  EXPECT_EQ(step.barrier.dstQueueFamilyIndex, VK_QUEUE_FAMILY_IGNORED);
}

TEST(DeferredRasterDepthReadOnlyTransitionRecorderTests,
     RecorderRejectsNullCommandBufferOrEmptyPlan) {
  const auto activePlan =
      buildDeferredRasterDepthReadOnlyTransitionPlan(readyInputs());

  EXPECT_FALSE(recordDeferredRasterDepthReadOnlyTransitionCommands(
      VK_NULL_HANDLE, activePlan));
  EXPECT_FALSE(recordDeferredRasterDepthReadOnlyTransitionCommands(
      fakeHandle<VkCommandBuffer>(0x3), {}));
}
