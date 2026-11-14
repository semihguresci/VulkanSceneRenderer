#include "Container/renderer/lighting/DdgiAsyncHistory.h"
#include <gtest/gtest.h>

namespace {
using container::renderer::DdgiAsyncHistory;

TEST(DdgiAsyncHistory, RequiresSuccessfullySubmittedGraphicsInputs) {
  DdgiAsyncHistory history;
  history.reset(2);
  EXPECT_THROW((void)history.plan(0), std::logic_error);
  EXPECT_EQ(history.graphicsSignal(), 1);
  EXPECT_THROW(history.commitGraphics(2), std::logic_error);
  history.commitGraphics(1);
  EXPECT_EQ(history.plan(0).graphicsReady, 1);
}
TEST(DdgiAsyncHistory, PublishingRequiresGpuCompletion) {
  DdgiAsyncHistory history;
  history.reset(2);
  history.commitGraphics(1);
  const auto work = history.plan(0);
  history.commit(work);
  EXPECT_EQ(history.history(), 0);
  EXPECT_EQ(history.publication(), 0);
  EXPECT_FALSE(history.publish(work.completion - 1));
  EXPECT_EQ(history.history(), work.previous);
  ASSERT_TRUE(history.publish(work.completion));
  EXPECT_EQ(history.history(), work.next);
  EXPECT_EQ(history.publication(), work.completion);
}
TEST(DdgiAsyncHistory, GraphicsCanContinueReadingWhileComputeRuns) {
  DdgiAsyncHistory history;
  history.reset(3);
  history.commitGraphics(1);
  const auto work = history.plan(0);
  history.commit(work);
  history.commitGraphics(2);
  history.commitGraphics(3);
  EXPECT_EQ(history.history(), work.previous);
  EXPECT_EQ(history.graphicsSignal(), 4);
  EXPECT_TRUE(history.pending());
  EXPECT_THROW((void)history.plan(1), std::logic_error);
}
TEST(DdgiAsyncHistory, RetainsBorrowedInputImageUntilComputeRetires) {
  DdgiAsyncHistory history;
  history.reset(2);
  history.commitGraphics(1);
  const auto work = history.plan(1);
  history.commit(work);
  EXPECT_EQ(history.imageRetirement(0), 0);
  EXPECT_EQ(history.imageRetirement(1), work.completion);
  ASSERT_TRUE(history.publish(work.completion));
  EXPECT_EQ(history.imageRetirement(1), work.completion);
}
TEST(DdgiAsyncHistory, NewWriterWaitsForLatestGraphicsReaders) {
  DdgiAsyncHistory history;
  history.reset(2);
  history.commitGraphics(1);
  auto work = history.plan(0);
  history.commit(work);
  ASSERT_TRUE(history.publish(1));
  history.commitGraphics(2);
  work = history.plan(1);
  EXPECT_EQ(work.previous, 1);
  EXPECT_EQ(work.next, 0);
  EXPECT_EQ(work.graphicsReady, 2);
  EXPECT_EQ(work.completion, 2);
}
TEST(DdgiAsyncHistory, StaleSubmissionCannotAdvanceHistory) {
  DdgiAsyncHistory history;
  history.reset(2);
  history.commitGraphics(1);
  auto stale = history.plan(0);
  history.commitGraphics(2);
  EXPECT_THROW(history.commit(stale), std::logic_error);
  EXPECT_FALSE(history.pending());
  EXPECT_EQ(history.lastCompute(), 0);
}
TEST(DdgiAsyncHistory, ResetCannotReleaseLiveResources) {
  DdgiAsyncHistory history;
  history.reset(2);
  history.commitGraphics(1);
  const auto work = history.plan(0);
  history.commit(work);
  EXPECT_THROW(history.reset(3), std::logic_error);
  ASSERT_TRUE(history.publish(work.completion));
  history.reset(3);
  EXPECT_EQ(history.history(), 0);
  EXPECT_EQ(history.publication(), 0);
  EXPECT_EQ(history.imageRetirement(2), 0);
}
TEST(DdgiAsyncHistory, CompletedPublicationIsIdempotent) {
  DdgiAsyncHistory history;
  history.reset(2);
  history.commitGraphics(1);
  history.commit(history.plan(0));
  ASSERT_TRUE(history.publish(1));
  EXPECT_FALSE(history.publish(100));
  EXPECT_EQ(history.publication(), 1);
}
TEST(DdgiAsyncHistory, GraphicsRemapRetainsComputeVisibilityDependency) {
  DdgiAsyncHistory history;
  history.reset(2);
  history.commitGraphics(1);
  history.commit(history.plan(0));
  ASSERT_TRUE(history.publish(1));
  history.remapPublishedHistory(0);
  EXPECT_EQ(history.history(), 0);
  EXPECT_EQ(history.publication(), 1);
  history.commitGraphics(2);
  EXPECT_EQ(history.plan(1).previous, 0);
  EXPECT_EQ(history.plan(1).next, 1);
}
TEST(DdgiAsyncHistory, RemapCannotAliasLiveWriter) {
  DdgiAsyncHistory history;
  history.reset(2);
  history.commitGraphics(1);
  history.commit(history.plan(0));
  EXPECT_THROW(history.remapPublishedHistory(1), std::logic_error);
  EXPECT_EQ(history.history(), 0);
  ASSERT_TRUE(history.publish(1));
  EXPECT_THROW(history.remapPublishedHistory(2), std::logic_error);
  EXPECT_EQ(history.history(), 1);
}
} // namespace
