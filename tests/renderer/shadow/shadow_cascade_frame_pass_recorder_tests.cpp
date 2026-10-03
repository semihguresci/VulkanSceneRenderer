#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/pipeline/PipelineTypes.h"
#include "Container/renderer/resources/FrameResourceRegistry.h"
#include "Container/renderer/shadow/ShadowCascadeFramePassRecorder.h"

#include <gtest/gtest.h>

#include <array>

namespace {

using container::gpu::kShadowCascadeCount;
using container::renderer::FrameRecordParams;
using container::renderer::FrameDescriptorBinding;
using container::renderer::FrameResourceRegistry;
using container::renderer::GraphicsPipelines;
using container::renderer::PipelineRegistry;
using container::renderer::RenderTechniqueId;
using container::renderer::ShadowCascadeFramePassRecorder;
using container::renderer::buildGraphicsPipelineHandleRegistry;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

[[nodiscard]] FrameRecordParams recordableShadowParams(
    std::array<RenderingTargetHandle, kShadowCascadeCount> &framebuffers,
    const PipelineRegistry *pipelineHandles,
    const FrameResourceRegistry *resourceBindings) {
  framebuffers[0] = fakeHandle<RenderingTargetHandle>(0x4);

  FrameRecordParams params{};
  params.shadows.renderPass = fakeHandle<RenderingPassHandle>(0x1);
  params.registries.pipelineHandles = pipelineHandles;
  params.registries.resourceBindings = resourceBindings;
  params.shadows.shadowFramebuffers = framebuffers.data();
  return params;
}

} // namespace

TEST(ShadowCascadeFramePassRecorderTests,
     CanRecordCascadeRequiresShadowPassResources) {
  std::array<RenderingTargetHandle, kShadowCascadeCount> framebuffers{};
  GraphicsPipelines pipelines;
  pipelines.shadowDepth = fakeHandle<VkPipeline>(0x2);
  const auto pipelineRegistry = buildGraphicsPipelineHandleRegistry(pipelines);
  FrameResourceRegistry resourceBindings;
  resourceBindings.bindDescriptorSet(
      RenderTechniqueId::DeferredRaster, "shadow-descriptor-set", 0u,
      FrameDescriptorBinding{
          .descriptorSet = fakeHandle<VkDescriptorSet>(0x3)});
  const FrameRecordParams params =
      recordableShadowParams(framebuffers, pipelineRegistry.get(),
                             &resourceBindings);
  const ShadowCascadeFramePassRecorder recorder;

  EXPECT_TRUE(recorder.canRecordCascade(params, 0u));
  EXPECT_FALSE(recorder.canRecordCascade(params, kShadowCascadeCount));
}

TEST(ShadowCascadeFramePassRecorderTests,
     CanRecordCascadeRejectsMissingFramebuffer) {
  std::array<RenderingTargetHandle, kShadowCascadeCount> framebuffers{};
  GraphicsPipelines pipelines;
  pipelines.shadowDepth = fakeHandle<VkPipeline>(0x2);
  const auto pipelineRegistry = buildGraphicsPipelineHandleRegistry(pipelines);
  FrameResourceRegistry resourceBindings;
  resourceBindings.bindDescriptorSet(
      RenderTechniqueId::DeferredRaster, "shadow-descriptor-set", 0u,
      FrameDescriptorBinding{
          .descriptorSet = fakeHandle<VkDescriptorSet>(0x3)});
  FrameRecordParams params =
      recordableShadowParams(framebuffers, pipelineRegistry.get(),
                             &resourceBindings);
  framebuffers[0] = VK_NULL_HANDLE;
  const ShadowCascadeFramePassRecorder recorder;

  EXPECT_FALSE(recorder.canRecordCascade(params, 0u));
}

TEST(ShadowCascadeFramePassRecorderTests,
     CanRecordCascadeRejectsMissingPipeline) {
  std::array<RenderingTargetHandle, kShadowCascadeCount> framebuffers{};
  FrameResourceRegistry resourceBindings;
  resourceBindings.bindDescriptorSet(
      RenderTechniqueId::DeferredRaster, "shadow-descriptor-set", 0u,
      FrameDescriptorBinding{
          .descriptorSet = fakeHandle<VkDescriptorSet>(0x3)});
  FrameRecordParams params =
      recordableShadowParams(framebuffers, nullptr, &resourceBindings);
  const ShadowCascadeFramePassRecorder recorder;

  EXPECT_FALSE(recorder.canRecordCascade(params, 0u));
}
