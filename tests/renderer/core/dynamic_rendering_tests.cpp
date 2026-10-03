#include "Container/common/DynamicRendering.h"
#include <array>
#include <gtest/gtest.h>

TEST(DynamicRendering, PipelineFormatsAndSecondaryInheritanceMatchAttachments) {
  std::array<VkAttachmentDescription, 2> attachments{};
  attachments[0].format = VK_FORMAT_R16G16B16A16_SFLOAT;
  attachments[0].samples = VK_SAMPLE_COUNT_4_BIT;
  attachments[1].format = VK_FORMAT_D32_SFLOAT_S8_UINT;
  attachments[1].samples = VK_SAMPLE_COUNT_4_BIT;
  VkAttachmentReference color{0u, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference depth{1u,
                              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass{};
  subpass.colorAttachmentCount = 1u;
  subpass.pColorAttachments = &color;
  subpass.pDepthStencilAttachment = &depth;
  VkRenderPassCreateInfo description{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  description.attachmentCount = 2u;
  description.pAttachments = attachments.data();
  description.subpassCount = 1u;
  description.pSubpasses = &subpass;
  RenderingPassHandle pass{};
  ASSERT_EQ(createRenderingPass(VK_NULL_HANDLE, &description, nullptr, &pass),
            VK_SUCCESS);
  const auto pipeline = pass->pipelineInfo();
  const auto inheritance = pass->inheritanceInfo();
  ASSERT_EQ(pipeline.colorAttachmentCount, 1u);
  EXPECT_EQ(pipeline.pColorAttachmentFormats[0], VK_FORMAT_R16G16B16A16_SFLOAT);
  EXPECT_EQ(pipeline.depthAttachmentFormat, VK_FORMAT_D32_SFLOAT_S8_UINT);
  EXPECT_EQ(pipeline.stencilAttachmentFormat, VK_FORMAT_D32_SFLOAT_S8_UINT);
  EXPECT_EQ(inheritance.depthAttachmentFormat, pipeline.depthAttachmentFormat);
  EXPECT_EQ(inheritance.stencilAttachmentFormat,
            pipeline.stencilAttachmentFormat);
  EXPECT_EQ(inheritance.rasterizationSamples, VK_SAMPLE_COUNT_4_BIT);
  destroyRenderingPass(VK_NULL_HANDLE, pass, nullptr);
}

TEST(DynamicRendering, ReverseZMsaaDepthResolveRetainsModeAndAttachment) {
  std::array<VkAttachmentDescription2, 2> attachments{};
  for (auto &attachment : attachments) {
    attachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
    attachment.format = VK_FORMAT_D32_SFLOAT;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  }
  attachments[0].samples = VK_SAMPLE_COUNT_4_BIT;
  VkAttachmentReference2 depth{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
  depth.attachment = 0u;
  depth.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  VkAttachmentReference2 resolved = depth;
  resolved.attachment = 1u;
  VkSubpassDescriptionDepthStencilResolve resolve{
      VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE};
  resolve.depthResolveMode = VK_RESOLVE_MODE_MAX_BIT;
  resolve.pDepthStencilResolveAttachment = &resolved;
  VkSubpassDescription2 subpass{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
  subpass.pNext = &resolve;
  subpass.pDepthStencilAttachment = &depth;
  VkRenderPassCreateInfo2 description{
      VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
  description.attachmentCount = 2u;
  description.pAttachments = attachments.data();
  description.subpassCount = 1u;
  description.pSubpasses = &subpass;
  RenderingPassHandle pass{};
  ASSERT_EQ(createRenderingPass2(VK_NULL_HANDLE, &description, nullptr, &pass),
            VK_SUCCESS);
  EXPECT_EQ(pass->depthResolveMode, VK_RESOLVE_MODE_MAX_BIT);
  EXPECT_EQ(pass->depthResolve.attachment, 1u);
  EXPECT_EQ(pass->stencilFormat, VK_FORMAT_UNDEFINED);
  EXPECT_EQ(pass->samples, VK_SAMPLE_COUNT_4_BIT);
  destroyRenderingPass(VK_NULL_HANDLE, pass, nullptr);
}

TEST(DynamicRendering, TargetRejectsMismatchedAttachments) {
  container::gpu::RenderingPass pass;
  pass.attachments.resize(2u);
  VkImageView view{};
  RenderingTargetCreateInfo description{};
  description.renderPass = &pass;
  description.attachmentCount = 1u;
  description.pAttachments = &view;
  RenderingTargetHandle target{};
  EXPECT_EQ(
      createRenderingTarget(VK_NULL_HANDLE, &description, nullptr, &target),
      VK_ERROR_INITIALIZATION_FAILED);
  EXPECT_EQ(target, nullptr);
}
