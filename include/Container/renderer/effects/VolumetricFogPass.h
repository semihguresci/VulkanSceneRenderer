#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/effects/VolumetricFogSettings.h"

#include <filesystem>
#include <memory>

namespace container::gpu {
class AllocationManager;
class PipelineManager;
class VulkanDevice;
}

namespace container::renderer {
class DeferredRasterFrameGraphContext;
class FrameResourceManager;
class ShadowManager;
class RenderGraphBuilder;
enum class RenderPassId : uint8_t;
struct FrameRecordParams;

// Graphics-queue pass. The caller retires submitted work before destroying or
// replacing its scene-size resources.
class VolumetricFogPass {
 public:
  VolumetricFogPass(std::shared_ptr<gpu::VulkanDevice>, gpu::AllocationManager&, gpu::PipelineManager&);
  ~VolumetricFogPass();

  void createResources(const std::filesystem::path& shaderRoot, const FrameResourceManager&, VkExtent2D,
      const ShadowManager&, VkDescriptorSetLayout lightLayout);
  [[nodiscard]] bool ready(const FrameRecordParams&) const;
  void record(VkCommandBuffer, const FrameRecordParams&, bool transparentOitActive);
  void onSubmitted(bool recorded) noexcept;
  [[nodiscard]] VkDeviceSize allocationBytes() const;
  [[nodiscard]] bool historyReprojected() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

void registerVolumetricFogPass(RenderGraphBuilder&, RenderPassId predecessor,
    const DeferredRasterFrameGraphContext*);

} // namespace container::renderer
