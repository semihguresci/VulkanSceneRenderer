#include "Container/renderer/shadow/ShadowCascadeGpuCullPlanner.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using container::renderer::ShadowCascadeGpuCullPlanInputs;
using container::renderer::ShadowGpuCullSourceUploadPlanInputs;
using container::renderer::buildShadowCascadeGpuCullPlan;
using container::renderer::buildShadowGpuCullSourceUploadPlan;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

ShadowCascadeGpuCullPlanInputs readyInputs() {
  return {.gpuShadowCullEnabled = true,
          .shadowCullPassActive = true,
          .shadowCullManagerReady = true,
          .shadowCullDispatchReady = true,
          .sceneSingleSidedDrawsAvailable = true,
          .cascadeIndexInRange = true,
          .indirectDrawBuffer = fakeHandle<VkBuffer>(0x10),
          .drawCountBuffer = fakeHandle<VkBuffer>(0x20),
          .maxDrawCount = 64u};
}

} // namespace

TEST(ShadowCascadeGpuCullPlannerTests, ReadyInputsEnableGpuCull) {
  EXPECT_TRUE(buildShadowCascadeGpuCullPlan(readyInputs()).useGpuCull);
}

TEST(ShadowCascadeGpuCullPlannerTests, DisabledGpuCullSuppressesUse) {
  auto inputs = readyInputs();
  inputs.gpuShadowCullEnabled = false;

  EXPECT_FALSE(buildShadowCascadeGpuCullPlan(inputs).useGpuCull);
}

TEST(ShadowCascadeGpuCullPlannerTests,
     InactiveShadowCullPassSuppressesUse) {
  auto inputs = readyInputs();
  inputs.shadowCullPassActive = false;

  EXPECT_FALSE(buildShadowCascadeGpuCullPlan(inputs).useGpuCull);
}

TEST(ShadowCascadeGpuCullPlannerTests,
     UnreadyManagerOrMissingDrawsSuppressesUse) {
  auto unreadyManager = readyInputs();
  unreadyManager.shadowCullManagerReady = false;

  auto unreadyDispatch = readyInputs();
  unreadyDispatch.shadowCullDispatchReady = false;

  auto missingDraws = readyInputs();
  missingDraws.sceneSingleSidedDrawsAvailable = false;

  EXPECT_FALSE(buildShadowCascadeGpuCullPlan(unreadyManager).useGpuCull);
  EXPECT_FALSE(buildShadowCascadeGpuCullPlan(unreadyDispatch).useGpuCull);
  EXPECT_FALSE(buildShadowCascadeGpuCullPlan(missingDraws).useGpuCull);
}

TEST(ShadowCascadeGpuCullPlannerTests,
     OutOfRangeCascadeOrMissingBuffersSuppressesUse) {
  auto outOfRange = readyInputs();
  outOfRange.cascadeIndexInRange = false;

  auto missingIndirect = readyInputs();
  missingIndirect.indirectDrawBuffer = VK_NULL_HANDLE;

  auto missingCount = readyInputs();
  missingCount.drawCountBuffer = VK_NULL_HANDLE;

  EXPECT_FALSE(buildShadowCascadeGpuCullPlan(outOfRange).useGpuCull);
  EXPECT_FALSE(buildShadowCascadeGpuCullPlan(missingIndirect).useGpuCull);
  EXPECT_FALSE(buildShadowCascadeGpuCullPlan(missingCount).useGpuCull);
}

TEST(ShadowCascadeGpuCullPlannerTests, EmptyDrawCapacitySuppressesUse) {
  auto inputs = readyInputs();
  inputs.maxDrawCount = 0u;

  EXPECT_FALSE(buildShadowCascadeGpuCullPlan(inputs).useGpuCull);
}

TEST(ShadowCascadeGpuCullPlannerTests,
     MultiInstanceSourceDrawsSuppressGpuCull) {
  auto inputs = readyInputs();
  inputs.sourceDrawCommandsAllSingleInstance = false;

  EXPECT_FALSE(buildShadowCascadeGpuCullPlan(inputs).useGpuCull);
}

TEST(ShadowCascadeGpuCullPlannerTests,
     HiddenAtlasSuppressesSourceUpload) {
  ShadowGpuCullSourceUploadPlanInputs inputs{
      .shadowAtlasVisible = false,
      .gpuShadowCullEnabled = true,
      .shadowCullManagerReady = true,
      .sourceDrawCommandsPresent = true,
      .sourceDrawCount = 12u};

  const auto plan = buildShadowGpuCullSourceUploadPlan(inputs);

  EXPECT_FALSE(plan.uploadSourceDrawCommands);
  EXPECT_EQ(plan.requiredDrawCapacity, 0u);
}

TEST(ShadowCascadeGpuCullPlannerTests,
     DisabledGpuCullOrUnreadyManagerSuppressesSourceUpload) {
  ShadowGpuCullSourceUploadPlanInputs disabled{
      .shadowAtlasVisible = true,
      .gpuShadowCullEnabled = false,
      .shadowCullManagerReady = true,
      .sourceDrawCommandsPresent = true,
      .sourceDrawCount = 12u};
  ShadowGpuCullSourceUploadPlanInputs unready = disabled;
  unready.gpuShadowCullEnabled = true;
  unready.shadowCullManagerReady = false;

  EXPECT_FALSE(
      buildShadowGpuCullSourceUploadPlan(disabled).uploadSourceDrawCommands);
  EXPECT_FALSE(
      buildShadowGpuCullSourceUploadPlan(unready).uploadSourceDrawCommands);
}

TEST(ShadowCascadeGpuCullPlannerTests,
     MissingSourcePointerSuppressesSourceUpload) {
  ShadowGpuCullSourceUploadPlanInputs inputs{
      .shadowAtlasVisible = true,
      .gpuShadowCullEnabled = true,
      .shadowCullManagerReady = true,
      .sourceDrawCommandsPresent = false,
      .sourceDrawCount = 12u};

  EXPECT_FALSE(
      buildShadowGpuCullSourceUploadPlan(inputs).uploadSourceDrawCommands);
}

TEST(ShadowCascadeGpuCullPlannerTests,
     EmptySourceSuppressesSourceUpload) {
  ShadowGpuCullSourceUploadPlanInputs inputs{
      .shadowAtlasVisible = true,
      .gpuShadowCullEnabled = true,
      .shadowCullManagerReady = true,
      .sourceDrawCommandsPresent = true,
      .sourceDrawCount = 0u};

  const auto plan = buildShadowGpuCullSourceUploadPlan(inputs);

  EXPECT_FALSE(plan.uploadSourceDrawCommands);
  EXPECT_EQ(plan.requiredDrawCapacity, 0u);
}

TEST(ShadowCascadeGpuCullPlannerTests,
     ReadySourceUploadCarriesRequiredDrawCapacity) {
  ShadowGpuCullSourceUploadPlanInputs inputs{
      .shadowAtlasVisible = true,
      .gpuShadowCullEnabled = true,
      .shadowCullManagerReady = true,
      .sourceDrawCommandsPresent = true,
      .sourceDrawCount = 37u};

  const auto plan = buildShadowGpuCullSourceUploadPlan(inputs);

  EXPECT_TRUE(plan.uploadSourceDrawCommands);
  EXPECT_EQ(plan.requiredDrawCapacity, 37u);
}

TEST(ShadowCascadeGpuCullPlannerTests,
     MultiInstanceSourceDrawsSuppressSourceUpload) {
  ShadowGpuCullSourceUploadPlanInputs inputs{
      .shadowAtlasVisible = true,
      .gpuShadowCullEnabled = true,
      .shadowCullManagerReady = true,
      .sourceDrawCommandsPresent = true,
      .sourceDrawCommandsAllSingleInstance = false,
      .sourceDrawCount = 1u};

  const auto plan = buildShadowGpuCullSourceUploadPlan(inputs);

  EXPECT_FALSE(plan.uploadSourceDrawCommands);
  EXPECT_EQ(plan.requiredDrawCapacity, 0u);
}
