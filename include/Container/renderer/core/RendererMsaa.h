#pragma once

#include "Container/common/CommonVulkan.h"

#include <cstdint>
#include <vector>

namespace container::renderer {

struct RendererMsaaDeviceSupport {
  VkSampleCountFlags color{VK_SAMPLE_COUNT_1_BIT};
  VkSampleCountFlags depth{VK_SAMPLE_COUNT_1_BIT};
  VkResolveModeFlags depthResolveModes{VK_RESOLVE_MODE_SAMPLE_ZERO_BIT};
  bool standardSampleLocations{false};
};

[[nodiscard]] uint32_t sampleCountToSamples(VkSampleCountFlagBits sampleCount);
[[nodiscard]] VkSampleCountFlagBits sampleCountFromSamples(uint32_t samples);
[[nodiscard]] std::vector<VkSampleCountFlagBits> supportedMsaaSampleCounts(
    VkSampleCountFlags colorSampleCounts, VkSampleCountFlags depthSampleCounts);
[[nodiscard]] VkSampleCountFlagBits clampMsaaSampleCount(
    uint32_t requestedSamples, VkSampleCountFlags colorSampleCounts,
    VkSampleCountFlags depthSampleCounts);
[[nodiscard]] RendererMsaaDeviceSupport queryRendererMsaaDeviceSupport(
    VkPhysicalDevice physicalDevice);
[[nodiscard]] VkResolveModeFlagBits preferredDepthResolveMode(
    VkResolveModeFlags supportedModes);

} // namespace container::renderer
