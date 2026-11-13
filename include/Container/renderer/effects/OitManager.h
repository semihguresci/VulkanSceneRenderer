#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/utility/VulkanDevice.h"

#include <memory>

namespace container::renderer {

struct OitFrameResources {
  VkImage headPointerImage{VK_NULL_HANDLE};
  VkBuffer nodeBuffer{VK_NULL_HANDLE};
  VkBuffer counterBuffer{VK_NULL_HANDLE};
};

[[nodiscard]] VkBufferMemoryBarrier2
makeOitCounterReadBarrier(VkBuffer counterBuffer);

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
