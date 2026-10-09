#include "Container/renderer/scene/SceneRasterPassRecorder.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {

using container::renderer::SceneOpaqueDrawPlan;
using container::renderer::SceneRasterPassClearValues;
using container::renderer::SceneRasterPassKind;
using container::renderer::SceneRasterPassRecordInputs;
using container::renderer::recordSceneRasterPassCommands;
using container::renderer::sceneRasterPassClearValues;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

SceneRasterPassRecordInputs requiredInputs(const SceneOpaqueDrawPlan &plan) {
  return {.renderPass = fakeHandle<RenderingPassHandle>(0x1),
          .framebuffer = fakeHandle<RenderingTargetHandle>(0x2),
          .extent = {640u, 480u},
          .clearValues = sceneRasterPassClearValues(
              SceneRasterPassKind::DepthPrepass),
          .plan = &plan};
}

} // namespace

TEST(SceneRasterPassRecorderTests, MissingCommandBufferReturnsFalse) {
  const SceneOpaqueDrawPlan plan{};

  EXPECT_FALSE(
      recordSceneRasterPassCommands(VK_NULL_HANDLE, requiredInputs(plan)));
}

TEST(SceneRasterPassRecorderTests, MissingPlanReturnsFalse) {
  const SceneOpaqueDrawPlan plan{};
  SceneRasterPassRecordInputs inputs = requiredInputs(plan);
  inputs.plan = nullptr;

  EXPECT_FALSE(recordSceneRasterPassCommands(
      fakeHandle<VkCommandBuffer>(0x3), inputs));
}

TEST(SceneRasterPassRecorderTests, MissingRenderPassReturnsFalse) {
  const SceneOpaqueDrawPlan plan{};
  SceneRasterPassRecordInputs inputs = requiredInputs(plan);
  inputs.renderPass = VK_NULL_HANDLE;

  EXPECT_FALSE(recordSceneRasterPassCommands(
      fakeHandle<VkCommandBuffer>(0x3), inputs));
}

TEST(SceneRasterPassRecorderTests, MissingFramebufferReturnsFalse) {
  const SceneOpaqueDrawPlan plan{};
  SceneRasterPassRecordInputs inputs = requiredInputs(plan);
  inputs.framebuffer = VK_NULL_HANDLE;

  EXPECT_FALSE(recordSceneRasterPassCommands(
      fakeHandle<VkCommandBuffer>(0x3), inputs));
}

TEST(SceneRasterPassRecorderTests, ZeroWidthReturnsFalse) {
  const SceneOpaqueDrawPlan plan{};
  SceneRasterPassRecordInputs inputs = requiredInputs(plan);
  inputs.extent.width = 0u;

  EXPECT_FALSE(recordSceneRasterPassCommands(
      fakeHandle<VkCommandBuffer>(0x3), inputs));
}

TEST(SceneRasterPassRecorderTests, ZeroHeightReturnsFalse) {
  const SceneOpaqueDrawPlan plan{};
  SceneRasterPassRecordInputs inputs = requiredInputs(plan);
  inputs.extent.height = 0u;

  EXPECT_FALSE(recordSceneRasterPassCommands(
      fakeHandle<VkCommandBuffer>(0x3), inputs));
}

TEST(SceneRasterPassRecorderTests, EmptyClearValuesReturnFalse) {
  const SceneOpaqueDrawPlan plan{};
  SceneRasterPassRecordInputs inputs = requiredInputs(plan);
  inputs.clearValues = SceneRasterPassClearValues{};

  EXPECT_FALSE(recordSceneRasterPassCommands(
      fakeHandle<VkCommandBuffer>(0x3), inputs));
}

TEST(SceneRasterPassRecorderTests, DepthPrepassClearValuesMatchContract) {
  const SceneRasterPassClearValues clearValues =
      sceneRasterPassClearValues(SceneRasterPassKind::DepthPrepass);

  EXPECT_EQ(clearValues.count, 1u);
  EXPECT_FLOAT_EQ(clearValues.values[0].depthStencil.depth, 0.0f);
  EXPECT_EQ(clearValues.values[0].depthStencil.stencil, 0u);
}

TEST(SceneRasterPassRecorderTests, GBufferClearValuesMatchContract) {
  const SceneRasterPassClearValues clearValues =
      sceneRasterPassClearValues(SceneRasterPassKind::GBuffer);

  ASSERT_EQ(clearValues.count, 8u);
  EXPECT_FLOAT_EQ(clearValues.values[0].color.float32[0], 0.0f);
  EXPECT_FLOAT_EQ(clearValues.values[0].color.float32[1], 0.0f);
  EXPECT_FLOAT_EQ(clearValues.values[0].color.float32[2], 0.0f);
  EXPECT_FLOAT_EQ(clearValues.values[0].color.float32[3], 0.0f);
  EXPECT_FLOAT_EQ(clearValues.values[1].color.float32[0], 0.5f);
  EXPECT_FLOAT_EQ(clearValues.values[1].color.float32[1], 0.5f);
  EXPECT_FLOAT_EQ(clearValues.values[1].color.float32[2], 1.0f);
  EXPECT_FLOAT_EQ(clearValues.values[1].color.float32[3], 1.0f);
  EXPECT_FLOAT_EQ(clearValues.values[2].color.float32[0], 0.0f);
  EXPECT_FLOAT_EQ(clearValues.values[2].color.float32[1], 1.0f);
  EXPECT_FLOAT_EQ(clearValues.values[2].color.float32[2], 1.0f);
  EXPECT_FLOAT_EQ(clearValues.values[2].color.float32[3], 1.0f);
  EXPECT_FLOAT_EQ(clearValues.values[3].color.float32[0], 0.0f);
  EXPECT_FLOAT_EQ(clearValues.values[3].color.float32[1], 0.0f);
  EXPECT_FLOAT_EQ(clearValues.values[3].color.float32[2], 0.0f);
  EXPECT_FLOAT_EQ(clearValues.values[3].color.float32[3], 0.0f);
  EXPECT_FLOAT_EQ(clearValues.values[6].color.float32[0], 0.0f);
  EXPECT_FLOAT_EQ(clearValues.values[6].color.float32[1], 1.0f);
  EXPECT_FLOAT_EQ(clearValues.values[6].color.float32[2], 0.0f);
  EXPECT_FLOAT_EQ(clearValues.values[6].color.float32[3], 0.0f);
  EXPECT_FLOAT_EQ(clearValues.values[7].color.float32[0], 0.0f);
  EXPECT_FLOAT_EQ(clearValues.values[7].color.float32[1], 0.0f);
  EXPECT_FLOAT_EQ(clearValues.values[7].color.float32[2], 0.0f);
  EXPECT_FLOAT_EQ(clearValues.values[7].color.float32[3], 1.0f);
  EXPECT_FLOAT_EQ(clearValues.values[4].color.float32[0], 0.0f);
  EXPECT_FLOAT_EQ(clearValues.values[4].color.float32[1], 0.0f);
  EXPECT_FLOAT_EQ(clearValues.values[4].color.float32[2], 0.0f);
  EXPECT_FLOAT_EQ(clearValues.values[4].color.float32[3], 0.0f);
  EXPECT_EQ(clearValues.values[5].color.uint32[0], 0u);
  EXPECT_EQ(clearValues.values[5].color.uint32[1], 0u);
  EXPECT_EQ(clearValues.values[5].color.uint32[2], 0u);
  EXPECT_EQ(clearValues.values[5].color.uint32[3], 0u);
}
