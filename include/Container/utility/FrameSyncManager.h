#pragma once

#include <cstddef>
#include <vector>

#include "Container/common/CommonVulkan.h"

namespace container::gpu {

class FrameSyncManager {
 public:
  FrameSyncManager(VkDevice device, size_t framesInFlight);
  ~FrameSyncManager();

  void initialize();
  void cleanup();

  [[nodiscard]] VkSemaphore imageAvailable(size_t frameIndex) const;
  [[nodiscard]] VkFence fence(size_t frameIndex) const;

  void waitForFrame(size_t frameIndex) const;
  void waitForAllFrames() const;
  void resetFence(size_t frameIndex) const;

  [[nodiscard]] size_t framesInFlight() const noexcept {
    return framesInFlight_;
  }

 private:
  VkDevice device_{VK_NULL_HANDLE};
  size_t framesInFlight_{0};

  std::vector<VkSemaphore> imageAvailableSemaphores_;
  std::vector<VkFence> inFlightFences_;
};

}  // namespace container::gpu
