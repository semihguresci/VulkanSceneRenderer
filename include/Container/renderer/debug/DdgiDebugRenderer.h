#pragma once

#include "Container/common/CommonVulkan.h"

#include <cstdint>
#include <filesystem>
#include <memory>

namespace container::gpu {
class VulkanDevice;
}

namespace container::renderer {
enum class DdgiDebugView : uint32_t;

// Procedural sphere impostors in the existing swapchain rendering scope.
// Main-depth sampling provides reverse-Z occlusion without a depth attachment.
// The caller supplies the scene viewport and fence-retired descriptor sets.
class DdgiDebugRenderer {
public:
  DdgiDebugRenderer(std::shared_ptr<container::gpu::VulkanDevice> device,
                    const std::filesystem::path &root,
                    VkDescriptorSetLayout postProcess,
                    VkDescriptorSetLayout ddgi, VkFormat colorFormat);
  ~DdgiDebugRenderer();
  DdgiDebugRenderer(const DdgiDebugRenderer &) = delete;
  DdgiDebugRenderer &operator=(const DdgiDebugRenderer &) = delete;

  void record(VkCommandBuffer cmd, VkDescriptorSet postSet,
              VkDescriptorSet ddgiSet, uint32_t probeCount, DdgiDebugView view,
              float radius) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace container::renderer
