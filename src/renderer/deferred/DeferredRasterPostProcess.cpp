#include "Container/renderer/deferred/DeferredRasterPostProcess.h"

#include "Container/renderer/deferred/DeferredRasterFrameState.h"
#include "Container/renderer/scene/SceneViewport.h"
#include "Container/utility/GuiManager.h"

#include <algorithm>
#include <cmath>

namespace container::renderer {

namespace {

// Krystek's Planckian-locus approximation and CMCCAT2000 adaptation, adapted
// from nvpro_core2. See docs/third-party-notices.md.
glm::vec3 whiteBalanceGain(const DisplayGrade &grade) {
  constexpr DisplayGrade neutral{};
  if (grade.temperatureKelvin == neutral.temperatureKelvin &&
      grade.tintDuv == neutral.tintDuv)
    return glm::vec3(1.0f);

  const double t = std::isfinite(grade.temperatureKelvin)
      ? std::clamp(double(grade.temperatureKelvin), 2000.0, 15000.0)
      : neutral.temperatureKelvin;
  const double tint = std::isfinite(grade.tintDuv)
      ? std::clamp(double(grade.tintDuv), -0.025, 0.025) : neutral.tintDuv;
  const double uDenom = (7.08145163e-7 * t + 8.42420235e-4) * t + 1.0;
  const double vDenom = (1.61456053e-7 * t - 2.89741816e-5) * t + 1.0;
  glm::dvec2 uv(((1.28641212e-7 * t + 1.54118254e-4) * t + 0.860117757) / uDenom,
                ((4.20481691e-8 * t + 4.22806245e-5) * t + 0.317398726) / vDenom);
  const auto derivative = glm::normalize(glm::dvec2(
      ((-7.68136056380576e-13 * t - 9.60894034459919e-7) * t - 0.000570462348979613) /
          (uDenom * uDenom),
      ((-8.04477403759601e-12 * t - 1.8395552854377e-8) * t + 5.14769928267326e-5) /
          (vDenom * vDenom)));
  uv += tint * glm::dvec2(derivative.y, -derivative.x);
  const glm::dvec2 xy = uv * glm::dvec2(3.0, 2.0) / (2.0 * uv.x - 8.0 * uv.y + 4.0);
  const glm::dvec3 xyz(xy.x / xy.y, 1.0, std::max(1.0 - xy.x - xy.y, 0.0) / xy.y);
  const glm::dvec3 cone(glm::dot(glm::dvec3(0.7982, 0.3389, -0.1371), xyz),
                       glm::dot(glm::dvec3(-0.5918, 1.5512, 0.0406), xyz),
                       glm::dot(glm::dvec3(0.0008, 0.0239, 0.9753), xyz));
  return glm::vec3(glm::dvec3(0.948287308, 1.03290749, 1.08676255) /
                   glm::max(cone, glm::dvec3(0.05)));
}

[[nodiscard]] bool
displayModeUsesShadowCascadeSplits(container::ui::GBufferViewMode mode) {
  return mode == container::ui::GBufferViewMode::Overview ||
         mode == container::ui::GBufferViewMode::ShadowCascades ||
         mode == container::ui::GBufferViewMode::ShadowTexelDensity;
}

[[nodiscard]] uint32_t tileCountXForFramebufferWidth(uint32_t width) {
  return (width + container::gpu::kTileSize - 1u) / container::gpu::kTileSize;
}

} // namespace

container::gpu::PostProcessPushConstants buildDeferredPostProcessPushConstants(
    const DeferredPostProcessPushConstantInputs &inputs) {
  container::gpu::PostProcessPushConstants pushConstants{};
  pushConstants.outputMode = inputs.outputMode;
  pushConstants.bloomEnabled = inputs.bloomEnabled ? 1u : 0u;
  pushConstants.bloomIntensity = inputs.bloomIntensity;
  pushConstants.exposure = inputs.resolvedExposure;
  pushConstants.toneMapper = static_cast<uint32_t>(inputs.displayGrade.toneMapper);
  pushConstants.saturation = inputs.displayGrade.saturation;
  pushConstants.contrast = inputs.displayGrade.contrast;
  const auto gain = whiteBalanceGain(inputs.displayGrade);
  pushConstants.whiteBalanceGain[0] = gain.x;
  pushConstants.whiteBalanceGain[1] = gain.y;
  pushConstants.whiteBalanceGain[2] = gain.z;
  pushConstants.vibrance = inputs.displayGrade.vibrance;
  std::ranges::copy(inputs.displayGrade.shadowTone, pushConstants.shadowTone);
  std::ranges::copy(inputs.displayGrade.highlightTone, pushConstants.highlightTone);
  pushConstants.exposureMode = inputs.exposureSettings.mode;
  pushConstants.targetLuminance = inputs.exposureSettings.targetLuminance;
  pushConstants.minExposure = inputs.exposureSettings.minExposure;
  pushConstants.maxExposure = inputs.exposureSettings.maxExposure;
  pushConstants.adaptationRate = inputs.exposureSettings.adaptationRate;
  pushConstants.cameraNear = inputs.cameraNear;
  pushConstants.cameraFar = inputs.cameraFar;

  if (inputs.includeShadowCascadeSplits && inputs.shadowData != nullptr) {
    for (uint32_t i = 0; i < container::gpu::kShadowCascadeCount; ++i) {
      pushConstants.cascadeSplits[i] =
          inputs.shadowData->cascades[i].splitDepth;
    }
  }

  if (inputs.tileCullActive) {
    pushConstants.tileCountX = inputs.tileCountX;
    pushConstants.totalLights = inputs.totalLights;
    pushConstants.depthSliceCount = inputs.depthSliceCount;
  } else {
    pushConstants.tileCountX = 1u;
    pushConstants.totalLights = 0u;
    pushConstants.depthSliceCount = 1u;
  }

  pushConstants.oitEnabled = inputs.oitEnabled ? 1u : 0u;
  return pushConstants;
}

float resolvePostProcessExposure(
    const container::gpu::ExposureSettings &settings) {
  if (settings.mode == container::gpu::kExposureModeManual) {
    return settings.manualExposure;
  }

  return std::clamp(settings.manualExposure, settings.minExposure,
                    settings.maxExposure);
}

DeferredPostProcessStateBuilder::DeferredPostProcessStateBuilder(
    DeferredPostProcessFrameInputs inputs)
    : inputs_(inputs) {}

DeferredPostProcessFrameState DeferredPostProcessStateBuilder::build() const {
  const bool bloomActive = displayModeRecordsBloom(inputs_.displayMode) &&
                           inputs_.bloomPassActive && inputs_.bloomReady &&
                           inputs_.bloomEnabled;
  const bool tileCullActive = displayModeRecordsTileCull(inputs_.displayMode) &&
                              inputs_.tileCullPassActive &&
                              inputs_.tiledLightingReady;

  return {
      .pushConstants = buildDeferredPostProcessPushConstants(
          {.outputMode = static_cast<uint32_t>(inputs_.displayMode),
           .bloomEnabled = bloomActive,
           .bloomIntensity = inputs_.bloomIntensity,
           .exposureSettings = inputs_.exposureSettings,
           .displayGrade = inputs_.displayGrade,
           .resolvedExposure = inputs_.resolvedExposure,
           .cameraNear = inputs_.cameraNear,
           .cameraFar = inputs_.cameraFar,
           .includeShadowCascadeSplits =
               displayModeUsesShadowCascadeSplits(inputs_.displayMode),
           .shadowData = inputs_.shadowData,
           .tileCullActive = tileCullActive,
           .tileCountX =
               tileCountXForFramebufferWidth(inputs_.framebufferWidth),
           .totalLights = inputs_.pointLightCount,
           .depthSliceCount = container::gpu::kClusterDepthSlices,
           .oitEnabled = inputs_.transparentOitActive}),
      .bloomActive = bloomActive,
      .tileCullActive = tileCullActive,
  };
}

DeferredPostProcessFrameState buildDeferredPostProcessFrameState(
    const DeferredPostProcessFrameInputs &inputs) {
  return DeferredPostProcessStateBuilder(inputs).build();
}

bool deferredPostProcessPassRecordInputsReady(
    const DeferredPostProcessPassRecordInputs &inputs) {
  if (inputs.commandBuffer == VK_NULL_HANDLE ||
      inputs.renderPass == VK_NULL_HANDLE ||
      inputs.swapChainFramebuffers == nullptr ||
      inputs.imageIndex >= inputs.swapChainFramebuffers->size() ||
      inputs.extent.width == 0u || inputs.extent.height == 0u ||
      inputs.pipeline == VK_NULL_HANDLE ||
      inputs.pipelineLayout == VK_NULL_HANDLE) {
    return false;
  }

  if ((*inputs.swapChainFramebuffers)[inputs.imageIndex] == VK_NULL_HANDLE) {
    return false;
  }

  return std::all_of(inputs.descriptorSets.begin(),
                     inputs.descriptorSets.end(),
                     [](VkDescriptorSet descriptorSet) {
                       return descriptorSet != VK_NULL_HANDLE;
                     });
}

bool recordDeferredPostProcessPassCommands(
    const DeferredPostProcessPassRecordInputs &inputs) {
  if (!deferredPostProcessPassRecordInputsReady(inputs)) {
    return false;
  }

  DeferredPostProcessFrameInputs frameInputs = inputs.frameInputs;
  frameInputs.framebufferWidth = inputs.extent.width;
  DeferredPostProcessFrameState postProcessState =
      buildDeferredPostProcessFrameState(frameInputs);
  // Keep the 128-byte push constant ABI. The low bits still select the display mode.
  if (inputs.captureSceneLinear)
    postProcessState.pushConstants.outputMode |= 0x80000000u;

  const DeferredPostProcessPassScope postProcessPass(
      {.commandBuffer = inputs.commandBuffer,
       .renderPass = inputs.renderPass,
       .framebuffer = (*inputs.swapChainFramebuffers)[inputs.imageIndex],
       .extent = inputs.extent});
  postProcessPass.recordFullscreenDraw(
      {.pipeline = inputs.pipeline,
       .pipelineLayout = inputs.pipelineLayout,
       .descriptorSets = inputs.descriptorSets,
       .pushConstants = postProcessState.pushConstants});

  if (inputs.recordAfterFullscreenDraw) {
    inputs.recordAfterFullscreenDraw(inputs.commandBuffer);
  }

  return true;
}

DeferredPostProcessPassScope::DeferredPostProcessPassScope(
    const DeferredPostProcessPassBeginInfo &beginInfo)
    : commandBuffer_(beginInfo.commandBuffer), extent_(beginInfo.extent) {
  RenderingBeginInfo info{};
  info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  info.renderPass = beginInfo.renderPass;
  info.framebuffer = beginInfo.framebuffer;
  info.renderArea.offset = {0, 0};
  info.renderArea.extent = beginInfo.extent;
  VkClearValue clearVal{};
  clearVal.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  info.clearValueCount = 1;
  info.pClearValues = &clearVal;

  beginDynamicRendering(commandBuffer_, &info, VK_SUBPASS_CONTENTS_INLINE);
}

DeferredPostProcessPassScope::~DeferredPostProcessPassScope() {
  if (commandBuffer_ != VK_NULL_HANDLE) {
    endDynamicRendering(commandBuffer_);
  }
}

void DeferredPostProcessPassScope::recordFullscreenDraw(
    const DeferredPostProcessFullscreenDraw &draw) const {
  vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    draw.pipeline);
  vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          draw.pipelineLayout, 0,
                          static_cast<uint32_t>(draw.descriptorSets.size()),
                          draw.descriptorSets.data(), 0, nullptr);
  recordSceneViewportAndScissor(commandBuffer_, extent_);
  vkCmdPushConstants(
      commandBuffer_, draw.pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
      sizeof(container::gpu::PostProcessPushConstants), &draw.pushConstants);
  vkCmdDraw(commandBuffer_, 3, 1, 0, 0);
}

} // namespace container::renderer
