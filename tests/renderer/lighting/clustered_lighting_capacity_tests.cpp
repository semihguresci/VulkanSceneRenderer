#include <Container/renderer/lighting/LightingManager.h>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {

using container::renderer::ClusteredLightCapacityStatus;
using container::renderer::clusteredLightCapacityStatusName;
using container::renderer::planClusteredLightCapacity;

constexpr VkDeviceSize kProfileStorageRange = 128ull * 1024 * 1024;
constexpr std::array<uint32_t, 3> kDispatchLimits{65535, 65535, 65535};

TEST(ClusteredLightingCapacity, FourKExceedsThe128MiBStorageRange) {
  const auto plan = planClusteredLightCapacity(
      {3840, 2160}, kProfileStorageRange, kDispatchLimits);

  EXPECT_EQ(plan.status, ClusteredLightCapacityStatus::StorageRangeExceeded);
  EXPECT_EQ(clusteredLightCapacityStatusName(plan.status), "storageRangeExceeded");
  EXPECT_EQ(plan.storageRangeLimit, 134217728u);
  EXPECT_EQ(plan.tileCountX, 240u);
  EXPECT_EQ(plan.tileCountY, 135u);
  EXPECT_EQ(plan.clusterCount, 518400u);
  EXPECT_EQ(plan.tileGridBytes, 4147200u);
  EXPECT_EQ(plan.lightIndexListBytes, 265420800u);
}

TEST(ClusteredLightingCapacity, LargestDescriptorMayEqualTheDeviceLimit) {
  const auto atLimit = planClusteredLightCapacity(
      {2048, 2048}, kProfileStorageRange, kDispatchLimits);
  const auto belowLimit = planClusteredLightCapacity(
      {2048, 2048}, kProfileStorageRange - 1, kDispatchLimits);

  EXPECT_TRUE(atLimit.available());
  EXPECT_EQ(atLimit.lightIndexListBytes, kProfileStorageRange);
  EXPECT_EQ(belowLimit.status, ClusteredLightCapacityStatus::StorageRangeExceeded);
  EXPECT_EQ(belowLimit.lightIndexListBytes, atLimit.lightIndexListBytes);
}

TEST(ClusteredLightingCapacity, PartialTilesReserveFullShaderStrides) {
  const auto plan = planClusteredLightCapacity(
      {33, 17}, kProfileStorageRange, kDispatchLimits);

  EXPECT_TRUE(plan.available());
  EXPECT_EQ(plan.tileCountX, 3u);
  EXPECT_EQ(plan.tileCountY, 2u);
  EXPECT_EQ(plan.clusterCount, 96u);
  EXPECT_EQ(plan.tileGridBytes, 768u);
  EXPECT_EQ(plan.lightIndexListBytes, 49152u);
}

TEST(ClusteredLightingCapacity, DispatchLimitsIncludeBothTileAxesAndDepth) {
  constexpr std::array<std::array<uint32_t, 3>, 3> tooSmall{{
      {2, 2, 16}, {3, 1, 16}, {3, 2, 15}}};
  for (const auto &limits : tooSmall) {
    EXPECT_EQ(planClusteredLightCapacity({33, 17}, kProfileStorageRange, limits).status,
              ClusteredLightCapacityStatus::DispatchLimitExceeded);
  }
  EXPECT_TRUE(planClusteredLightCapacity(
                  {33, 17}, kProfileStorageRange, {3, 2, 16})
                  .available());
}

TEST(ClusteredLightingCapacity, RejectsShaderByteOffsetOverflowBeforeNarrowing) {
  constexpr VkDeviceSize kPermissiveStorageRange = 64ull * 1024 * 1024 * 1024;
  const auto exact = planClusteredLightCapacity(
      {8192, 16384}, kPermissiveStorageRange, kDispatchLimits);
  const auto overflow = planClusteredLightCapacity(
      {8192, 16385}, kPermissiveStorageRange, kDispatchLimits);
  const auto huge = planClusteredLightCapacity(
      {UINT32_MAX, UINT32_MAX}, kPermissiveStorageRange, kDispatchLimits);

  EXPECT_TRUE(exact.available());
  EXPECT_EQ(exact.lightIndexListBytes, uint64_t(UINT32_MAX) + 1u);
  EXPECT_EQ(overflow.status, ClusteredLightCapacityStatus::ShaderIndexLimitExceeded);
  EXPECT_EQ(huge.status, ClusteredLightCapacityStatus::ShaderIndexLimitExceeded);
  EXPECT_GT(huge.clusterCount, UINT32_MAX);
}

} // namespace
