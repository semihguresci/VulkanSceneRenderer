#include "Container/renderer/deferred/DeferredTransparentOitFramePassRecorder.h"

#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/scene/DrawCommand.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using container::renderer::DeferredTransparentOitFramePassRecorder;
using container::renderer::DrawCommand;
using container::renderer::FrameRecordParams;
using container::renderer::RenderPassSkipReason;
using container::renderer::RenderResourceId;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

} // namespace

TEST(DeferredTransparentOitFramePassRecorderTests,
     EmptyFrameIsDisabledAndNotNeeded) {
  DeferredTransparentOitFramePassRecorder recorder({});
  const FrameRecordParams params{};

  EXPECT_FALSE(recorder.enabled(params));
  EXPECT_FALSE(recorder.readiness(params).ready);
}

TEST(DeferredTransparentOitFramePassRecorderTests,
     ClearAndResolveRejectDisabledFrame) {
  DeferredTransparentOitFramePassRecorder recorder({});
  const FrameRecordParams params{};

  EXPECT_FALSE(recorder.recordClear(fakeHandle<VkCommandBuffer>(0x1), params));
  EXPECT_FALSE(
      recorder.recordResolvePreparation(fakeHandle<VkCommandBuffer>(0x2),
                                        params));
}

TEST(DeferredTransparentOitFramePassRecorderTests,
     EnabledFrameRequiresPublishedOitResourcesBeforeReady) {
  std::vector<DrawCommand> transparentDraws(1u);
  DeferredTransparentOitFramePassRecorder recorder({});
  FrameRecordParams params{};
  params.draws.transparentDrawCommands = &transparentDraws;

  const auto readiness = recorder.readiness(params);

  EXPECT_TRUE(recorder.enabled(params));
  EXPECT_FALSE(readiness.ready);
  EXPECT_EQ(readiness.skipReason, RenderPassSkipReason::MissingResource);
  EXPECT_EQ(readiness.blockingResource, RenderResourceId::OitStorage);
}
