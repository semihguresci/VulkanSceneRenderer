#pragma once

#include "Container/common/CommonVulkan.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace container::gpu {
class VulkanDevice;
class VulkanMemoryManager;
} // namespace container::gpu

namespace container::renderer {

// Immutable, device-lifetime LTC tables. VMA owns image storage; Vulkan-Hpp
// RAII owns views, sampler and upload commands. The allocator must outlive this
// object and the caller must retire all draws before its destruction.
class LtcLutResources {
public:
  LtcLutResources(std::shared_ptr<container::gpu::VulkanDevice> device,
                  container::gpu::VulkanMemoryManager &memory);
  ~LtcLutResources();
  LtcLutResources(const LtcLutResources &) = delete;
  LtcLutResources &operator=(const LtcLutResources &) = delete;

  // Startup-only upload. Missing/corrupt/unsupported tables leave valid dummy
  // images bound and ready()==false, so sampled lighting remains available.
  void load(const std::filesystem::path &assetRoot);
  [[nodiscard]] bool ready() const;
  [[nodiscard]] const std::string &status() const;
  [[nodiscard]] uint64_t allocatedBytes() const;
  [[nodiscard]] VkImageView matrixView() const;
  [[nodiscard]] VkImageView amplitudeView() const;
  [[nodiscard]] VkSampler sampler() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace container::renderer
