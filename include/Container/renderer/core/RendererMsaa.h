#pragma once

#include "Container/common/CommonVulkan.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace container::renderer {

struct GBufferFormats;

struct RendererMsaaDeviceSupport {
  VkSampleCountFlags color{VK_SAMPLE_COUNT_1_BIT};
  VkSampleCountFlags depth{VK_SAMPLE_COUNT_1_BIT};
  VkSampleCountFlags stencil{VK_SAMPLE_COUNT_1_BIT};
  VkSampleCountFlags integerColor{VK_SAMPLE_COUNT_1_BIT};
  VkResolveModeFlags depthResolveModes{};
  VkResolveModeFlags stencilResolveModes{};
  bool independentResolveNone{false};
  bool standardSampleLocations{false};
};

struct RendererDepthStencilResolve {
  VkResolveModeFlagBits depth{VK_RESOLVE_MODE_NONE};
  VkResolveModeFlagBits stencil{VK_RESOLVE_MODE_NONE};
};

struct RendererMsaaAttachment {
  VkFormat format;
  VkImageUsageFlags usage;
};

[[nodiscard]] uint32_t sampleCountToSamples(VkSampleCountFlagBits sampleCount);
[[nodiscard]] VkSampleCountFlagBits sampleCountFromSamples(uint32_t samples);
[[nodiscard]] std::vector<VkSampleCountFlagBits> supportedMsaaSampleCounts(
    VkSampleCountFlags sampleMask);
[[nodiscard]] std::vector<VkSampleCountFlagBits> supportedMsaaSampleCounts(
    VkSampleCountFlags colorSampleCounts, VkSampleCountFlags depthSampleCounts);
[[nodiscard]] VkSampleCountFlagBits clampMsaaSampleCount(
    uint32_t requestedSamples, VkSampleCountFlags sampleMask);
[[nodiscard]] VkSampleCountFlagBits clampMsaaSampleCount(
    uint32_t requestedSamples, VkSampleCountFlags colorSampleCounts,
    VkSampleCountFlags depthSampleCounts);
[[nodiscard]] RendererMsaaDeviceSupport queryRendererMsaaDeviceSupport(
    VkPhysicalDevice physicalDevice);
[[nodiscard]] RendererDepthStencilResolve chooseRendererDepthStencilResolve(
    VkResolveModeFlags depthModes, VkResolveModeFlags stencilModes,
    bool independentResolveNone);
[[nodiscard]] std::array<RendererMsaaAttachment, 10>
rendererMsaaAttachments(const GBufferFormats &formats);
[[nodiscard]] VkSampleCountFlags rendererMsaaSampleMask(
    const RendererMsaaDeviceSupport &support,
    std::span<const VkSampleCountFlags> attachmentMasks);
[[nodiscard]] VkSampleCountFlags queryRendererMsaaSampleMask(
    VkPhysicalDevice physicalDevice, const GBufferFormats &formats,
    const RendererMsaaDeviceSupport &support);

} // namespace container::renderer
