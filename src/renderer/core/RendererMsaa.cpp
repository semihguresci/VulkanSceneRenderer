#include "Container/renderer/core/RendererMsaa.h"
#include "Container/renderer/resources/FrameResourceManager.h"

#include <array>

namespace container::renderer {

namespace {

constexpr std::array<VkSampleCountFlagBits, 7> kOrderedSampleCounts = {
    VK_SAMPLE_COUNT_1_BIT,  VK_SAMPLE_COUNT_2_BIT,  VK_SAMPLE_COUNT_4_BIT,
    VK_SAMPLE_COUNT_8_BIT,  VK_SAMPLE_COUNT_16_BIT, VK_SAMPLE_COUNT_32_BIT,
    VK_SAMPLE_COUNT_64_BIT,
};

} // namespace

uint32_t sampleCountToSamples(VkSampleCountFlagBits sampleCount) {
  switch (sampleCount) {
  case VK_SAMPLE_COUNT_1_BIT:
    return 1u;
  case VK_SAMPLE_COUNT_2_BIT:
    return 2u;
  case VK_SAMPLE_COUNT_4_BIT:
    return 4u;
  case VK_SAMPLE_COUNT_8_BIT:
    return 8u;
  case VK_SAMPLE_COUNT_16_BIT:
    return 16u;
  case VK_SAMPLE_COUNT_32_BIT:
    return 32u;
  case VK_SAMPLE_COUNT_64_BIT:
    return 64u;
  default:
    return 1u;
  }
}

VkSampleCountFlagBits sampleCountFromSamples(uint32_t samples) {
  VkSampleCountFlagBits result = VK_SAMPLE_COUNT_1_BIT;
  for (VkSampleCountFlagBits sampleCount : kOrderedSampleCounts) {
    if (sampleCountToSamples(sampleCount) > samples) {
      break;
    }
    result = sampleCount;
  }
  return result;
}

std::vector<VkSampleCountFlagBits> supportedMsaaSampleCounts(
    VkSampleCountFlags sampleMask) {
  std::vector<VkSampleCountFlagBits> result;
  for (VkSampleCountFlagBits sampleCount : kOrderedSampleCounts) {
    if ((sampleMask & sampleCount) != 0u) {
      result.push_back(sampleCount);
    }
  }
  if (result.empty()) {
    result.push_back(VK_SAMPLE_COUNT_1_BIT);
  }
  return result;
}

std::vector<VkSampleCountFlagBits> supportedMsaaSampleCounts(
    VkSampleCountFlags colorSampleCounts, VkSampleCountFlags depthSampleCounts) {
  return supportedMsaaSampleCounts(colorSampleCounts & depthSampleCounts);
}

VkSampleCountFlagBits clampMsaaSampleCount(
    uint32_t requestedSamples, VkSampleCountFlags sampleMask) {
  VkSampleCountFlagBits result = VK_SAMPLE_COUNT_1_BIT;
  for (VkSampleCountFlagBits sampleCount : kOrderedSampleCounts) {
    if (sampleCountToSamples(sampleCount) > requestedSamples) {
      break;
    }
    if ((sampleMask & sampleCount) != 0u) {
      result = sampleCount;
    }
  }
  return result;
}

VkSampleCountFlagBits clampMsaaSampleCount(
    uint32_t requestedSamples, VkSampleCountFlags colorSampleCounts,
    VkSampleCountFlags depthSampleCounts) {
  return clampMsaaSampleCount(requestedSamples,
                              colorSampleCounts & depthSampleCounts);
}

RendererMsaaDeviceSupport queryRendererMsaaDeviceSupport(
    VkPhysicalDevice physicalDevice) {
  if (physicalDevice == VK_NULL_HANDLE) {
    return {};
  }

  VkPhysicalDeviceVulkan12Properties properties12{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
  VkPhysicalDeviceProperties2 properties2{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  properties2.pNext = &properties12;
  vkGetPhysicalDeviceProperties2(physicalDevice, &properties2);

  const auto &limits = properties2.properties.limits;
  return {.color = limits.framebufferColorSampleCounts,
          .depth = limits.framebufferDepthSampleCounts,
          .stencil = limits.framebufferStencilSampleCounts,
          .integerColor = properties12.framebufferIntegerColorSampleCounts,
          .depthResolveModes = properties12.supportedDepthResolveModes,
          .stencilResolveModes = properties12.supportedStencilResolveModes,
          .independentResolveNone = properties12.independentResolveNone == VK_TRUE,
          .standardSampleLocations = limits.standardSampleLocations == VK_TRUE};
}

RendererDepthStencilResolve chooseRendererDepthStencilResolve(
    VkResolveModeFlags depthModes, VkResolveModeFlags stencilModes,
    bool independentResolveNone) {
  const VkResolveModeFlags legalModes =
      independentResolveNone ? depthModes : depthModes & stencilModes;
  // Reverse-Z prefers the nearest covered sample. If the packed attachment
  // requires equal resolve modes, use the best mode shared by both aspects.
  for (VkResolveModeFlagBits mode :
       {VK_RESOLVE_MODE_MAX_BIT, VK_RESOLVE_MODE_SAMPLE_ZERO_BIT}) {
    if ((legalModes & mode) != 0u) {
      return {.depth = mode,
              .stencil = independentResolveNone ? VK_RESOLVE_MODE_NONE : mode};
    }
  }
  return {};
}

std::array<RendererMsaaAttachment, 10>
rendererMsaaAttachments(const GBufferFormats &formats) {
  // FrameResourceManager allocates both raster targets so switching technique
  // can retain the same attachments and graphics pipelines.
  constexpr auto color = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  return {{{formats.albedo, color},
           {formats.normal, color},
           {formats.material, color},
           {formats.emissive, color},
           {formats.specular, color},
           {formats.pickId, color},
           {formats.materialLayers, color},
           {formats.materialSheen, color},
           {formats.sceneColor, color},
           {formats.depthStencil, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT}}};
}

VkSampleCountFlags rendererMsaaSampleMask(
    const RendererMsaaDeviceSupport &support,
    std::span<const VkSampleCountFlags> attachmentMasks) {
  const auto resolve = chooseRendererDepthStencilResolve(
      support.depthResolveModes, support.stencilResolveModes,
      support.independentResolveNone);
  if (resolve.depth == VK_RESOLVE_MODE_NONE) {
    return VK_SAMPLE_COUNT_1_BIT;
  }

  VkSampleCountFlags samples =
      support.color & support.depth & support.stencil & support.integerColor;
  for (const auto mask : attachmentMasks) {
    samples &= mask;
  }
  // Single-sample targets have their own usage and support checks.
  return samples | VK_SAMPLE_COUNT_1_BIT;
}

VkSampleCountFlags queryRendererMsaaSampleMask(
    VkPhysicalDevice physicalDevice, const GBufferFormats &formats,
    const RendererMsaaDeviceSupport &support) {
  if (physicalDevice == VK_NULL_HANDLE) {
    return VK_SAMPLE_COUNT_1_BIT;
  }
  const auto attachments = rendererMsaaAttachments(formats);
  std::array<VkSampleCountFlags, attachments.size()> masks{};
  for (size_t index = 0; index < attachments.size(); ++index) {
    const auto &[format, usage] = attachments[index];
    VkImageFormatProperties properties{};
    if (vkGetPhysicalDeviceImageFormatProperties(
            physicalDevice, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
            usage, 0, &properties) == VK_SUCCESS) {
      masks[index] = properties.sampleCounts;
    }
  }
  return rendererMsaaSampleMask(support, masks);
}

} // namespace container::renderer
