#include "GeometryUploadPlan.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <limits>

namespace container::gpu::detail {
namespace {

TEST(GeometryUploadPlan, EmptyBatchesNeedNoStaging) {
  EXPECT_EQ(planGeometryUpload({}, 0).stagingBytes, 0u);
  const std::array<GeometryUploadSource, 2> sources{};
  const auto plan = planGeometryUpload(sources, 0);
  EXPECT_EQ(plan.totalBytes, 0u);
  EXPECT_EQ(plan.stagingBytes, 0u);
}

TEST(GeometryUploadPlan, OneStagingSlabServesTargetsOfDifferentSizes) {
  const std::array<std::byte, 13> vertices{};
  const std::array<std::byte, 7> indices{};
  const std::array sources{
      GeometryUploadSource{vertices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT},
      GeometryUploadSource{},
      GeometryUploadSource{indices, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT},
  };
  const auto capped = planGeometryUpload(sources, 11);
  EXPECT_EQ(capped.totalBytes, 20u);
  EXPECT_EQ(capped.stagingBytes, 8u);
  const auto large = planGeometryUpload(sources, 256);
  EXPECT_EQ(large.totalBytes, 20u);
  EXPECT_EQ(large.stagingBytes, 16u);
}

TEST(GeometryUploadPlan, RejectsInvalidUsageAndTooSmallStaging) {
  const std::array<std::byte, 4> data{};
  auto source = GeometryUploadSource{data, 0};
  const auto batch = std::span(&source, 1);
  EXPECT_THROW((void)planGeometryUpload(batch, 4), std::invalid_argument);
  source.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  EXPECT_THROW((void)planGeometryUpload(batch, 4), std::invalid_argument);
  source.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  EXPECT_THROW((void)planGeometryUpload(batch, 3), std::invalid_argument);
  EXPECT_EQ(planGeometryUpload(batch, 4).stagingBytes, 4u);
}

TEST(GeometryUploadPlan, RespectsFrameBudgetAndPadsOnlyTheFinalChunk) {
  constexpr auto sourceSize = 13u;
  constexpr auto stagingSize = 8u;
  VkDeviceSize offset = 0;
  const auto first = planGeometryUploadChunk(sourceSize, offset, stagingSize, 20);
  ASSERT_EQ(first.copyBytes, 8u);
  ASSERT_EQ(first.logicalBytes, 8u);
  offset += first.logicalBytes;

  const auto paused = planGeometryUploadChunk(sourceSize, offset, stagingSize, 3);
  EXPECT_EQ(paused.copyBytes, 0u);
  EXPECT_EQ(paused.logicalBytes, 0u);

  const auto resumed = planGeometryUploadChunk(sourceSize, offset, stagingSize, 7);
  ASSERT_EQ(resumed.copyBytes, 4u);
  ASSERT_EQ(resumed.logicalBytes, 4u);
  offset += resumed.logicalBytes;

  const auto tail = planGeometryUploadChunk(sourceSize, offset, stagingSize, 4);
  EXPECT_EQ(tail.copyBytes, 4u);
  EXPECT_EQ(tail.logicalBytes, 1u);
  offset += tail.logicalBytes;
  EXPECT_EQ(offset, sourceSize);
  EXPECT_EQ(planGeometryUploadChunk(sourceSize, offset, stagingSize, 8).copyBytes, 0u);
}

TEST(GeometryUploadPlan, PaddingRejectsUnrepresentableCopySizes) {
  constexpr auto maxBytes = (std::numeric_limits<VkDeviceSize>::max)();
  EXPECT_EQ(paddedGeometrySize(0), 0u);
  EXPECT_EQ(paddedGeometrySize(1), 4u);
  EXPECT_EQ(paddedGeometrySize(maxBytes - 3), maxBytes - 3);
  EXPECT_THROW(paddedGeometrySize(maxBytes - 2), std::overflow_error);
}

} // namespace
} // namespace container::gpu::detail
