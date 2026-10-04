#pragma once

#include "Container/common/CommonVMA.h"
#include "Container/common/CommonVulkan.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace container::gpu {
class AllocationManager;
class PipelineManager;
struct ExposureSettings;
class VulkanDevice;
}  // namespace container::gpu

namespace container::renderer {

// GPU luminance histogram and GPU-resident exposure adaptation for the
// post-process tone mapper.
class ExposureManager {
 public:
  static constexpr uint32_t kHistogramBinCount = 64;

  ExposureManager(
      std::shared_ptr<container::gpu::VulkanDevice> device,
      container::gpu::AllocationManager& allocationManager,
      container::gpu::PipelineManager& pipelineManager);
  ~ExposureManager();

  ExposureManager(const ExposureManager&) = delete;
  ExposureManager& operator=(const ExposureManager&) = delete;

  void createResources(const std::filesystem::path& shaderDir,
                       uint32_t descriptorSetCount = 1);
  void dispatch(uint32_t imageIndex,
                VkCommandBuffer cmd,
                VkImageView sceneColorView,
                uint32_t sceneWidth,
                uint32_t sceneHeight,
                const container::gpu::ExposureSettings& settings,
      VkImageLayout sourceLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  void collectReadback(uint32_t imageIndex,
                       const container::gpu::ExposureSettings& settings);
  void destroy();

  [[nodiscard]] bool isReady() const {
    return histogramPipeline_ != VK_NULL_HANDLE &&
           adaptPipeline_ != VK_NULL_HANDLE &&
           !descriptorSets_.empty() &&
           !histogramBuffers_.empty() &&
           exposureStateBuffer_.buffer != VK_NULL_HANDLE;
  }

  [[nodiscard]] float resolvedExposure(
      const container::gpu::ExposureSettings& settings) const;
  [[nodiscard]] float averageLuminance() const { return averageLuminance_; }
  [[nodiscard]] bool hasExposureDebugState() const { return hasCurrentExposure_; }
  [[nodiscard]] VkBuffer exposureStateBuffer(uint32_t imageIndex) const {
    (void)imageIndex;
    return exposureStateBuffer_.buffer;
  }
  [[nodiscard]] std::span<const container::gpu::AllocatedBuffer>
  exposureStateBuffers() const {
    if (exposureStateBuffer_.buffer == VK_NULL_HANDLE) return {};
    return std::span<const container::gpu::AllocatedBuffer>(
        &exposureStateBuffer_, 1u);
  }
  [[nodiscard]] VkDeviceSize exposureStateBufferSize() const;

 private:
  void createPipeline(const std::filesystem::path& shaderDir);
  void resizeFrameResources(uint32_t descriptorSetCount);
  void createHistogramBuffer(container::gpu::AllocatedBuffer& buffer);
  void createExposureStateBuffer(container::gpu::AllocatedBuffer& buffer);
  void destroyFrameResources();
  void updateDescriptorSet(uint32_t imageIndex, VkImageView sceneColorView,
                           VkImageLayout sourceLayout);

  std::shared_ptr<container::gpu::VulkanDevice> device_;
  container::gpu::AllocationManager& allocationManager_;
  container::gpu::PipelineManager& pipelineManager_;

  std::vector<container::gpu::AllocatedBuffer> histogramBuffers_{};
  // Temporal adaptation must be shared across swapchain images; per-image
  // exposure history presents as brightness flicker in static scenes.
  container::gpu::AllocatedBuffer exposureStateBuffer_{};
  VkDescriptorSetLayout setLayout_{VK_NULL_HANDLE};
  VkDescriptorPool descriptorPool_{VK_NULL_HANDLE};
  std::vector<VkDescriptorSet> descriptorSets_{};
  VkPipelineLayout pipelineLayout_{VK_NULL_HANDLE};
  VkPipeline histogramPipeline_{VK_NULL_HANDLE};
  VkPipeline adaptPipeline_{VK_NULL_HANDLE};

  float currentExposure_{0.25f};
  float averageLuminance_{0.18f};
  bool hasCurrentExposure_{false};
};

}  // namespace container::renderer
