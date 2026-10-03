#include "Container/renderer/core/RenderPassScopeRecorder.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {

using container::renderer::RenderPassScopeRecordInputs;
using container::renderer::recordRenderPassBeginCommands;
using container::renderer::recordRenderPassEndCommands;
using container::renderer::recordRenderPassExecuteSecondaryCommands;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

RenderPassScopeRecordInputs requiredInputs() {
  static const std::array<VkClearValue, 1> clearValues = {VkClearValue{}};
  return {.renderPass = fakeHandle<RenderingPassHandle>(0x1),
          .framebuffer = fakeHandle<RenderingTargetHandle>(0x2),
          .renderArea = {.offset = {0, 0}, .extent = {640u, 480u}},
          .clearValues = clearValues};
}

} // namespace

TEST(RenderPassScopeRecorderTests, BeginMissingCommandBufferReturnsFalse) {
  EXPECT_FALSE(recordRenderPassBeginCommands(VK_NULL_HANDLE, requiredInputs()));
}

TEST(RenderPassScopeRecorderTests, BeginMissingRenderPassReturnsFalse) {
  RenderPassScopeRecordInputs inputs = requiredInputs();
  inputs.renderPass = VK_NULL_HANDLE;

  EXPECT_FALSE(recordRenderPassBeginCommands(
      fakeHandle<VkCommandBuffer>(0x3), inputs));
}

TEST(RenderPassScopeRecorderTests, BeginMissingFramebufferReturnsFalse) {
  RenderPassScopeRecordInputs inputs = requiredInputs();
  inputs.framebuffer = VK_NULL_HANDLE;

  EXPECT_FALSE(recordRenderPassBeginCommands(
      fakeHandle<VkCommandBuffer>(0x3), inputs));
}

TEST(RenderPassScopeRecorderTests, BeginZeroWidthReturnsFalse) {
  RenderPassScopeRecordInputs inputs = requiredInputs();
  inputs.renderArea.extent.width = 0u;

  EXPECT_FALSE(recordRenderPassBeginCommands(
      fakeHandle<VkCommandBuffer>(0x3), inputs));
}

TEST(RenderPassScopeRecorderTests, BeginZeroHeightReturnsFalse) {
  RenderPassScopeRecordInputs inputs = requiredInputs();
  inputs.renderArea.extent.height = 0u;

  EXPECT_FALSE(recordRenderPassBeginCommands(
      fakeHandle<VkCommandBuffer>(0x3), inputs));
}

TEST(RenderPassScopeRecorderTests, ExecuteMissingCommandBufferReturnsFalse) {
  const std::array<VkCommandBuffer, 1> secondaryCommands = {
      fakeHandle<VkCommandBuffer>(0x4)};

  EXPECT_FALSE(recordRenderPassExecuteSecondaryCommands(VK_NULL_HANDLE,
                                                       secondaryCommands));
}

TEST(RenderPassScopeRecorderTests, ExecuteEmptySecondaryListReturnsFalse) {
  EXPECT_FALSE(recordRenderPassExecuteSecondaryCommands(
      fakeHandle<VkCommandBuffer>(0x3), {}));
}

TEST(RenderPassScopeRecorderTests, ExecuteNullSecondaryReturnsFalse) {
  const std::array<VkCommandBuffer, 1> secondaryCommands = {VK_NULL_HANDLE};

  EXPECT_FALSE(recordRenderPassExecuteSecondaryCommands(
      fakeHandle<VkCommandBuffer>(0x3), secondaryCommands));
}

TEST(RenderPassScopeRecorderTests, EndMissingCommandBufferReturnsFalse) {
  EXPECT_FALSE(recordRenderPassEndCommands(VK_NULL_HANDLE));
}
