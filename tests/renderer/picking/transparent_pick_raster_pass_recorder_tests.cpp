#include "Container/renderer/picking/TransparentPickRasterPassRecorder.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using container::renderer::TransparentPickRasterPassRecordInputs;
using container::renderer::TransparentPickFramePassRecordInputs;
using container::renderer::recordTransparentPickFramePassCommands;
using container::renderer::recordTransparentPickRasterPassCommands;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

TransparentPickRasterPassRecordInputs requiredInputs() {
  return {.active = true,
          .renderPass = fakeHandle<RenderingPassHandle>(0x1),
          .framebuffer = fakeHandle<RenderingTargetHandle>(0x2),
          .extent = {640u, 480u}};
}

TransparentPickFramePassRecordInputs requiredFrameInputs() {
  static container::gpu::BindlessPushConstants pushConstants{};
  return {.renderPass = fakeHandle<RenderingPassHandle>(0x1),
          .framebuffer = fakeHandle<RenderingTargetHandle>(0x2),
          .extent = {640u, 480u},
          .sourceDepthStencilImage = fakeHandle<VkImage>(0x3),
          .pickDepthImage = fakeHandle<VkImage>(0x4),
          .pickIdImage = fakeHandle<VkImage>(0x5),
          .pipelines = {.primary = fakeHandle<VkPipeline>(0x6)},
          .pipelineLayout = fakeHandle<VkPipelineLayout>(0x7),
          .pushConstants = &pushConstants,
          .debugOverlay =
              fakeHandle<const container::renderer::DebugOverlayRenderer *>(
                  0x9)};
}

} // namespace

TEST(TransparentPickRasterPassRecorderTests, InactivePassReturnsFalse) {
  TransparentPickRasterPassRecordInputs inputs = requiredInputs();
  inputs.active = false;

  EXPECT_FALSE(recordTransparentPickRasterPassCommands(
      fakeHandle<VkCommandBuffer>(0x3), inputs));
}

TEST(TransparentPickRasterPassRecorderTests, MissingCommandBufferReturnsFalse) {
  EXPECT_FALSE(
      recordTransparentPickRasterPassCommands(VK_NULL_HANDLE, requiredInputs()));
}

TEST(TransparentPickRasterPassRecorderTests, MissingRenderPassReturnsFalse) {
  TransparentPickRasterPassRecordInputs inputs = requiredInputs();
  inputs.renderPass = VK_NULL_HANDLE;

  EXPECT_FALSE(recordTransparentPickRasterPassCommands(
      fakeHandle<VkCommandBuffer>(0x3), inputs));
}

TEST(TransparentPickRasterPassRecorderTests, MissingFramebufferReturnsFalse) {
  TransparentPickRasterPassRecordInputs inputs = requiredInputs();
  inputs.framebuffer = VK_NULL_HANDLE;

  EXPECT_FALSE(recordTransparentPickRasterPassCommands(
      fakeHandle<VkCommandBuffer>(0x3), inputs));
}

TEST(TransparentPickRasterPassRecorderTests, ZeroWidthReturnsFalse) {
  TransparentPickRasterPassRecordInputs inputs = requiredInputs();
  inputs.extent.width = 0u;

  EXPECT_FALSE(recordTransparentPickRasterPassCommands(
      fakeHandle<VkCommandBuffer>(0x3), inputs));
}

TEST(TransparentPickRasterPassRecorderTests, ZeroHeightReturnsFalse) {
  TransparentPickRasterPassRecordInputs inputs = requiredInputs();
  inputs.extent.height = 0u;

  EXPECT_FALSE(recordTransparentPickRasterPassCommands(
      fakeHandle<VkCommandBuffer>(0x3), inputs));
}

TEST(TransparentPickRasterPassRecorderTests,
     FramePassMissingDepthSourceReturnsFalse) {
  TransparentPickFramePassRecordInputs inputs = requiredFrameInputs();
  inputs.sourceDepthStencilImage = VK_NULL_HANDLE;

  EXPECT_FALSE(recordTransparentPickFramePassCommands(
      fakeHandle<VkCommandBuffer>(0x8), inputs));
}

TEST(TransparentPickRasterPassRecorderTests,
     FramePassWithoutSceneOrBimWorkReturnsFalse) {
  EXPECT_FALSE(recordTransparentPickFramePassCommands(
      fakeHandle<VkCommandBuffer>(0x8), requiredFrameInputs()));
}
