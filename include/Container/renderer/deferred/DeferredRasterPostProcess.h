#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/utility/SceneData.h"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace container::ui {
enum class GBufferViewMode : uint32_t;
} // namespace container::ui

namespace container::renderer {

struct DeferredPostProcessPushConstantInputs {
  uint32_t outputMode{0};
  bool bloomEnabled{false};
  float bloomIntensity{0.0f};
  container::gpu::ExposureSettings exposureSettings{};
  float resolvedExposure{0.25f};
  float cameraNear{0.1f};
  float cameraFar{100.0f};
  bool includeShadowCascadeSplits{false};
  const container::gpu::ShadowData *shadowData{nullptr};
  bool tileCullActive{false};
  uint32_t tileCountX{1};
  uint32_t totalLights{0};
  uint32_t depthSliceCount{container::gpu::kClusterDepthSlices};
  bool oitEnabled{false};
};

struct DeferredPostProcessFrameInputs {
  container::ui::GBufferViewMode displayMode{};
  bool bloomPassActive{false};
  bool bloomReady{false};
  bool bloomEnabled{false};
  float bloomIntensity{0.0f};
  container::gpu::ExposureSettings exposureSettings{};
  float resolvedExposure{0.25f};
  float cameraNear{0.1f};
  float cameraFar{100.0f};
  const container::gpu::ShadowData *shadowData{nullptr};
  bool tileCullPassActive{false};
  bool tiledLightingReady{false};
  uint32_t framebufferWidth{0};
  uint32_t pointLightCount{0};
  bool transparentOitActive{false};
};

struct DeferredPostProcessFrameState {
  container::gpu::PostProcessPushConstants pushConstants{};
  bool bloomActive{false};
  bool tileCullActive{false};
};

class DeferredPostProcessStateBuilder {
public:
  explicit DeferredPostProcessStateBuilder(
      DeferredPostProcessFrameInputs inputs);

  [[nodiscard]] DeferredPostProcessFrameState build() const;

private:
  DeferredPostProcessFrameInputs inputs_{};
};

struct DeferredPostProcessPassBeginInfo {
  VkCommandBuffer commandBuffer{VK_NULL_HANDLE};
  RenderingPassHandle renderPass{VK_NULL_HANDLE};
  RenderingTargetHandle framebuffer{VK_NULL_HANDLE};
  VkExtent2D extent{};
};

struct DeferredPostProcessFullscreenDraw {
  VkPipeline pipeline{VK_NULL_HANDLE};
  VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
  std::array<VkDescriptorSet, 2> descriptorSets{};
  container::gpu::PostProcessPushConstants pushConstants{};
};

struct DeferredPostProcessPassRecordInputs {
  VkCommandBuffer commandBuffer{VK_NULL_HANDLE};
  RenderingPassHandle renderPass{VK_NULL_HANDLE};
  const std::vector<RenderingTargetHandle> *swapChainFramebuffers{nullptr};
  uint32_t imageIndex{0};
  VkExtent2D extent{};
  VkPipeline pipeline{VK_NULL_HANDLE};
  VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
  std::array<VkDescriptorSet, 2> descriptorSets{};
  DeferredPostProcessFrameInputs frameInputs{};
  std::function<void(VkCommandBuffer)> recordAfterFullscreenDraw{};
};

class DeferredPostProcessPassScope {
public:
  explicit DeferredPostProcessPassScope(
      const DeferredPostProcessPassBeginInfo &beginInfo);
  ~DeferredPostProcessPassScope();

  DeferredPostProcessPassScope(const DeferredPostProcessPassScope &) = delete;
  DeferredPostProcessPassScope &
  operator=(const DeferredPostProcessPassScope &) = delete;
  DeferredPostProcessPassScope(DeferredPostProcessPassScope &&) = delete;
  DeferredPostProcessPassScope &
  operator=(DeferredPostProcessPassScope &&) = delete;

  void
  recordFullscreenDraw(const DeferredPostProcessFullscreenDraw &draw) const;

private:
  VkCommandBuffer commandBuffer_{VK_NULL_HANDLE};
  VkExtent2D extent_{};
};

[[nodiscard]] container::gpu::PostProcessPushConstants
buildDeferredPostProcessPushConstants(
    const DeferredPostProcessPushConstantInputs &inputs);

[[nodiscard]] float
resolvePostProcessExposure(const container::gpu::ExposureSettings &settings);

[[nodiscard]] DeferredPostProcessFrameState buildDeferredPostProcessFrameState(
    const DeferredPostProcessFrameInputs &inputs);

[[nodiscard]] bool deferredPostProcessPassRecordInputsReady(
    const DeferredPostProcessPassRecordInputs &inputs);

[[nodiscard]] bool recordDeferredPostProcessPassCommands(
    const DeferredPostProcessPassRecordInputs &inputs);

} // namespace container::renderer
