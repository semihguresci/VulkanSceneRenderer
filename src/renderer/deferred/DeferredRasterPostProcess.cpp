#include "Container/renderer/deferred/DeferredRasterPostProcess.h"

#include "Container/renderer/deferred/DeferredRasterFrameState.h"
#include "Container/renderer/scene/SceneViewport.h"
#include "Container/utility/GuiManager.h"

#include <algorithm>

namespace container::renderer {

namespace {

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
  const DeferredPostProcessFrameState postProcessState =
      buildDeferredPostProcessFrameState(frameInputs);

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
