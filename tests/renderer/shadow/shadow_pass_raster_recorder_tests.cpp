#include "Container/renderer/shadow/ShadowPassRasterRecorder.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using container::renderer::ShadowPassRasterPlan;
using container::renderer::ShadowPassRasterPlanInputs;
using container::renderer::buildShadowPassRasterPlan;
using container::renderer::recordShadowPassRasterCommands;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

ShadowPassRasterPlan activeInlinePlan() {
  return buildShadowPassRasterPlan(
      {.shadowAtlasVisible = true, .shadowPassRecordable = true});
}

} // namespace

TEST(ShadowPassRasterRecorderTests, NullCommandBufferReturnsFalse) {
  const ShadowPassRasterPlan plan = activeInlinePlan();

  EXPECT_FALSE(recordShadowPassRasterCommands(
      VK_NULL_HANDLE, {.plan = &plan,
                       .renderPass = fakeHandle<RenderingPassHandle>(0x1),
                       .framebuffer = fakeHandle<RenderingTargetHandle>(0x2),
                       .recordBody = [](VkCommandBuffer) {}}));
}

TEST(ShadowPassRasterRecorderTests, NullPlanReturnsFalse) {
  EXPECT_FALSE(recordShadowPassRasterCommands(
      fakeHandle<VkCommandBuffer>(0x3),
      {.renderPass = fakeHandle<RenderingPassHandle>(0x1),
       .framebuffer = fakeHandle<RenderingTargetHandle>(0x2),
       .recordBody = [](VkCommandBuffer) {}}));
}

TEST(ShadowPassRasterRecorderTests, InactivePlanDoesNotInvokeCallback) {
  bool invoked = false;
  const ShadowPassRasterPlan plan{};

  EXPECT_FALSE(recordShadowPassRasterCommands(
      fakeHandle<VkCommandBuffer>(0x3),
      {.plan = &plan,
       .renderPass = fakeHandle<RenderingPassHandle>(0x1),
       .framebuffer = fakeHandle<RenderingTargetHandle>(0x2),
       .recordBody = [&invoked](VkCommandBuffer) { invoked = true; }}));
  EXPECT_FALSE(invoked);
}

TEST(ShadowPassRasterRecorderTests, MissingRenderPassOrFramebufferReturnsFalse) {
  const ShadowPassRasterPlan plan = activeInlinePlan();

  EXPECT_FALSE(recordShadowPassRasterCommands(
      fakeHandle<VkCommandBuffer>(0x3),
      {.plan = &plan,
       .framebuffer = fakeHandle<RenderingTargetHandle>(0x2),
       .recordBody = [](VkCommandBuffer) {}}));
  EXPECT_FALSE(recordShadowPassRasterCommands(
      fakeHandle<VkCommandBuffer>(0x3),
      {.plan = &plan,
       .renderPass = fakeHandle<RenderingPassHandle>(0x1),
       .recordBody = [](VkCommandBuffer) {}}));
}

TEST(ShadowPassRasterRecorderTests, InlinePlanRequiresCallback) {
  const ShadowPassRasterPlan plan = activeInlinePlan();

  EXPECT_FALSE(recordShadowPassRasterCommands(
      fakeHandle<VkCommandBuffer>(0x3),
      {.plan = &plan,
       .renderPass = fakeHandle<RenderingPassHandle>(0x1),
       .framebuffer = fakeHandle<RenderingTargetHandle>(0x2)}));
}
