#include "Container/renderer/culling/GpuCullDrawUploadPlanner.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using container::renderer::DrawCommand;
using container::renderer::GpuCullDrawUploadAction;
using container::renderer::GpuCullDrawUploadCacheView;
using container::renderer::GpuCullDrawUploadPlanInputs;
using container::renderer::buildGpuCullDrawUploadPlan;

GpuCullDrawUploadCacheView cacheView(
    const std::vector<const DrawCommand *> &sourceData,
    const std::vector<size_t> &sourceSizes,
    const std::vector<uint64_t> &sourceRevisions) {
  return {.sourceData = std::span<const DrawCommand *const>(
              sourceData.data(), sourceData.size()),
          .sourceSizes =
              std::span<const size_t>(sourceSizes.data(), sourceSizes.size()),
          .sourceRevisions = std::span<const uint64_t>(
              sourceRevisions.data(), sourceRevisions.size())};
}

std::vector<DrawCommand> drawCommands(size_t count) {
  std::vector<DrawCommand> commands(count);
  for (size_t i = 0; i < commands.size(); ++i) {
    commands[i] = {.objectIndex = static_cast<uint32_t>(i),
                   .firstIndex = static_cast<uint32_t>(i * 3u),
                   .indexCount = 3u};
  }
  return commands;
}

} // namespace

TEST(GpuCullDrawUploadPlannerTests,
     MissingInputBufferOrCacheSlotSkipsUpload) {
  const auto commands = drawCommands(2u);
  const std::vector<const DrawCommand *> sourceData{nullptr};
  const std::vector<size_t> sourceSizes{0u};
  const std::vector<uint64_t> sourceRevisions{0u};

  auto inputs = GpuCullDrawUploadPlanInputs{
      .inputBufferReady = false,
      .cache = cacheView(sourceData, sourceSizes, sourceRevisions),
      .imageIndex = 0u,
      .sourceData = commands.data(),
      .sourceSize = commands.size(),
      .sourceRevision = 1u,
      .maxObjectCount = 64u};

  EXPECT_EQ(buildGpuCullDrawUploadPlan(inputs).action,
            GpuCullDrawUploadAction::SkipUnavailable);

  inputs.inputBufferReady = true;
  inputs.imageIndex = 1u;
  EXPECT_EQ(buildGpuCullDrawUploadPlan(inputs).action,
            GpuCullDrawUploadAction::SkipUnavailable);
}

TEST(GpuCullDrawUploadPlannerTests, EmptyDrawStreamSkipsUpload) {
  const std::vector<DrawCommand> commands{};
  const std::vector<const DrawCommand *> sourceData{nullptr};
  const std::vector<size_t> sourceSizes{0u};
  const std::vector<uint64_t> sourceRevisions{0u};

  const auto plan = buildGpuCullDrawUploadPlan(
      {.inputBufferReady = true,
       .cache = cacheView(sourceData, sourceSizes, sourceRevisions),
       .imageIndex = 0u,
       .sourceData = commands.data(),
       .sourceSize = commands.size(),
       .sourceRevision = 1u,
       .maxObjectCount = 64u});

  EXPECT_EQ(plan.action, GpuCullDrawUploadAction::SkipUnavailable);
  EXPECT_FALSE(plan.updatesInputCount());
  EXPECT_FALSE(plan.uploadsBuffer());
}

TEST(GpuCullDrawUploadPlannerTests,
     CachedUploadReuseIsScopedToImageIndex) {
  const auto commands = drawCommands(3u);
  const std::vector<const DrawCommand *> sourceData{commands.data(), nullptr};
  const std::vector<size_t> sourceSizes{commands.size(), 0u};
  const std::vector<uint64_t> sourceRevisions{11u, 0u};
  const GpuCullDrawUploadCacheView cache =
      cacheView(sourceData, sourceSizes, sourceRevisions);

  const auto cachedImagePlan = buildGpuCullDrawUploadPlan(
      {.inputBufferReady = true,
       .cache = cache,
       .imageIndex = 0u,
       .sourceData = commands.data(),
       .sourceSize = commands.size(),
       .sourceRevision = 11u,
       .maxObjectCount = 64u});

  EXPECT_EQ(cachedImagePlan.action, GpuCullDrawUploadAction::ReuseCached);
  EXPECT_TRUE(cachedImagePlan.updatesInputCount());
  EXPECT_FALSE(cachedImagePlan.uploadsBuffer());
  EXPECT_EQ(cachedImagePlan.drawCount, commands.size());

  const auto uncachedImagePlan = buildGpuCullDrawUploadPlan(
      {.inputBufferReady = true,
       .cache = cache,
       .imageIndex = 1u,
       .sourceData = commands.data(),
       .sourceSize = commands.size(),
       .sourceRevision = 11u,
       .maxObjectCount = 64u});

  EXPECT_EQ(uncachedImagePlan.action, GpuCullDrawUploadAction::Upload);
  EXPECT_TRUE(uncachedImagePlan.uploadsBuffer());
  EXPECT_EQ(uncachedImagePlan.drawCount, commands.size());
}

TEST(GpuCullDrawUploadPlannerTests,
     RevisionChangeInvalidatesCachedUpload) {
  const auto commands = drawCommands(1u);
  const std::vector<const DrawCommand *> sourceData{commands.data()};
  const std::vector<size_t> sourceSizes{commands.size()};
  const std::vector<uint64_t> sourceRevisions{41u};

  const auto plan = buildGpuCullDrawUploadPlan(
      {.inputBufferReady = true,
       .cache = cacheView(sourceData, sourceSizes, sourceRevisions),
       .imageIndex = 0u,
       .sourceData = commands.data(),
       .sourceSize = commands.size(),
       .sourceRevision = 42u,
       .maxObjectCount = 64u});

  EXPECT_EQ(plan.action, GpuCullDrawUploadAction::Upload);
  EXPECT_TRUE(plan.uploadsBuffer());
  EXPECT_EQ(plan.drawCount, 1u);
}

TEST(GpuCullDrawUploadPlannerTests, UploadCountIsClampedToCapacity) {
  const auto commands = drawCommands(5u);
  const std::vector<const DrawCommand *> sourceData{nullptr};
  const std::vector<size_t> sourceSizes{0u};
  const std::vector<uint64_t> sourceRevisions{0u};

  const auto plan = buildGpuCullDrawUploadPlan(
      {.inputBufferReady = true,
       .cache = cacheView(sourceData, sourceSizes, sourceRevisions),
       .imageIndex = 0u,
       .sourceData = commands.data(),
       .sourceSize = commands.size(),
       .sourceRevision = 7u,
       .maxObjectCount = 2u});

  EXPECT_EQ(plan.action, GpuCullDrawUploadAction::Upload);
  EXPECT_TRUE(plan.uploadsBuffer());
  EXPECT_EQ(plan.drawCount, 2u);
}
