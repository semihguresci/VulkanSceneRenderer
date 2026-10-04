#pragma once

#include "Container/common/CommonVMA.h"
#include "Container/common/CommonVulkan.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <cstdint>

namespace container::renderer {

struct AttachmentImage {
  VkImage image{VK_NULL_HANDLE};
  VmaAllocation allocation{nullptr};
  VkImageView view{VK_NULL_HANDLE};
  VkFormat format{VK_FORMAT_UNDEFINED};
  VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};
};

struct FrameResources {
  AttachmentImage albedo{};
  AttachmentImage albedoMsaa{};
  AttachmentImage normal{};
  AttachmentImage normalMsaa{};
  AttachmentImage material{};
  AttachmentImage materialMsaa{};
  AttachmentImage emissive{};
  AttachmentImage emissiveMsaa{};
  AttachmentImage specular{};
  AttachmentImage specularMsaa{};
  AttachmentImage pickId{};
  AttachmentImage pickIdMsaa{};
  AttachmentImage pickDepth{};
  AttachmentImage depthStencil{};
  AttachmentImage depthStencilMsaa{};
  VkImageView     depthSamplingView{VK_NULL_HANDLE};
  AttachmentImage sceneColor{};
  AttachmentImage sceneColorMsaa{};
  AttachmentImage
      temporalMotion{}; // signed UV, expected previous depth, validity
  AttachmentImage temporalIdentity{};
  AttachmentImage temporalReactive{};
  AttachmentImage temporalComposite{};
  AttachmentImage temporalDiagnostics{};
  AttachmentImage oitHeadPointers{};
  container::gpu::AllocatedBuffer oitNodeBuffer{};
  container::gpu::AllocatedBuffer oitCounterBuffer{};
  container::gpu::AllocatedBuffer oitMetadataBuffer{};
  uint32_t oitNodeCapacity{0};
  RenderingTargetHandle depthPrepassFramebuffer{VK_NULL_HANDLE};
  RenderingTargetHandle bimDepthPrepassFramebuffer{VK_NULL_HANDLE};
  RenderingTargetHandle gBufferFramebuffer{VK_NULL_HANDLE};
  RenderingTargetHandle bimGBufferFramebuffer{VK_NULL_HANDLE};
  RenderingTargetHandle transparentPickFramebuffer{VK_NULL_HANDLE};
  RenderingTargetHandle lightingFramebuffer{VK_NULL_HANDLE};
  RenderingTargetHandle forwardLightingFramebuffer{VK_NULL_HANDLE};
  RenderingTargetHandle forwardTransparentFramebuffer{VK_NULL_HANDLE};
  RenderingTargetHandle transformGizmoFramebuffer{VK_NULL_HANDLE};
  VkDescriptorSet lightingDescriptorSet{VK_NULL_HANDLE};
  VkDescriptorSet postProcessDescriptorSet{VK_NULL_HANDLE};
  VkDescriptorSet oitDescriptorSet{VK_NULL_HANDLE};
};

}  // namespace container::renderer
