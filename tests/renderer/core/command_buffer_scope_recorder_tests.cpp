#include "Container/renderer/core/CommandBufferScopeRecorder.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using container::renderer::CommandBufferBeginRecordInputs;
using container::renderer::SecondaryCommandBufferBeginRecordInputs;
using container::renderer::recordCommandBufferBeginCommands;
using container::renderer::recordCommandBufferEndCommands;
using container::renderer::recordCommandBufferResetCommands;
using container::renderer::recordSecondaryCommandBufferBeginCommands;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

SecondaryCommandBufferBeginRecordInputs requiredSecondaryInputs() {
  return {.renderPass = fakeHandle<RenderingPassHandle>(0x1),
          .framebuffer = fakeHandle<RenderingTargetHandle>(0x2)};
}

} // namespace

TEST(CommandBufferScopeRecorderTests, ResetMissingCommandBufferReturnsFalse) {
  EXPECT_FALSE(recordCommandBufferResetCommands(VK_NULL_HANDLE));
}

TEST(CommandBufferScopeRecorderTests, BeginMissingCommandBufferReturnsFalse) {
  EXPECT_FALSE(
      recordCommandBufferBeginCommands(VK_NULL_HANDLE,
                                       CommandBufferBeginRecordInputs{}));
}

TEST(CommandBufferScopeRecorderTests,
     SecondaryBeginMissingCommandBufferReturnsFalse) {
  EXPECT_FALSE(recordSecondaryCommandBufferBeginCommands(
      VK_NULL_HANDLE, requiredSecondaryInputs()));
}

TEST(CommandBufferScopeRecorderTests,
     SecondaryBeginMissingRenderPassReturnsFalse) {
  SecondaryCommandBufferBeginRecordInputs inputs = requiredSecondaryInputs();
  inputs.renderPass = VK_NULL_HANDLE;

  EXPECT_FALSE(recordSecondaryCommandBufferBeginCommands(
      fakeHandle<VkCommandBuffer>(0x3), inputs));
}

TEST(CommandBufferScopeRecorderTests,
     SecondaryBeginMissingFramebufferReturnsFalse) {
  SecondaryCommandBufferBeginRecordInputs inputs = requiredSecondaryInputs();
  inputs.framebuffer = VK_NULL_HANDLE;

  EXPECT_FALSE(recordSecondaryCommandBufferBeginCommands(
      fakeHandle<VkCommandBuffer>(0x3), inputs));
}

TEST(CommandBufferScopeRecorderTests, EndMissingCommandBufferReturnsFalse) {
  EXPECT_FALSE(recordCommandBufferEndCommands(VK_NULL_HANDLE));
}
