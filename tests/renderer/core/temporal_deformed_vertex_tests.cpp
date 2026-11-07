#include "Container/common/CommonMath.h"
#include "Container/renderer/temporal/TemporalConventions.h"
#include "Container/renderer/temporal/TemporalVelocityVertexInput.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {
using container::gpu::BufferSlice;
using namespace container::renderer;

template<class Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

struct VertexBindingCall {
  VkCommandBuffer command{};
  uint32_t firstBinding{};
  uint32_t count{};
  VkBuffer buffer{};
  VkDeviceSize offset{};
};
std::vector<VertexBindingCall> bindingCalls;

VKAPI_ATTR void VKAPI_CALL captureVertexBinding(
    VkCommandBuffer command, uint32_t firstBinding, uint32_t count,
    const VkBuffer* buffers, const VkDeviceSize* offsets) {
  bindingCalls.push_back(
      {command, firstBinding, count, buffers[0], offsets[0]});
}

glm::mat4 testProjection() {
  return container::math::perspectiveRH_ReverseZ(
      glm::radians(90.0f), 1.0f, 1.0f, 11.0f);
}
} // namespace

TEST(TemporalDeformedVertexInput, MatchesCurrentAndPreviousPackedVertexAbi) {
  const auto bindings = temporalVelocityVertexBindings();
  ASSERT_EQ(bindings.size(), 2u);
  EXPECT_EQ(bindings[0].binding, 0u);
  EXPECT_EQ(bindings[1].binding, 1u);
  for (const auto& binding : bindings) {
    EXPECT_EQ(binding.stride, 72u);
    EXPECT_EQ(binding.inputRate, VK_VERTEX_INPUT_RATE_VERTEX);
  }
  const auto attributes = temporalVelocityVertexAttributes();
  ASSERT_EQ(attributes.size(), 7u);
  EXPECT_EQ(attributes[4].location, 6u);
  EXPECT_EQ(attributes[4].binding, 1u);
  EXPECT_EQ(attributes[4].offset, 0u);
  EXPECT_EQ(attributes[4].format, VK_FORMAT_R32G32B32_SFLOAT);
  EXPECT_EQ(attributes[5].location, 7u);
  EXPECT_EQ(attributes[5].binding, 1u);
  EXPECT_EQ(attributes[5].offset, 40u);
  EXPECT_EQ(attributes[5].format, VK_FORMAT_R32G32B32_SFLOAT);
  for (size_t i = 0; i < 4; ++i)
    EXPECT_EQ(attributes[i].binding, 0u);
  EXPECT_EQ(attributes[6].location, 8u);
  EXPECT_EQ(attributes[6].binding, 0u);
  EXPECT_EQ(attributes[6].offset, 68u);
  EXPECT_EQ(attributes[6].format, VK_FORMAT_R32_SFLOAT);
}

TEST(TemporalDeformedVertexInput, BindsSubmittedDeformationWithItsOwnOffset) {
  bindingCalls.clear();
  const auto command = fakeHandle<VkCommandBuffer>(1);
  const BufferSlice current{fakeHandle<VkBuffer>(2), 68, 680};
  const BufferSlice previous{fakeHandle<VkBuffer>(3), 136, 680};
  ASSERT_TRUE(recordTemporalPreviousVertexBinding(
      command, current, previous, captureVertexBinding));
  ASSERT_EQ(bindingCalls.size(), 1u);
  EXPECT_EQ(bindingCalls[0].command, command);
  EXPECT_EQ(bindingCalls[0].firstBinding, 1u);
  EXPECT_EQ(bindingCalls[0].count, 1u);
  EXPECT_EQ(bindingCalls[0].buffer, previous.buffer);
  EXPECT_EQ(bindingCalls[0].offset, previous.offset);
}

TEST(TemporalDeformedVertexInput, StaticAndBimBindingsDoNotInheritPrimaryHistory) {
  bindingCalls.clear();
  const auto command = fakeHandle<VkCommandBuffer>(1);
  const BufferSlice primary{fakeHandle<VkBuffer>(2), 68, 680};
  const BufferSlice previous{fakeHandle<VkBuffer>(3), 136, 680};
  const BufferSlice bim{fakeHandle<VkBuffer>(4), 204, 1360};
  ASSERT_TRUE(recordTemporalPreviousVertexBinding(
      command, primary, previous, captureVertexBinding));
  ASSERT_TRUE(recordTemporalPreviousVertexBinding(
      command, bim, {}, captureVertexBinding));
  ASSERT_TRUE(recordTemporalPreviousVertexBinding(
      command, primary, {}, captureVertexBinding));
  ASSERT_EQ(bindingCalls.size(), 3u);
  EXPECT_EQ(bindingCalls[1].buffer, bim.buffer);
  EXPECT_EQ(bindingCalls[1].offset, bim.offset);
  EXPECT_EQ(bindingCalls[2].buffer, primary.buffer);
  EXPECT_EQ(bindingCalls[2].offset, primary.offset);
  for (const auto& binding : bindingCalls) {
    EXPECT_EQ(binding.firstBinding, 1u);
    EXPECT_EQ(binding.count, 1u);
  }
}

TEST(TemporalDeformedVertexInput, MissingStreamDoesNotRecordNullVertexBinding) {
  bindingCalls.clear();
  const auto command = fakeHandle<VkCommandBuffer>(1);
  const BufferSlice current{fakeHandle<VkBuffer>(2), 0, 680};
  EXPECT_FALSE(recordTemporalPreviousVertexBinding(
      command, {}, {}, captureVertexBinding));
  EXPECT_FALSE(recordTemporalPreviousVertexBinding(
      VK_NULL_HANDLE, current, {}, captureVertexBinding));
  EXPECT_FALSE(recordTemporalPreviousVertexBinding(command, current, {}, nullptr));
  EXPECT_TRUE(bindingCalls.empty());
}

TEST(TemporalDeformedMotion, LocalMotionIsVisibleWithUnchangedRigidTransforms) {
  const auto projection = testProjection();
  // At distance two, moving one unit right/up moves 1/4 of the UV extent.
  const auto current = projection * glm::vec4(0.5f, 0.25f, -2, 1);
  const auto previous = projection * glm::vec4(-0.5f, -0.25f, -2, 1);
  const auto motion = container::temporal::motionFromUnjitteredClips(
      current, previous, true);
  ASSERT_TRUE(motion.valid);
  EXPECT_NEAR(motion.uv.x, -0.25f, 1e-6f);
  EXPECT_NEAR(motion.uv.y, 0.125f, 1e-6f);
  EXPECT_NEAR(motion.previousReverseZDepth, 0.45f, 1e-6f);
}

TEST(TemporalDeformedMotion, DeformationCombinesWithPreviousObjectTransform) {
  const auto projection = testProjection();
  const auto currentModel = glm::translate(glm::mat4(1), glm::vec3(0.5f, 0, 0));
  const auto previousModel = glm::translate(glm::mat4(1), glm::vec3(-0.5f, 0, 0));
  const auto motion = container::temporal::motionFromUnjitteredClips(
      projection * currentModel * glm::vec4(0.25f, 0, -2, 1),
      projection * previousModel * glm::vec4(-0.25f, 0, -2, 1), true);
  ASSERT_TRUE(motion.valid);
  // One unit of rigid motion plus half a unit of local motion, at z=-2.
  EXPECT_NEAR(motion.uv.x, -0.375f, 1e-6f);
  EXPECT_NEAR(motion.uv.y, 0, 1e-6f);
}

TEST(TemporalDeformedMotion, PreviousNormalChangesHeightDisplacedCorrespondence) {
  const auto projection = testProjection();
  const glm::vec3 local(0, 0, -2);
  // A constant height of 1/2 and unit scale displaces along each frame's
  // corresponding local normal, even when the base position has not moved.
  const auto current = local + glm::vec3(0, 1, 0) * 0.5f;
  const auto previous = local + glm::vec3(1, 0, 0) * 0.5f;
  const auto motion = container::temporal::motionFromUnjitteredClips(
      projection * glm::vec4(current, 1),
      projection * glm::vec4(previous, 1), true);
  ASSERT_TRUE(motion.valid);
  EXPECT_NEAR(motion.uv.x, 0.125f, 1e-6f);
  EXPECT_NEAR(motion.uv.y, 0.125f, 1e-6f);
}

TEST(TemporalDeformedMotion, MaterialDiscontinuityStillRejectsPreviousSurface) {
  const auto projection = testProjection();
  const auto motion = container::temporal::motionFromUnjitteredClips(
      projection * glm::vec4(0.5f, 0, -2, 1),
      projection * glm::vec4(-0.5f, 0, -2, 1), false);
  EXPECT_FALSE(motion.valid);
  EXPECT_EQ(motion.uv, glm::vec2(0));
}
