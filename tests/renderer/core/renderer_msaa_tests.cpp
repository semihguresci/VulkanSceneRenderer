#include "Container/renderer/core/RendererMsaa.h"
#include "Container/renderer/resources/FrameResourceManager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <vector>

namespace {

using container::renderer::GBufferFormats;
using container::renderer::RendererMsaaDeviceSupport;
using container::renderer::chooseRendererDepthStencilResolve;
using container::renderer::clampMsaaSampleCount;
using container::renderer::rendererMsaaAttachments;
using container::renderer::rendererMsaaSampleMask;
using container::renderer::supportedMsaaSampleCounts;

constexpr VkSampleCountFlags kSamples1To8 = VK_SAMPLE_COUNT_1_BIT |
    VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT | VK_SAMPLE_COUNT_8_BIT;
constexpr VkResolveModeFlags kReverseZDepth =
    VK_RESOLVE_MODE_MAX_BIT | VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;

RendererMsaaDeviceSupport supportAllSampleCounts() {
  RendererMsaaDeviceSupport support{};
  support.color = kSamples1To8;
  support.depth = kSamples1To8;
  support.stencil = kSamples1To8;
  support.integerColor = kSamples1To8;
  support.depthResolveModes = kReverseZDepth;
  support.stencilResolveModes = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
  support.independentResolveNone = true;
  return support;
}

TEST(RendererMsaa, SharedFrameAttachmentsUseActualFormatsAndImageUsages) {
  GBufferFormats formats{};
  // Give repeated format fields distinct values to catch omissions and
  // accidental aliasing. The same tuple drives both raster techniques.
  formats.albedo = VK_FORMAT_R8G8B8A8_SRGB;
  formats.normal = VK_FORMAT_R16G16B16A16_SNORM;
  formats.material = VK_FORMAT_R32G32B32A32_SFLOAT;
  formats.emissive = VK_FORMAT_R16G16B16A16_UNORM;
  formats.specular = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
  formats.pickId = VK_FORMAT_R32_UINT;
  formats.materialLayers = VK_FORMAT_R8G8B8A8_SNORM;
  formats.materialSheen = VK_FORMAT_A2R10G10B10_UNORM_PACK32;
  formats.sceneColor = VK_FORMAT_R16G16B16A16_SFLOAT;
  formats.depthStencil = VK_FORMAT_D24_UNORM_S8_UINT;
  const std::array colorFormats{formats.albedo, formats.normal, formats.material,
      formats.emissive, formats.specular, formats.pickId, formats.materialLayers,
      formats.materialSheen, formats.sceneColor};

  const auto attachments = rendererMsaaAttachments(formats);
  std::vector<VkFormat> actualColorFormats;
  unsigned depthCount = 0;
  for (const auto &attachment : attachments) {
    if (attachment.format == formats.depthStencil) {
      EXPECT_EQ(attachment.usage, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
      ++depthCount;
    } else {
      EXPECT_EQ(attachment.usage, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
      actualColorFormats.push_back(attachment.format);
    }
  }
  EXPECT_EQ(depthCount, 1u);
  EXPECT_TRUE(std::is_permutation(actualColorFormats.begin(), actualColorFormats.end(),
                                  colorFormats.begin(), colorFormats.end()));
}

TEST(RendererMsaa, EachAttachmentCanLimitTheSharedSampleMask) {
  const auto support = supportAllSampleCounts();
  std::array<VkSampleCountFlags, 10> attachmentMasks{};
  attachmentMasks.fill(kSamples1To8);
  EXPECT_EQ(rendererMsaaSampleMask(support, attachmentMasks), kSamples1To8);

  constexpr VkSampleCountFlags restricted = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
  for (auto &attachmentMask : attachmentMasks) {
    attachmentMask = restricted;
    EXPECT_EQ(rendererMsaaSampleMask(support, attachmentMasks), restricted);
    attachmentMask = kSamples1To8;
  }
}

TEST(RendererMsaa, ColorDepthStencilAndIntegerLimitsAllApply) {
  const std::array<VkSampleCountFlags, 2> attachmentMasks{kSamples1To8, kSamples1To8};
  constexpr VkSampleCountFlags restricted = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT;
  for (const auto limit : {&RendererMsaaDeviceSupport::color, &RendererMsaaDeviceSupport::depth,
                           &RendererMsaaDeviceSupport::stencil, &RendererMsaaDeviceSupport::integerColor}) {
    auto support = supportAllSampleCounts();
    support.*limit = restricted;
    EXPECT_EQ(rendererMsaaSampleMask(support, attachmentMasks), restricted);
  }
}

TEST(RendererMsaa, UnsupportedAttachmentQueryOrDisjointMasksLeaveSingleSample) {
  const auto support = supportAllSampleCounts();
  const std::array<VkSampleCountFlags, 2> failedQuery{kSamples1To8, 0};
  const std::array<VkSampleCountFlags, 2> disjoint{
      VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT,
      VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_8_BIT};
  EXPECT_EQ(rendererMsaaSampleMask(support, failedQuery), VK_SAMPLE_COUNT_1_BIT);
  EXPECT_EQ(rendererMsaaSampleMask(support, disjoint), VK_SAMPLE_COUNT_1_BIT);
}

TEST(RendererMsaa, OptionsAndClampingUseTheSameSparseMask) {
  auto support = supportAllSampleCounts();
  support.color |= VK_SAMPLE_COUNT_16_BIT;
  const std::array<VkSampleCountFlags, 2> attachmentMasks{
      VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT | VK_SAMPLE_COUNT_8_BIT,
      VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT};
  const auto mask = rendererMsaaSampleMask(support, attachmentMasks);
  const std::vector<VkSampleCountFlagBits> expected{VK_SAMPLE_COUNT_1_BIT, VK_SAMPLE_COUNT_4_BIT};
  EXPECT_EQ(supportedMsaaSampleCounts(mask), expected);
  EXPECT_EQ(clampMsaaSampleCount(0, mask), VK_SAMPLE_COUNT_1_BIT);
  EXPECT_EQ(clampMsaaSampleCount(3, mask), VK_SAMPLE_COUNT_1_BIT);
  EXPECT_EQ(clampMsaaSampleCount(4, mask), VK_SAMPLE_COUNT_4_BIT);
  EXPECT_EQ(clampMsaaSampleCount(64, mask), VK_SAMPLE_COUNT_4_BIT);
}

TEST(RendererMsaa, IndependentNoneKeepsReverseZMaximumWithoutResolvingStencil) {
  const auto resolve = chooseRendererDepthStencilResolve(kReverseZDepth, VK_RESOLVE_MODE_SAMPLE_ZERO_BIT, true);
  EXPECT_EQ(resolve.depth, VK_RESOLVE_MODE_MAX_BIT);
  EXPECT_EQ(resolve.stencil, VK_RESOLVE_MODE_NONE);

  const auto fallback = chooseRendererDepthStencilResolve(VK_RESOLVE_MODE_SAMPLE_ZERO_BIT, 0, true);
  EXPECT_EQ(fallback.depth, VK_RESOLVE_MODE_SAMPLE_ZERO_BIT);
  EXPECT_EQ(fallback.stencil, VK_RESOLVE_MODE_NONE);
}

TEST(RendererMsaa, RequiredCommonModePrefersMaximumThenSampleZero) {
  const auto maximum = chooseRendererDepthStencilResolve(kReverseZDepth, kReverseZDepth, false);
  EXPECT_EQ(maximum.depth, VK_RESOLVE_MODE_MAX_BIT);
  EXPECT_EQ(maximum.stencil, VK_RESOLVE_MODE_MAX_BIT);

  auto support = supportAllSampleCounts();
  support.independentResolveNone = false;
  const auto sampleZero = chooseRendererDepthStencilResolve(
      support.depthResolveModes, support.stencilResolveModes, support.independentResolveNone);
  EXPECT_EQ(sampleZero.depth, VK_RESOLVE_MODE_SAMPLE_ZERO_BIT);
  EXPECT_EQ(sampleZero.stencil, VK_RESOLVE_MODE_SAMPLE_ZERO_BIT);
  const std::array<VkSampleCountFlags, 1> attachmentMasks{kSamples1To8};
  EXPECT_EQ(rendererMsaaSampleMask(support, attachmentMasks), kSamples1To8);
}

TEST(RendererMsaa, NoUsableResolveDisablesMsaa) {
  auto support = supportAllSampleCounts();
  support.independentResolveNone = false;
  support.depthResolveModes = VK_RESOLVE_MODE_MAX_BIT;
  const std::array<VkSampleCountFlags, 1> attachmentMasks{kSamples1To8};
  const auto disjoint = chooseRendererDepthStencilResolve(support.depthResolveModes, support.stencilResolveModes, false);
  EXPECT_EQ(disjoint.depth, VK_RESOLVE_MODE_NONE);
  EXPECT_EQ(disjoint.stencil, VK_RESOLVE_MODE_NONE);
  EXPECT_EQ(rendererMsaaSampleMask(support, attachmentMasks), VK_SAMPLE_COUNT_1_BIT);

  support.independentResolveNone = true;
  for (VkResolveModeFlags modes : {0u, VkResolveModeFlags{VK_RESOLVE_MODE_AVERAGE_BIT | VK_RESOLVE_MODE_MIN_BIT}}) {
    support.depthResolveModes = modes;
    const auto unsupported = chooseRendererDepthStencilResolve(modes, support.stencilResolveModes, true);
    EXPECT_EQ(unsupported.depth, VK_RESOLVE_MODE_NONE);
    EXPECT_EQ(unsupported.stencil, VK_RESOLVE_MODE_NONE);
    EXPECT_EQ(rendererMsaaSampleMask(support, attachmentMasks), VK_SAMPLE_COUNT_1_BIT);
  }
}

} // namespace
