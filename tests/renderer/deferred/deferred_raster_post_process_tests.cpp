#include "Container/renderer/deferred/DeferredRasterPostProcess.h"

#include "Container/renderer/deferred/DeferredRasterFrameState.h"
#include "Container/utility/GuiManager.h"

#include <gtest/gtest.h>

namespace {

using container::renderer::buildDeferredPostProcessFrameState;
using container::renderer::buildDeferredPostProcessPushConstants;
using container::renderer::DeferredPostProcessFrameInputs;
using container::renderer::DeferredPostProcessPassRecordInputs;
using container::renderer::DeferredPostProcessPushConstantInputs;
using container::renderer::deferredPostProcessPassRecordInputsReady;
using container::renderer::displayModeRecordsBloom;
using container::renderer::displayModeRecordsExposureAdaptation;
using container::renderer::displayModeRecordsGtao;
using container::renderer::displayModeRecordsShadowAtlas;
using container::renderer::displayModeRecordsTileCull;
using container::renderer::FrameRecordParams;
using container::renderer::currentDisplayMode;
using container::renderer::recordDeferredPostProcessPassCommands;
using container::renderer::resolvePostProcessExposure;
using container::renderer::shouldRecordTransparentOit;

template <typename Handle>
Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

DeferredPostProcessPassRecordInputs
readyPostProcessRecordInputs(std::vector<VkFramebuffer> &framebuffers) {
  framebuffers = {fakeHandle<VkFramebuffer>(0x101)};
  return {.commandBuffer = fakeHandle<VkCommandBuffer>(0x102),
          .renderPass = fakeHandle<VkRenderPass>(0x103),
          .swapChainFramebuffers = &framebuffers,
          .imageIndex = 0u,
          .extent = {1920u, 1080u},
          .pipeline = fakeHandle<VkPipeline>(0x104),
          .pipelineLayout = fakeHandle<VkPipelineLayout>(0x105),
          .descriptorSets = {fakeHandle<VkDescriptorSet>(0x106),
                             fakeHandle<VkDescriptorSet>(0x107)}};
}

TEST(DeferredRasterPostProcessTests, MapsExposureCameraBloomAndOitState) {
  container::gpu::ExposureSettings exposure{};
  exposure.mode = container::gpu::kExposureModeAuto;
  exposure.targetLuminance = 0.22f;
  exposure.minExposure = 0.05f;
  exposure.maxExposure = 6.0f;
  exposure.adaptationRate = 2.5f;

  const auto pc =
      buildDeferredPostProcessPushConstants({.outputMode = 12u,
                                             .bloomEnabled = true,
                                             .bloomIntensity = 0.75f,
                                             .exposureSettings = exposure,
                                             .resolvedExposure = 1.4f,
                                             .cameraNear = 0.25f,
                                             .cameraFar = 500.0f,
                                             .oitEnabled = true});

  EXPECT_EQ(pc.outputMode, 12u);
  EXPECT_EQ(pc.bloomEnabled, 1u);
  EXPECT_FLOAT_EQ(pc.bloomIntensity, 0.75f);
  EXPECT_FLOAT_EQ(pc.exposure, 1.4f);
  EXPECT_FLOAT_EQ(pc.cameraNear, 0.25f);
  EXPECT_FLOAT_EQ(pc.cameraFar, 500.0f);
  EXPECT_EQ(pc.oitEnabled, 1u);
  EXPECT_EQ(pc.exposureMode, container::gpu::kExposureModeAuto);
  EXPECT_FLOAT_EQ(pc.targetLuminance, 0.22f);
  EXPECT_FLOAT_EQ(pc.minExposure, 0.05f);
  EXPECT_FLOAT_EQ(pc.maxExposure, 6.0f);
  EXPECT_FLOAT_EQ(pc.adaptationRate, 2.5f);
}

TEST(DeferredRasterPostProcessTests, KeepsInactiveTileCullFallbackCompact) {
  const auto pc =
      buildDeferredPostProcessPushConstants({.tileCullActive = false,
                                             .tileCountX = 42u,
                                             .totalLights = 13u,
                                             .depthSliceCount = 9u});

  EXPECT_EQ(pc.tileCountX, 1u);
  EXPECT_EQ(pc.totalLights, 0u);
  EXPECT_EQ(pc.depthSliceCount, 1u);
}

TEST(DeferredRasterPostProcessTests, ManualExposureBypassesFallbackClamp) {
  container::gpu::ExposureSettings exposure{};
  exposure.mode = container::gpu::kExposureModeManual;
  exposure.manualExposure = 0.25f;
  exposure.minExposure = 1.0f;
  exposure.maxExposure = 2.0f;

  EXPECT_FLOAT_EQ(resolvePostProcessExposure(exposure), 0.25f);
}

TEST(DeferredRasterPostProcessTests, AutoExposureFallbackClampsToRange) {
  container::gpu::ExposureSettings exposure{};
  exposure.mode = container::gpu::kExposureModeAuto;
  exposure.minExposure = 1.0f;
  exposure.maxExposure = 2.0f;

  exposure.manualExposure = 0.25f;
  EXPECT_FLOAT_EQ(resolvePostProcessExposure(exposure), 1.0f);

  exposure.manualExposure = 1.5f;
  EXPECT_FLOAT_EQ(resolvePostProcessExposure(exposure), 1.5f);

  exposure.manualExposure = 3.0f;
  EXPECT_FLOAT_EQ(resolvePostProcessExposure(exposure), 2.0f);
}

TEST(DeferredRasterPostProcessTests, MapsActiveTileCullMetadata) {
  const auto pc =
      buildDeferredPostProcessPushConstants({.tileCullActive = true,
                                             .tileCountX = 42u,
                                             .totalLights = 13u,
                                             .depthSliceCount = 9u});

  EXPECT_EQ(pc.tileCountX, 42u);
  EXPECT_EQ(pc.totalLights, 13u);
  EXPECT_EQ(pc.depthSliceCount, 9u);
}

TEST(DeferredRasterPostProcessTests, IncludesShadowSplitsOnlyWhenRequested) {
  container::gpu::ShadowData shadowData{};
  for (uint32_t i = 0; i < container::gpu::kShadowCascadeCount; ++i) {
    shadowData.cascades[i].splitDepth = 10.0f + static_cast<float>(i);
  }

  const auto omitted = buildDeferredPostProcessPushConstants(
      {.includeShadowCascadeSplits = false, .shadowData = &shadowData});
  const auto included = buildDeferredPostProcessPushConstants(
      {.includeShadowCascadeSplits = true, .shadowData = &shadowData});

  for (uint32_t i = 0; i < container::gpu::kShadowCascadeCount; ++i) {
    EXPECT_FLOAT_EQ(omitted.cascadeSplits[i], 0.0f);
    EXPECT_FLOAT_EQ(included.cascadeSplits[i], 10.0f + static_cast<float>(i));
  }
}

TEST(DeferredRasterPostProcessTests, FrameStateAppliesDisplayModePolicy) {
  DeferredPostProcessFrameInputs inputs{};
  inputs.displayMode = container::ui::GBufferViewMode::Overview;
  inputs.bloomPassActive = true;
  inputs.bloomReady = true;
  inputs.bloomEnabled = true;
  inputs.bloomIntensity = 0.5f;
  inputs.tileCullPassActive = true;
  inputs.tiledLightingReady = true;
  inputs.framebufferWidth = 65u;
  inputs.pointLightCount = 7u;

  const auto overviewState = buildDeferredPostProcessFrameState(inputs);
  EXPECT_TRUE(overviewState.bloomActive);
  EXPECT_TRUE(overviewState.tileCullActive);
  EXPECT_EQ(overviewState.pushConstants.bloomEnabled, 1u);
  EXPECT_EQ(overviewState.pushConstants.tileCountX, 5u);
  EXPECT_EQ(overviewState.pushConstants.totalLights, 7u);
  EXPECT_EQ(overviewState.pushConstants.depthSliceCount,
            container::gpu::kClusterDepthSlices);

  inputs.displayMode = container::ui::GBufferViewMode::Lit;
  const auto litState = buildDeferredPostProcessFrameState(inputs);
  EXPECT_TRUE(litState.bloomActive);
  EXPECT_TRUE(litState.tileCullActive);
  EXPECT_EQ(litState.pushConstants.bloomEnabled, 1u);
  EXPECT_EQ(litState.pushConstants.tileCountX, 5u);
  EXPECT_EQ(litState.pushConstants.totalLights, 7u);
  EXPECT_EQ(litState.pushConstants.depthSliceCount,
            container::gpu::kClusterDepthSlices);
}

TEST(DeferredRasterPostProcessTests, FrameStateUsesShadowDebugModesForSplits) {
  container::gpu::ShadowData shadowData{};
  for (uint32_t i = 0; i < container::gpu::kShadowCascadeCount; ++i) {
    shadowData.cascades[i].splitDepth = 20.0f + static_cast<float>(i);
  }

  DeferredPostProcessFrameInputs inputs{};
  inputs.displayMode = container::ui::GBufferViewMode::Lit;
  inputs.shadowData = &shadowData;
  const auto litState = buildDeferredPostProcessFrameState(inputs);

  inputs.displayMode = container::ui::GBufferViewMode::Overview;
  const auto overviewState = buildDeferredPostProcessFrameState(inputs);

  inputs.displayMode = container::ui::GBufferViewMode::ShadowTexelDensity;
  const auto shadowDebugState = buildDeferredPostProcessFrameState(inputs);

  for (uint32_t i = 0; i < container::gpu::kShadowCascadeCount; ++i) {
    EXPECT_FLOAT_EQ(litState.pushConstants.cascadeSplits[i], 0.0f);
    EXPECT_FLOAT_EQ(overviewState.pushConstants.cascadeSplits[i],
                    20.0f + static_cast<float>(i));
    EXPECT_FLOAT_EQ(shadowDebugState.pushConstants.cascadeSplits[i],
                    20.0f + static_cast<float>(i));
  }
}

TEST(DeferredRasterPostProcessTests,
     OverviewModeRequestsInputsForEmbeddedDebugPanels) {
  const auto overview = container::ui::GBufferViewMode::Overview;

  EXPECT_TRUE(displayModeRecordsShadowAtlas(overview));
  EXPECT_TRUE(displayModeRecordsTileCull(overview));
  EXPECT_TRUE(displayModeRecordsGtao(overview));
  EXPECT_TRUE(displayModeRecordsExposureAdaptation(overview));
  EXPECT_TRUE(displayModeRecordsBloom(overview));
}

TEST(DeferredRasterPostProcessTests,
     OverviewModeRecordsTransparentOitForEmbeddedTransparencyPanels) {
  std::vector<container::renderer::DrawCommand> transparentDraws(1);
  FrameRecordParams params{};
  params.draws.transparentDrawCommands = &transparentDraws;

  EXPECT_TRUE(shouldRecordTransparentOit(params, nullptr));
}

TEST(DeferredRasterPostProcessTests,
     HeadlessDisplayModeUsesExplicitFallbackWhenGuiIsUnavailable) {
  EXPECT_EQ(currentDisplayMode(nullptr, container::ui::GBufferViewMode::Lit),
            container::ui::GBufferViewMode::Lit);

  std::vector<container::renderer::DrawCommand> transparentDraws(1);
  FrameRecordParams params{};
  params.draws.transparentDrawCommands = &transparentDraws;

  EXPECT_FALSE(shouldRecordTransparentOit(
      params, nullptr, container::ui::GBufferViewMode::ShadowTexelDensity));
}

TEST(DeferredRasterPostProcessTests,
     RejectsIncompleteRecordInputsBeforeRecording) {
  std::vector<VkFramebuffer> framebuffers;
  DeferredPostProcessPassRecordInputs inputs =
      readyPostProcessRecordInputs(framebuffers);
  EXPECT_TRUE(deferredPostProcessPassRecordInputsReady(inputs));

  auto invalid = inputs;
  invalid.commandBuffer = VK_NULL_HANDLE;
  EXPECT_FALSE(deferredPostProcessPassRecordInputsReady(invalid));

  invalid = inputs;
  invalid.renderPass = VK_NULL_HANDLE;
  EXPECT_FALSE(deferredPostProcessPassRecordInputsReady(invalid));

  invalid = inputs;
  invalid.swapChainFramebuffers = nullptr;
  EXPECT_FALSE(deferredPostProcessPassRecordInputsReady(invalid));

  invalid = inputs;
  invalid.imageIndex = 1u;
  EXPECT_FALSE(deferredPostProcessPassRecordInputsReady(invalid));

  invalid = inputs;
  framebuffers[0] = VK_NULL_HANDLE;
  EXPECT_FALSE(deferredPostProcessPassRecordInputsReady(invalid));
  framebuffers[0] = fakeHandle<VkFramebuffer>(0x101);

  invalid = inputs;
  invalid.extent.width = 0u;
  EXPECT_FALSE(deferredPostProcessPassRecordInputsReady(invalid));

  invalid = inputs;
  invalid.extent.height = 0u;
  EXPECT_FALSE(deferredPostProcessPassRecordInputsReady(invalid));

  invalid = inputs;
  invalid.pipeline = VK_NULL_HANDLE;
  EXPECT_FALSE(deferredPostProcessPassRecordInputsReady(invalid));

  invalid = inputs;
  invalid.pipelineLayout = VK_NULL_HANDLE;
  EXPECT_FALSE(deferredPostProcessPassRecordInputsReady(invalid));

  invalid = inputs;
  invalid.descriptorSets[0] = VK_NULL_HANDLE;
  EXPECT_FALSE(deferredPostProcessPassRecordInputsReady(invalid));

  invalid = inputs;
  invalid.descriptorSets[1] = VK_NULL_HANDLE;
  EXPECT_FALSE(deferredPostProcessPassRecordInputsReady(invalid));
}

TEST(DeferredRasterPostProcessTests,
     RecordReturnsFalseForIncompleteInputs) {
  EXPECT_FALSE(recordDeferredPostProcessPassCommands({}));

  std::vector<VkFramebuffer> framebuffers;
  DeferredPostProcessPassRecordInputs inputs =
      readyPostProcessRecordInputs(framebuffers);
  inputs.pipeline = VK_NULL_HANDLE;

  EXPECT_FALSE(recordDeferredPostProcessPassCommands(inputs));
}

} // namespace
