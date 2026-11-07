#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/effects/ScreenReflectionSettings.h"

#include <filesystem>
#include <memory>

namespace container::gpu {
class AllocationManager;
class PipelineManager;
class VulkanDevice;
}

namespace container::renderer {
class FrameResourceManager;
struct FrameRecordParams;

// Runs after opaque lighting, before the transparent resolve and temporal HDR
// consumers. Its private image holds the original opaque color for hit lookups.
class ScreenReflectionPass {
 public:
  ScreenReflectionPass(std::shared_ptr<gpu::VulkanDevice> device,
                       gpu::AllocationManager& allocator,
                       gpu::PipelineManager& pipelines);
  ~ScreenReflectionPass();

  void createResources(const std::filesystem::path& shaderRoot,
                       const FrameResourceManager& frames, VkExtent2D extent,
                       VkDescriptorSetLayout lightLayout, VkDescriptorSetLayout sceneLayout,
                       bool requestRayFallback, bool enableHistory);
  [[nodiscard]] bool ready(const FrameRecordParams& frame) const;
  [[nodiscard]] bool wantsRayScene(const FrameRecordParams& frame) const;
  [[nodiscard]] bool rayFallbackAvailable() const;
  [[nodiscard]] bool rayFallbackActive() const;
  [[nodiscard]] VkDeviceSize allocationBytes() const;
  [[nodiscard]] bool historyAvailable() const;
  [[nodiscard]] bool historyReprojected() const;
  void record(VkCommandBuffer command, const FrameRecordParams& frame);
  void onSubmitted(bool recorded) noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace container::renderer
