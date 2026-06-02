#include "Container/renderer/shadow/ShadowCullPassRecorder.h"
#include "Container/renderer/shadow/ShadowCullManager.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using container::renderer::RenderPassReadiness;
using container::renderer::DrawCommand;
using container::renderer::ShadowCullPassPlan;
using container::renderer::ShadowCullPassRecordInputs;
using container::renderer::recordShadowCullPassCommands;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

ShadowCullPassPlan activePlan() {
  return {.active = true, .readiness = RenderPassReadiness{}, .drawCount = 4u};
}

std::filesystem::path repoRoot() {
  for (auto path = std::filesystem::current_path(); !path.empty();) {
    if (std::filesystem::exists(
            path / "src/renderer/shadow/ShadowCullPassRecorder.cpp")) {
      return path;
    }

    const auto parent = path.parent_path();
    if (parent == path) {
      break;
    }
    path = parent;
  }
  return {};
}

std::string readRepoTextFile(const std::filesystem::path &relativePath) {
  const auto root = repoRoot();
  if (root.empty()) {
    return {};
  }

  std::ifstream file(root / relativePath, std::ios::binary);
  std::ostringstream text;
  text << file.rdbuf();
  return text.str();
}

} // namespace

TEST(ShadowCullPassRecorderTests, NullCommandBufferReturnsFalse) {
  EXPECT_FALSE(recordShadowCullPassCommands(
      VK_NULL_HANDLE, {.shadowCullManager =
                           reinterpret_cast<container::renderer::ShadowCullManager *>(0x1),
                       .plan = activePlan()}));
}

TEST(ShadowCullPassRecorderTests, NullManagerReturnsFalse) {
  EXPECT_FALSE(recordShadowCullPassCommands(
      fakeHandle<VkCommandBuffer>(0x2), {.plan = activePlan()}));
}

TEST(ShadowCullPassRecorderTests, InactivePlanReturnsFalse) {
  EXPECT_FALSE(recordShadowCullPassCommands(
      fakeHandle<VkCommandBuffer>(0x2),
      {.shadowCullManager =
           reinterpret_cast<container::renderer::ShadowCullManager *>(0x1),
       .plan = {}}));
}

TEST(ShadowCullPassRecorderTests, NotReadyPlanReturnsFalse) {
  ShadowCullPassPlan plan = activePlan();
  plan.readiness.ready = false;

  EXPECT_FALSE(recordShadowCullPassCommands(
      fakeHandle<VkCommandBuffer>(0x2),
      {.shadowCullManager =
           reinterpret_cast<container::renderer::ShadowCullManager *>(0x1),
       .plan = plan}));
}

TEST(ShadowCullPassRecorderTests, ZeroDrawCountReturnsFalse) {
  ShadowCullPassPlan plan = activePlan();
  plan.drawCount = 0u;

  EXPECT_FALSE(recordShadowCullPassCommands(
      fakeHandle<VkCommandBuffer>(0x2),
      {.shadowCullManager =
           reinterpret_cast<container::renderer::ShadowCullManager *>(0x1),
       .plan = plan}));
}

TEST(ShadowCullPassRecorderTests,
     ShadowCullManagerDrawBuffersAreImageScopedByContract) {
  using ExpectedUploadSignature =
      void (container::renderer::ShadowCullManager::*)(
          uint32_t, const std::vector<DrawCommand>&, uint64_t);
  using ExpectedIndirectBufferSignature =
      VkBuffer (container::renderer::ShadowCullManager::*)(
          uint32_t, uint32_t) const;

  EXPECT_TRUE((std::is_same_v<
               decltype(&container::renderer::ShadowCullManager::
                            uploadDrawCommands),
               ExpectedUploadSignature>));
  EXPECT_TRUE((std::is_same_v<
               decltype(&container::renderer::ShadowCullManager::
                            indirectDrawBuffer),
               ExpectedIndirectBufferSignature>));
  EXPECT_TRUE((std::is_same_v<
               decltype(&container::renderer::ShadowCullManager::
                            drawCountBuffer),
               ExpectedIndirectBufferSignature>));
}

TEST(ShadowCullPassRecorderTests,
     DispatchCascadeCullReturnsWhetherItSubmittedWork) {
  using ExpectedSignature = bool (container::renderer::ShadowCullManager::*)(
      VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t);

  EXPECT_TRUE((std::is_same_v<
               decltype(&container::renderer::ShadowCullManager::
                            dispatchCascadeCull),
               ExpectedSignature>));
}

TEST(ShadowCullPassRecorderTests,
     RecorderReturnsManagerDispatchResultByContract) {
  const std::string recorder =
      readRepoTextFile("src/renderer/shadow/ShadowCullPassRecorder.cpp");
  ASSERT_FALSE(recorder.empty());

  EXPECT_NE(recorder.find(
                 "return inputs.shadowCullManager->dispatchCascadeCull("),
            std::string::npos);
  EXPECT_NE(recorder.find(
                "cmd, inputs.imageIndex, inputs.cascadeIndex"),
            std::string::npos);
}

TEST(ShadowCullPassRecorderTests,
     DispatchReadinessChecksDescriptorSetBeforeGpuRouteByContract) {
  const std::string manager =
      readRepoTextFile("src/renderer/shadow/ShadowCullManager.cpp");
  ASSERT_FALSE(manager.empty());

  const auto readiness = manager.find(
      "bool ShadowCullManager::canDispatchCascadeCull");
  const auto descriptorNoOp = manager.find(
      "shadowCullSets_[setIndex] != VK_NULL_HANDLE", readiness);
  const auto dispatch = manager.find(
      "bool ShadowCullManager::dispatchCascadeCull");
  const auto readinessUse = manager.find(
      "if (!canDispatchCascadeCull(imageIndex, cascadeIndex))", dispatch);
  const auto fill =
      manager.find("vkCmdFillBuffer(cmd, drawCountBuffer.buffer", dispatch);

  ASSERT_NE(readiness, std::string::npos);
  ASSERT_NE(descriptorNoOp, std::string::npos);
  ASSERT_NE(dispatch, std::string::npos);
  ASSERT_NE(readinessUse, std::string::npos);
  ASSERT_NE(fill, std::string::npos);
  EXPECT_LT(readinessUse, fill);
}

TEST(ShadowCullPassRecorderTests,
     ShadowCullShaderPreservesInputDrawOrderByContract) {
  const std::string shader = readRepoTextFile("shaders/shadow_cull.slang");
  ASSERT_FALSE(shader.empty());

  EXPECT_EQ(shader.find("InterlockedAdd"), std::string::npos);
  EXPECT_NE(shader.find("uDrawCount[0] = pc.drawCount"), std::string::npos);
  EXPECT_NE(shader.find("uOutputDraws[pc.outputOffset + idx]"),
            std::string::npos);
  EXPECT_NE(shader.find("outputDraw.instanceCount = 0"),
            std::string::npos);
}
