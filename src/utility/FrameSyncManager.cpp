#include <limits>
#include <stdexcept>

#include "Container/utility/FrameSyncManager.h"

namespace container::gpu {

FrameSyncManager::FrameSyncManager(VkDevice device, size_t framesInFlight)
    : device_(device), framesInFlight_(framesInFlight) {}

FrameSyncManager::~FrameSyncManager() { cleanup(); }

void FrameSyncManager::initialize() {
  // If someone calls initialize twice, don't accumulate resources.
  cleanup();

  VkSemaphoreCreateInfo semaphoreInfo{};
  semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

  VkFenceCreateInfo fenceInfo{};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

  imageAvailableSemaphores_.resize(framesInFlight_);
  for (size_t i = 0; i < framesInFlight_; ++i) {
    VkResult res = createOwnedSemaphore(device_, &semaphoreInfo, nullptr,
                                     &imageAvailableSemaphores_[i]);
    if (res != VK_SUCCESS) {
      throw std::runtime_error("Failed to create imageAvailable semaphore!");
    }
  }

  inFlightFences_.resize(framesInFlight_);
  for (size_t i = 0; i < framesInFlight_; ++i) {
    VkResult res =
        createOwnedFence(device_, &fenceInfo, nullptr, &inFlightFences_[i]);
    if (res != VK_SUCCESS) {
      throw std::runtime_error("Failed to create inFlight fence!");
    }
  }
}

void FrameSyncManager::cleanup() {
  for (VkSemaphore sem : imageAvailableSemaphores_) {
    if (sem != VK_NULL_HANDLE) {
      destroyOwnedSemaphore(device_, sem, nullptr);
    }
  }
  imageAvailableSemaphores_.clear();

  for (VkFence f : inFlightFences_) {
    if (f != VK_NULL_HANDLE) {
      destroyOwnedFence(device_, f, nullptr);
    }
  }
  inFlightFences_.clear();
}

VkSemaphore FrameSyncManager::imageAvailable(size_t frameIndex) const {
  return imageAvailableSemaphores_.at(frameIndex);
}

VkFence FrameSyncManager::fence(size_t frameIndex) const {
  return inFlightFences_.at(frameIndex);
}

void FrameSyncManager::waitForFrame(size_t frameIndex) const {
  VkFence f = inFlightFences_.at(frameIndex);

  VkResult res = vkWaitForFences(device_, 1, &f, VK_TRUE,
                                 std::numeric_limits<uint64_t>::max());

  if (res != VK_SUCCESS) {
    throw std::runtime_error("Failed to wait for fence!");
  }
}

void FrameSyncManager::waitForAllFrames() const {
  if (inFlightFences_.empty()) return;

  VkResult res = vkWaitForFences(
      device_, static_cast<uint32_t>(inFlightFences_.size()),
      inFlightFences_.data(), VK_TRUE,
      std::numeric_limits<uint64_t>::max());

  if (res != VK_SUCCESS) {
    throw std::runtime_error("Failed to wait for all in-flight fences!");
  }
}

void FrameSyncManager::resetFence(size_t frameIndex) const {
  VkFence f = inFlightFences_.at(frameIndex);
  VkResult res = vkResetFences(device_, 1, &f);
  if (res != VK_SUCCESS) {
    throw std::runtime_error("Failed to reset fence!");
  }
}

}  // namespace container::gpu
