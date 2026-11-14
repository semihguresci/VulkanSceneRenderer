#pragma once

#include "Container/utility/ComputePipeline.h"

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
class VulkanDevice;
}  // namespace container::gpu

namespace container::renderer {

// Manages bloom post-processing via a dual-filter downsample/upsample mip
// chain. Uses compute shaders with 13-tap downsample and 9-tap tent upsample
// (Jimenez 2014).
class BloomManager {
 public:
  static constexpr uint32_t kMaxBloomMips = 6;

  BloomManager(
      std::shared_ptr<container::gpu::VulkanDevice> device,
      container::gpu::AllocationManager&            allocationManager,
      container::gpu::PipelineManager&              pipelineManager,
      VkCommandPool                                  commandPool);

  ~BloomManager();
  BloomManager(const BloomManager&) = delete;
  BloomManager& operator=(const BloomManager&) = delete;

  // Create compute pipelines and sampler. Call once after construction.
  void createResources(const std::filesystem::path& shaderDir);

  // Replace all mip chains after retiring the old swapchain-image readers.
  void createTextures(uint32_t width, uint32_t height, uint32_t imageCount);

  // Dispatch the full bloom pass: downsample chain → upsample chain.
  // The caller retires this swapchain image before updating its descriptors.
  // sceneColorView should be the HDR lighting result image view.
  void dispatch(VkCommandBuffer cmd,
                uint32_t        imageIndex,
                VkImageView     sceneColorView,
                uint32_t        sceneWidth,
                uint32_t        sceneHeight,
                VkImageLayout sourceLayout =
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) const;

  void destroy();

  // ---- Accessors ----

  [[nodiscard]] bool isReady() const {
    return downsamplePipeline_ != VK_NULL_HANDLE && !resultViews_.empty();
  }

  // The final bloom result to sample in the post-process shader.
  [[nodiscard]] std::span<const VkImageView> bloomResultViews() const {
    return resultViews_;
  }
  [[nodiscard]] VkSampler    bloomSampler()    const { return linearSampler_; }

  // ---- Settings ----
  float&    threshold()     { return threshold_; }
  float&    knee()          { return knee_; }
  float&    intensity()     { return intensity_; }
  float&    filterRadius()  { return filterRadius_; }
  bool&     enabled()       { return enabled_; }

 private:
  struct MipLevel {
    VkImage image{VK_NULL_HANDLE};
    VmaAllocation allocation{nullptr};
    VkImageView view{VK_NULL_HANDLE};
    uint32_t width{0};
    uint32_t height{0};
  };
  struct FrameTextures {
    std::vector<MipLevel> downsample;
    std::vector<MipLevel> upsample;
    std::vector<VkDescriptorSet> downsampleSets;
    std::vector<VkDescriptorSet> upsampleSets;
  };

  void createPipelines(const std::filesystem::path& shaderDir);
  void createMip(MipLevel& mip);
  void destroyTextures(std::vector<FrameTextures>& frames) noexcept;

  std::shared_ptr<container::gpu::VulkanDevice> device_;
  container::gpu::AllocationManager&            allocationManager_;
  container::gpu::PipelineManager&              pipelineManager_;
  VkCommandPool                                  commandPool_{VK_NULL_HANDLE};

  std::vector<FrameTextures> frames_;
  std::vector<VkImageView> resultViews_;
  VkDescriptorPool descriptorPool_{VK_NULL_HANDLE};

  VkSampler linearSampler_{VK_NULL_HANDLE};

  // Downsample pipeline
  container::gpu::ComputePipeline downsamplePipeline_{};
  VkPipelineLayout      downsamplePipelineLayout_{VK_NULL_HANDLE};
  VkDescriptorSetLayout downsampleSetLayout_{VK_NULL_HANDLE};

  // Upsample pipeline
  container::gpu::ComputePipeline upsamplePipeline_{};
  VkPipelineLayout      upsamplePipelineLayout_{VK_NULL_HANDLE};
  VkDescriptorSetLayout upsampleSetLayout_{VK_NULL_HANDLE};

  // Settings
  float    threshold_{1.0f};
  float    knee_{0.1f};
  float    intensity_{0.3f};
  float    filterRadius_{1.0f};
  bool     enabled_{true};
};

}  // namespace container::renderer
