#include "Container/renderer/picking/TransparentPickDepthCopyRecorder.h"
#include "Container/renderer/picking/DepthReadback.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using container::renderer::TransparentPickDepthCopyInputs;
using container::renderer::buildTransparentPickDepthCopyPlan;
using container::renderer::recordTransparentPickDepthCopyCommands;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

TransparentPickDepthCopyInputs readyInputs() {
  return {.sourceDepthStencilImage = fakeHandle<VkImage>(0x1),
          .pickDepthImage = fakeHandle<VkImage>(0x2),
          .extent = {128u, 64u}};
}

} // namespace

TEST(DepthReadbackTests, PackedDepthStencilTransitionsIncludeBothAspects) {
  for (const auto format : {VK_FORMAT_D16_UNORM_S8_UINT,
                            VK_FORMAT_D24_UNORM_S8_UINT,
                            VK_FORMAT_D32_SFLOAT_S8_UINT}) {
    SCOPED_TRACE(format);
    EXPECT_EQ(container::renderer::depthReadbackBarrierAspects(format),
              VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
  }
}

TEST(DepthReadbackTests, PureDepthTransitionsDoNotInventAStencilAspect) {
  for (const auto format : {VK_FORMAT_D16_UNORM,
                            VK_FORMAT_X8_D24_UNORM_PACK32,
                            VK_FORMAT_D32_SFLOAT}) {
    SCOPED_TRACE(format);
    EXPECT_EQ(container::renderer::depthReadbackBarrierAspects(format),
              VK_IMAGE_ASPECT_DEPTH_BIT);
  }
}

TEST(TransparentPickDepthCopyRecorderTests, NullSourceImageReturnsInactive) {
  TransparentPickDepthCopyInputs inputs = readyInputs();
  inputs.sourceDepthStencilImage = VK_NULL_HANDLE;

  EXPECT_FALSE(buildTransparentPickDepthCopyPlan(inputs).active);
}

TEST(TransparentPickDepthCopyRecorderTests, NullPickDepthImageReturnsInactive) {
  TransparentPickDepthCopyInputs inputs = readyInputs();
  inputs.pickDepthImage = VK_NULL_HANDLE;

  EXPECT_FALSE(buildTransparentPickDepthCopyPlan(inputs).active);
}

TEST(TransparentPickDepthCopyRecorderTests, ZeroExtentReturnsInactive) {
  TransparentPickDepthCopyInputs inputs = readyInputs();
  inputs.extent.width = 0u;
  EXPECT_FALSE(buildTransparentPickDepthCopyPlan(inputs).active);

  inputs = readyInputs();
  inputs.extent.height = 0u;
  EXPECT_FALSE(buildTransparentPickDepthCopyPlan(inputs).active);
}

TEST(TransparentPickDepthCopyRecorderTests,
     ActivePlanUsesDepthStencilBarriersAndDepthOnlyCopy) {
  const auto plan = buildTransparentPickDepthCopyPlan(readyInputs());

  ASSERT_TRUE(plan.active);
  EXPECT_EQ(plan.depthStages, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
  EXPECT_EQ(plan.transferStage, VK_PIPELINE_STAGE_TRANSFER_BIT);
  EXPECT_EQ(plan.toTransfer[0].subresourceRange.aspectMask,
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
  EXPECT_EQ(plan.toTransfer[1].subresourceRange.aspectMask,
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
  EXPECT_EQ(plan.toAttachment[0].subresourceRange.aspectMask,
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
  EXPECT_EQ(plan.toAttachment[1].subresourceRange.aspectMask,
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
  EXPECT_EQ(plan.depthCopy.srcSubresource.aspectMask,
            VK_IMAGE_ASPECT_DEPTH_BIT);
  EXPECT_EQ(plan.depthCopy.dstSubresource.aspectMask,
            VK_IMAGE_ASPECT_DEPTH_BIT);
  EXPECT_EQ(plan.depthCopy.extent.width, 128u);
  EXPECT_EQ(plan.depthCopy.extent.height, 64u);
  EXPECT_EQ(plan.depthCopy.extent.depth, 1u);
}

TEST(TransparentPickDepthCopyRecorderTests,
     ActivePlanPreservesLayoutTransitionsAndAccessMasks) {
  const auto plan = buildTransparentPickDepthCopyPlan(readyInputs());

  ASSERT_TRUE(plan.active);
  EXPECT_EQ(plan.toTransfer[0].oldLayout,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
  EXPECT_EQ(plan.toTransfer[0].newLayout,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  EXPECT_EQ(plan.toTransfer[0].srcAccessMask,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
  EXPECT_EQ(plan.toTransfer[0].dstAccessMask, VK_ACCESS_TRANSFER_READ_BIT);
  EXPECT_EQ(plan.toTransfer[1].oldLayout,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
  EXPECT_EQ(plan.toTransfer[1].newLayout,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  EXPECT_EQ(plan.toTransfer[1].dstAccessMask, VK_ACCESS_TRANSFER_WRITE_BIT);
  EXPECT_EQ(plan.toAttachment[0].oldLayout,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  EXPECT_EQ(plan.toAttachment[0].newLayout,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
  EXPECT_EQ(plan.toAttachment[0].srcAccessMask, VK_ACCESS_TRANSFER_READ_BIT);
  EXPECT_EQ(plan.toAttachment[1].oldLayout,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  EXPECT_EQ(plan.toAttachment[1].newLayout,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
  EXPECT_EQ(plan.toAttachment[1].srcAccessMask, VK_ACCESS_TRANSFER_WRITE_BIT);
}

TEST(TransparentPickDepthCopyRecorderTests,
     RecorderRejectsNullCommandBufferOrInactivePlan) {
  const auto activePlan = buildTransparentPickDepthCopyPlan(readyInputs());

  EXPECT_FALSE(
      recordTransparentPickDepthCopyCommands(VK_NULL_HANDLE, activePlan));
  EXPECT_FALSE(recordTransparentPickDepthCopyCommands(
      fakeHandle<VkCommandBuffer>(0x3), {}));
}
