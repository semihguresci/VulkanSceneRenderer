#pragma once

#include <vulkan/vulkan_core.h>

namespace container::gpu {

enum class SwapchainPresentOutcome { Accepted, RejectedEnqueued, Failed };

// A rejected presentation can still enqueue waits on the present semaphores.
[[nodiscard]] constexpr SwapchainPresentOutcome classifySwapchainPresentResult(
    VkResult result) noexcept {
  switch (result) {
  case VK_SUCCESS:
  case VK_SUBOPTIMAL_KHR:
    return SwapchainPresentOutcome::Accepted;
  case VK_ERROR_OUT_OF_DATE_KHR:
  case VK_ERROR_SURFACE_LOST_KHR:
  case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT:
#ifdef VK_EXT_present_timing
  case VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT:
#endif
    return SwapchainPresentOutcome::RejectedEnqueued;
  default:
    return SwapchainPresentOutcome::Failed;
  }
}

}  // namespace container::gpu
