#pragma once

#include <vector>
#include <vulkan/vulkan.h>

namespace container::gpu {

// CPU descriptions, not Vulkan render-pass or framebuffer objects.
struct RenderingPass {
  std::vector<VkAttachmentDescription> attachments;
  std::vector<VkAttachmentReference> colors;
  std::vector<VkAttachmentReference> resolves;
  VkAttachmentReference depth{VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED};
  VkAttachmentReference depthResolve{VK_ATTACHMENT_UNUSED,
                                     VK_IMAGE_LAYOUT_UNDEFINED};
  VkResolveModeFlagBits depthResolveMode{VK_RESOLVE_MODE_NONE};
  VkResolveModeFlagBits stencilResolveMode{VK_RESOLVE_MODE_NONE};
  std::vector<VkFormat> colorFormats;
  VkFormat depthFormat{VK_FORMAT_UNDEFINED};
  VkFormat stencilFormat{VK_FORMAT_UNDEFINED};
  VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};
  uint32_t viewMask{0};

  [[nodiscard]] VkPipelineRenderingCreateInfo pipelineInfo() const;
  [[nodiscard]] VkCommandBufferInheritanceRenderingInfo inheritanceInfo() const;
};

struct RenderingTarget {
  std::vector<VkImageView> attachments;
  uint32_t width{}, height{}, layers{1};
};
} // namespace container::gpu

using RenderingPassHandle = container::gpu::RenderingPass *;
using RenderingTargetHandle = container::gpu::RenderingTarget *;

struct RenderingTargetCreateInfo {
  VkStructureType sType{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  const void *pNext{};
  VkFramebufferCreateFlags flags{};
  RenderingPassHandle renderPass{};
  uint32_t attachmentCount{};
  const VkImageView *pAttachments{};
  uint32_t width{}, height{}, layers{1};
};

struct RenderingBeginInfo {
  VkStructureType sType{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  const void *pNext{};
  RenderingPassHandle renderPass{};
  RenderingTargetHandle framebuffer{};
  VkRect2D renderArea{};
  uint32_t clearValueCount{};
  const VkClearValue *pClearValues{};
};

// The base structure remains an ordinary Vulkan pipeline description. The CPU
// pass supplies attachment formats through VkPipelineRenderingCreateInfo.
struct RenderingGraphicsPipelineCreateInfo : VkGraphicsPipelineCreateInfo {
  RenderingPassHandle renderPass{};
};

VkResult createRenderingPass(VkDevice, const VkRenderPassCreateInfo *,
                             const VkAllocationCallbacks *,
                             RenderingPassHandle *);
VkResult createRenderingPass2(VkDevice, const VkRenderPassCreateInfo2 *,
                              const VkAllocationCallbacks *,
                              RenderingPassHandle *);
void destroyRenderingPass(VkDevice, RenderingPassHandle,
                          const VkAllocationCallbacks *);
VkResult createRenderingTarget(VkDevice, const RenderingTargetCreateInfo *,
                               const VkAllocationCallbacks *,
                               RenderingTargetHandle *);
void destroyRenderingTarget(VkDevice, RenderingTargetHandle,
                            const VkAllocationCallbacks *);
void beginDynamicRendering(VkCommandBuffer, const RenderingBeginInfo *,
                           VkSubpassContents);
void endDynamicRendering(VkCommandBuffer);

VkResult createVulkanImageView(VkDevice, const VkImageViewCreateInfo *,
                               const VkAllocationCallbacks *, VkImageView *);
void destroyVulkanImageView(VkDevice, VkImageView,
                            const VkAllocationCallbacks *);
