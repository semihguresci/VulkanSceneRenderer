#pragma once

#include <vulkan/vulkan.hpp>

#include <exception>
#include <utility>

namespace container::renderer::detail {

// Call only after submission succeeds, while every submitted resource remains
// owned by the caller. A fence wait error (including OOM) does not prove that
// work has completed. Drain the queue before allowing those owners to unwind.
template <class WaitForFence, class DrainQueue>
void waitForSubmittedUpload(WaitForFence &&waitForFence,
                            DrainQueue &&drainQueue) {
  try {
    std::forward<WaitForFence>(waitForFence)();
  } catch (const vk::DeviceLostError &) {
    // Device loss ends the pending lifetime requirement, but initialization
    // must still fail rather than exposing an unsuccessful upload.
    throw;
  } catch (...) {
    try {
      std::forward<DrainQueue>(drainQueue)();
    } catch (const vk::DeviceLostError &) {
      // Resources may be released after loss; propagate the original failure.
    } catch (...) {
      // Neither operation established completion. Unwinding would destroy
      // in-flight owners, and retrying OOM indefinitely cannot guarantee
      // progress. Stop while the owners are still alive.
      std::terminate();
    }
    throw;
  }
}

} // namespace container::renderer::detail
