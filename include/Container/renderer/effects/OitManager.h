#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/utility/VulkanDevice.h"

#include <cstdint>
#include <memory>
#include <span>

namespace container::renderer {

// Match the striped allocation counter buffer in shaders/oit_common.slang.
inline constexpr uint32_t kOitCounterStripeCount = 64;
inline constexpr uint32_t kOitCounterOverflowIndex = kOitCounterStripeCount;
inline constexpr uint32_t kOitCounterWordCount = kOitCounterStripeCount + 1;
inline constexpr VkDeviceSize kOitCounterBufferSize =
    kOitCounterWordCount * sizeof(uint32_t);

struct OitFrameResources {
  VkImage headPointerImage{VK_NULL_HANDLE};
  VkBuffer nodeBuffer{VK_NULL_HANDLE};
  VkBuffer counterBuffer{VK_NULL_HANDLE};
};

[[nodiscard]] VkBufferMemoryBarrier2
makeOitCounterReadBarrier(VkBuffer counterBuffer);

[[nodiscard]] uint64_t requiredOitNodeCapacityAfterOverflow(
    std::span<const uint32_t, kOitCounterWordCount> counters,
    uint32_t currentCapacity);

// Handles OIT buffer clear, barrier, and resolve-prepare barriers.
// Does not own any resources; operates on per-frame handles passed in.
class OitManager {
 public:
  explicit OitManager(std::shared_ptr<container::gpu::VulkanDevice> device);

  OitManager(const OitManager&) = delete;
  OitManager& operator=(const OitManager&) = delete;

  void clearResources(VkCommandBuffer cmd, const OitFrameResources& resources,
                      uint32_t invalidNodeIndex) const;

  void prepareResolve(VkCommandBuffer cmd,
                      const OitFrameResources& resources) const;

 private:
  std::shared_ptr<container::gpu::VulkanDevice> device_;
};

}  // namespace container::renderer
