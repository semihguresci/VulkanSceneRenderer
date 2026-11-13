#include "Container/utility/SwapchainPresentResult.h"

#include <gtest/gtest.h>

namespace {

using container::gpu::classifySwapchainPresentResult;
using container::gpu::SwapchainPresentOutcome;

TEST(SwapchainPresentResult, SuccessAndSuboptimalAcceptPresentation) {
  EXPECT_EQ(classifySwapchainPresentResult(VK_SUCCESS),
            SwapchainPresentOutcome::Accepted);
  EXPECT_EQ(classifySwapchainPresentResult(VK_SUBOPTIMAL_KHR),
            SwapchainPresentOutcome::Accepted);
}

TEST(SwapchainPresentResult, PresentationRejectionsStillEnqueueWaits) {
  EXPECT_EQ(classifySwapchainPresentResult(VK_ERROR_OUT_OF_DATE_KHR),
            SwapchainPresentOutcome::RejectedEnqueued);
  EXPECT_EQ(classifySwapchainPresentResult(VK_ERROR_SURFACE_LOST_KHR),
            SwapchainPresentOutcome::RejectedEnqueued);
  EXPECT_EQ(classifySwapchainPresentResult(
                VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT),
            SwapchainPresentOutcome::RejectedEnqueued);
#ifdef VK_EXT_present_timing
  EXPECT_EQ(classifySwapchainPresentResult(VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT),
            SwapchainPresentOutcome::RejectedEnqueued);
#endif
}

TEST(SwapchainPresentResult, FailedResultsDoNotProvideAPresentFence) {
  EXPECT_EQ(classifySwapchainPresentResult(VK_ERROR_OUT_OF_HOST_MEMORY),
            SwapchainPresentOutcome::Failed);
  EXPECT_EQ(classifySwapchainPresentResult(VK_ERROR_OUT_OF_DEVICE_MEMORY),
            SwapchainPresentOutcome::Failed);
  EXPECT_EQ(classifySwapchainPresentResult(VK_ERROR_DEVICE_LOST),
            SwapchainPresentOutcome::Failed);
}

}  // namespace
