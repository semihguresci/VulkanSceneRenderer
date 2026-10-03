#include "Container/renderer/bim/BimSurfaceRasterPassRecorder.h"
#include "Container/renderer/scene/DrawCommand.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using container::renderer::BimSurfaceFramePassRecordInputs;
using container::renderer::BimSurfaceFrameBindingInputs;
using container::renderer::BimSurfacePassKind;
using container::renderer::BimSurfacePassPlan;
using container::renderer::bimSurfaceRasterPassPushConstants;
using container::renderer::BimSurfaceRasterPassRecordInputs;
using container::renderer::buildBimSurfaceFrameBinding;
using container::renderer::buildBimSurfaceFramePassInputs;
using container::renderer::buildBimSurfaceFramePassPlan;
using container::renderer::DrawCommand;
using container::renderer::recordBimSurfaceFramePassCommands;
using container::renderer::recordBimSurfaceRasterPassCommands;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

BimSurfacePassPlan activePlan() {
  BimSurfacePassPlan plan{};
  plan.active = true;
  return plan;
}

BimSurfaceRasterPassRecordInputs
requiredInputs(const BimSurfacePassPlan &plan) {
  return {.renderPass = fakeHandle<RenderingPassHandle>(0x1),
          .framebuffer = fakeHandle<RenderingTargetHandle>(0x2),
          .extent = {640u, 480u},
          .plan = &plan};
}

BimSurfaceFramePassRecordInputs
frameInputs(const std::vector<DrawCommand> &meshCommands,
            const container::gpu::BindlessPushConstants &pushConstants,
            std::span<const VkDescriptorSet> descriptorSets) {
  return {.kind = BimSurfacePassKind::GBuffer,
          .passReady = true,
          .draws = {.mesh = {.opaqueSingleSidedDrawCommands = &meshCommands},
                    .opaqueMeshDrawsUseGpuVisibility = true},
          .renderPass = fakeHandle<RenderingPassHandle>(0x10),
          .framebuffer = fakeHandle<RenderingTargetHandle>(0x11),
          .extent = {1280u, 720u},
          .geometry = {.descriptorSets = descriptorSets,
                       .vertexSlice = {.buffer = fakeHandle<VkBuffer>(0x12)},
                       .indexSlice = {.buffer = fakeHandle<VkBuffer>(0x13)}},
          .pipelines = {.singleSided = fakeHandle<VkPipeline>(0x14)},
          .pipelineLayout = fakeHandle<VkPipelineLayout>(0x15),
          .pushConstants = &pushConstants,
          .semanticColorMode = 4u};
}

} // namespace

TEST(BimSurfaceRasterPassRecorderTests, MissingCommandBufferReturnsFalse) {
  const BimSurfacePassPlan plan = activePlan();

  EXPECT_FALSE(
      recordBimSurfaceRasterPassCommands(VK_NULL_HANDLE, requiredInputs(plan)));
}

TEST(BimSurfaceRasterPassRecorderTests, MissingPlanReturnsFalse) {
  const BimSurfacePassPlan plan = activePlan();
  BimSurfaceRasterPassRecordInputs inputs = requiredInputs(plan);
  inputs.plan = nullptr;

  EXPECT_FALSE(recordBimSurfaceRasterPassCommands(
      fakeHandle<VkCommandBuffer>(0x3), inputs));
}

TEST(BimSurfaceRasterPassRecorderTests, InactivePlanReturnsFalse) {
  BimSurfacePassPlan plan = activePlan();
  plan.active = false;

  EXPECT_FALSE(recordBimSurfaceRasterPassCommands(
      fakeHandle<VkCommandBuffer>(0x3), requiredInputs(plan)));
}

TEST(BimSurfaceRasterPassRecorderTests, MissingRenderPassReturnsFalse) {
  const BimSurfacePassPlan plan = activePlan();
  BimSurfaceRasterPassRecordInputs inputs = requiredInputs(plan);
  inputs.renderPass = VK_NULL_HANDLE;

  EXPECT_FALSE(recordBimSurfaceRasterPassCommands(
      fakeHandle<VkCommandBuffer>(0x3), inputs));
}

TEST(BimSurfaceRasterPassRecorderTests, MissingFramebufferReturnsFalse) {
  const BimSurfacePassPlan plan = activePlan();
  BimSurfaceRasterPassRecordInputs inputs = requiredInputs(plan);
  inputs.framebuffer = VK_NULL_HANDLE;

  EXPECT_FALSE(recordBimSurfaceRasterPassCommands(
      fakeHandle<VkCommandBuffer>(0x3), inputs));
}

TEST(BimSurfaceRasterPassRecorderTests, ZeroWidthReturnsFalse) {
  const BimSurfacePassPlan plan = activePlan();
  BimSurfaceRasterPassRecordInputs inputs = requiredInputs(plan);
  inputs.extent.width = 0u;

  EXPECT_FALSE(recordBimSurfaceRasterPassCommands(
      fakeHandle<VkCommandBuffer>(0x3), inputs));
}

TEST(BimSurfaceRasterPassRecorderTests, ZeroHeightReturnsFalse) {
  const BimSurfacePassPlan plan = activePlan();
  BimSurfaceRasterPassRecordInputs inputs = requiredInputs(plan);
  inputs.extent.height = 0u;

  EXPECT_FALSE(recordBimSurfaceRasterPassCommands(
      fakeHandle<VkCommandBuffer>(0x3), inputs));
}

TEST(BimSurfaceRasterPassRecorderTests, SemanticColorModeCopiesWhenWritten) {
  BimSurfacePassPlan plan = activePlan();
  plan.writesSemanticColorMode = true;
  plan.semanticColorMode = 7u;
  container::gpu::BindlessPushConstants pushConstants{};
  pushConstants.semanticColorMode = 3u;

  const container::gpu::BindlessPushConstants result =
      bimSurfaceRasterPassPushConstants(pushConstants, plan);

  EXPECT_EQ(result.semanticColorMode, 7u);
}

TEST(BimSurfaceRasterPassRecorderTests, SemanticColorModeIsPreservedWhenUnset) {
  BimSurfacePassPlan plan = activePlan();
  plan.writesSemanticColorMode = false;
  plan.semanticColorMode = 7u;
  container::gpu::BindlessPushConstants pushConstants{};
  pushConstants.semanticColorMode = 3u;

  const container::gpu::BindlessPushConstants result =
      bimSurfaceRasterPassPushConstants(pushConstants, plan);

  EXPECT_EQ(result.semanticColorMode, 3u);
}

TEST(BimSurfaceRasterPassRecorderTests,
     BuildsFrameSurfacePassInputsFromCompactSources) {
  const std::vector<DrawCommand> meshCommands{
      DrawCommand{.objectIndex = 1u, .firstIndex = 0u, .indexCount = 3u}};
  const std::array<VkDescriptorSet, 1> descriptorSets = {
      fakeHandle<VkDescriptorSet>(0x21)};
  container::gpu::BindlessPushConstants pushConstants{};
  const BimSurfaceFramePassRecordInputs inputs =
      frameInputs(meshCommands, pushConstants, descriptorSets);

  const auto planInputs = buildBimSurfaceFramePassInputs(inputs);

  EXPECT_EQ(planInputs.kind, BimSurfacePassKind::GBuffer);
  EXPECT_TRUE(planInputs.passReady);
  EXPECT_TRUE(planInputs.geometryReady);
  EXPECT_TRUE(planInputs.descriptorSetReady);
  EXPECT_TRUE(planInputs.bindlessPushConstantsReady);
  EXPECT_TRUE(planInputs.basePipelineReady);
  EXPECT_EQ(planInputs.semanticColorMode, 4u);
  ASSERT_EQ(planInputs.sourceCount, 3u);
  EXPECT_TRUE(planInputs.sources[0].gpuCompactionEligible);
  EXPECT_TRUE(planInputs.sources[0].gpuVisibilityOwnsCpuFallback);
  EXPECT_EQ(planInputs.sources[0].draws.opaqueSingleSidedDrawCommands,
            &meshCommands);
  EXPECT_FALSE(planInputs.sources[1].gpuCompactionEligible);
  EXPECT_FALSE(planInputs.sources[2].gpuCompactionEligible);
}

TEST(BimSurfaceRasterPassRecorderTests,
     TransparentFrameSurfacePassUsesTransparentGpuVisibilityFlag) {
  const std::vector<DrawCommand> meshCommands{
      DrawCommand{.objectIndex = 2u, .firstIndex = 0u, .indexCount = 3u}};
  const std::array<VkDescriptorSet, 1> descriptorSets = {
      fakeHandle<VkDescriptorSet>(0x22)};
  container::gpu::BindlessPushConstants pushConstants{};
  BimSurfaceFramePassRecordInputs inputs =
      frameInputs(meshCommands, pushConstants, descriptorSets);
  inputs.kind = BimSurfacePassKind::TransparentLighting;
  inputs.draws.opaqueMeshDrawsUseGpuVisibility = false;
  inputs.draws.transparentMeshDrawsUseGpuVisibility = true;

  const auto planInputs = buildBimSurfaceFramePassInputs(inputs);

  ASSERT_GE(planInputs.sourceCount, 1u);
  EXPECT_TRUE(planInputs.sources[0].gpuVisibilityOwnsCpuFallback);
}

TEST(BimSurfaceRasterPassRecorderTests,
     CpuFilteredMeshSourcesDoNotAllowGpuCompaction) {
  const std::vector<DrawCommand> meshCommands{
      DrawCommand{.objectIndex = 3u, .firstIndex = 0u, .indexCount = 3u}};
  const std::array<VkDescriptorSet, 1> descriptorSets = {
      fakeHandle<VkDescriptorSet>(0x23)};
  container::gpu::BindlessPushConstants pushConstants{};
  BimSurfaceFramePassRecordInputs inputs =
      frameInputs(meshCommands, pushConstants, descriptorSets);
  inputs.draws.opaqueMeshDrawsUseGpuVisibility = false;

  const auto planInputs = buildBimSurfaceFramePassInputs(inputs);

  ASSERT_GE(planInputs.sourceCount, 1u);
  EXPECT_FALSE(planInputs.sources[0].gpuCompactionEligible);
  EXPECT_FALSE(planInputs.sources[0].gpuVisibilityOwnsCpuFallback);
}

TEST(BimSurfaceRasterPassRecorderTests,
     BuildsFrameBindingWithoutOwningDescriptorStorage) {
  const std::vector<DrawCommand> meshCommands{
      DrawCommand{.objectIndex = 4u, .firstIndex = 0u, .indexCount = 3u}};
  const std::vector<DrawCommand> pointCommands{
      DrawCommand{.objectIndex = 5u, .firstIndex = 0u, .indexCount = 6u}};
  const std::vector<DrawCommand> curveCommands{
      DrawCommand{.objectIndex = 6u, .firstIndex = 0u, .indexCount = 9u}};
  const std::array<VkDescriptorSet, 1> descriptorSets = {
      fakeHandle<VkDescriptorSet>(0x26)};

  const auto binding = buildBimSurfaceFrameBinding(
      BimSurfaceFrameBindingInputs{
          .draws = {.mesh = {.opaqueSingleSidedDrawCommands = &meshCommands},
                    .pointPlaceholders = {
                        .opaqueSingleSidedDrawCommands = &pointCommands},
                    .curvePlaceholders = {
                        .opaqueSingleSidedDrawCommands = &curveCommands},
                    .opaqueMeshDrawsUseGpuVisibility = true,
                    .transparentMeshDrawsUseGpuVisibility = true},
          .vertexSlice = {.buffer = fakeHandle<VkBuffer>(0x27), .offset = 8u},
          .indexSlice = {.buffer = fakeHandle<VkBuffer>(0x28), .offset = 16u},
          .indexType = VK_INDEX_TYPE_UINT16,
          .descriptorSets = descriptorSets,
          .semanticColorMode = 9u});

  EXPECT_EQ(binding.draws.mesh.opaqueSingleSidedDrawCommands, &meshCommands);
  EXPECT_EQ(binding.draws.pointPlaceholders.opaqueSingleSidedDrawCommands,
            &pointCommands);
  EXPECT_EQ(binding.draws.curvePlaceholders.opaqueSingleSidedDrawCommands,
            &curveCommands);
  EXPECT_TRUE(binding.draws.opaqueMeshDrawsUseGpuVisibility);
  EXPECT_TRUE(binding.draws.transparentMeshDrawsUseGpuVisibility);
  ASSERT_EQ(binding.geometry.descriptorSets.size(), descriptorSets.size());
  EXPECT_EQ(binding.geometry.descriptorSets.data(), descriptorSets.data());
  EXPECT_EQ(binding.geometry.vertexSlice.buffer, fakeHandle<VkBuffer>(0x27));
  EXPECT_EQ(binding.geometry.vertexSlice.offset, 8u);
  EXPECT_EQ(binding.geometry.indexSlice.buffer, fakeHandle<VkBuffer>(0x28));
  EXPECT_EQ(binding.geometry.indexSlice.offset, 16u);
  EXPECT_EQ(binding.geometry.indexType, VK_INDEX_TYPE_UINT16);
  EXPECT_EQ(binding.semanticColorMode, 9u);
}

TEST(BimSurfaceRasterPassRecorderTests,
     BuildsFrameSurfacePassPlanThroughBimOwnedFacade) {
  const std::vector<DrawCommand> meshCommands{
      DrawCommand{.objectIndex = 4u, .firstIndex = 0u, .indexCount = 3u}};
  const std::array<VkDescriptorSet, 1> descriptorSets = {
      fakeHandle<VkDescriptorSet>(0x25)};
  container::gpu::BindlessPushConstants pushConstants{};
  const BimSurfaceFramePassRecordInputs inputs =
      frameInputs(meshCommands, pushConstants, descriptorSets);

  const BimSurfacePassPlan plan = buildBimSurfaceFramePassPlan(inputs);

  EXPECT_TRUE(plan.active);
  EXPECT_TRUE(plan.writesSemanticColorMode);
  EXPECT_EQ(plan.semanticColorMode, 4u);
  ASSERT_GE(plan.sourceCount, 1u);
  ASSERT_GE(plan.sources[0].routeCount, 1u);
  EXPECT_TRUE(plan.sources[0].routes[0].gpuCompactionAllowed);
  EXPECT_FALSE(plan.sources[0].routes[0].cpuFallbackAllowed);
}

TEST(BimSurfaceRasterPassRecorderTests,
     FrameSurfacePassRejectsMissingPushConstantsBeforeRecording) {
  const std::vector<DrawCommand> meshCommands{
      DrawCommand{.objectIndex = 3u, .firstIndex = 0u, .indexCount = 3u}};
  const std::array<VkDescriptorSet, 1> descriptorSets = {
      fakeHandle<VkDescriptorSet>(0x23)};
  container::gpu::BindlessPushConstants pushConstants{};
  BimSurfaceFramePassRecordInputs inputs =
      frameInputs(meshCommands, pushConstants, descriptorSets);
  inputs.pushConstants = nullptr;

  EXPECT_FALSE(recordBimSurfaceFramePassCommands(
      fakeHandle<VkCommandBuffer>(0x24), inputs));
}
