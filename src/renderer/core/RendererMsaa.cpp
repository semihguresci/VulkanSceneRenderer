#include "Container/renderer/core/RendererMsaa.h"

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
    VkSampleCountFlags colorSampleCounts, VkSampleCountFlags depthSampleCounts) {
  const VkSampleCountFlags sharedSampleCounts =
      colorSampleCounts & depthSampleCounts;
  std::vector<VkSampleCountFlagBits> result;
  for (VkSampleCountFlagBits sampleCount : kOrderedSampleCounts) {
    if ((sharedSampleCounts & sampleCount) != 0u) {
      result.push_back(sampleCount);
    }
  }
  if (result.empty()) {
    result.push_back(VK_SAMPLE_COUNT_1_BIT);
  }
  return result;
}

VkSampleCountFlagBits clampMsaaSampleCount(
    uint32_t requestedSamples, VkSampleCountFlags colorSampleCounts,
    VkSampleCountFlags depthSampleCounts) {
  const VkSampleCountFlagBits requested =
      sampleCountFromSamples(requestedSamples);
  VkSampleCountFlagBits result = VK_SAMPLE_COUNT_1_BIT;
  for (VkSampleCountFlagBits sampleCount :
       supportedMsaaSampleCounts(colorSampleCounts, depthSampleCounts)) {
    if (sampleCountToSamples(sampleCount) > sampleCountToSamples(requested)) {
      break;
    }
    result = sampleCount;
  }
  return result;
}

RendererMsaaDeviceSupport queryRendererMsaaDeviceSupport(
    VkPhysicalDevice physicalDevice) {
  RendererMsaaDeviceSupport support{};
  if (physicalDevice == VK_NULL_HANDLE) {
    return support;
  }

  VkPhysicalDeviceDepthStencilResolveProperties depthResolveProperties{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES};
  VkPhysicalDeviceProperties2 properties2{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  properties2.pNext = &depthResolveProperties;
  vkGetPhysicalDeviceProperties2(physicalDevice, &properties2);

  support.color =
      properties2.properties.limits.framebufferColorSampleCounts;
  support.depth =
      properties2.properties.limits.framebufferDepthSampleCounts;
  support.depthResolveModes = depthResolveProperties.supportedDepthResolveModes;
  support.standardSampleLocations =
      properties2.properties.limits.standardSampleLocations == VK_TRUE;
  if ((support.color & VK_SAMPLE_COUNT_1_BIT) == 0u) {
    support.color |= VK_SAMPLE_COUNT_1_BIT;
  }
  if ((support.depth & VK_SAMPLE_COUNT_1_BIT) == 0u) {
    support.depth |= VK_SAMPLE_COUNT_1_BIT;
  }
  if (support.depthResolveModes == 0u) {
    support.depthResolveModes = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
  }
  return support;
}

VkResolveModeFlagBits preferredDepthResolveMode(
    VkResolveModeFlags supportedModes) {
  if ((supportedModes & VK_RESOLVE_MODE_MAX_BIT) != 0u) {
    return VK_RESOLVE_MODE_MAX_BIT;
  }
  if ((supportedModes & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT) != 0u) {
    return VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
  }
  return VK_RESOLVE_MODE_NONE;
}

} // namespace container::renderer
