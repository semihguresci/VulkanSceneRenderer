#pragma once

#include "Container/common/CommonVMA.h"
#include "Container/common/CommonVulkan.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <cstdint>
#include <filesystem>
#include <memory>
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

  // Create or recreate the mip chain textures for the given viewport size.
  void createTextures(uint32_t width, uint32_t height);

  // Dispatch the full bloom pass: downsample chain → upsample chain.
  // sceneColorView should be the HDR lighting result image view.
  void dispatch(VkCommandBuffer cmd,
                VkImageView     sceneColorView,
                uint32_t        sceneWidth,
                uint32_t        sceneHeight,
                VkImageLayout sourceLayout =
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) const;

  void destroy();

  // ---- Accessors ----

  [[nodiscard]] bool isReady() const { return downsamplePipeline_ != VK_NULL_HANDLE && mipCount_ > 0; }

  // The final bloom result to sample in the post-process shader.
  [[nodiscard]] VkImageView bloomResultView() const {
    if (!upsampleViews_.empty()) return upsampleViews_[0];
    return mipCount_ > 0 ? mipViews_[0] : VK_NULL_HANDLE;
  }
  [[nodiscard]] VkSampler    bloomSampler()    const { return linearSampler_; }

  // ---- Settings ----
  float&    threshold()     { return threshold_; }
  float&    knee()          { return knee_; }
  float&    intensity()     { return intensity_; }
  float&    filterRadius()  { return filterRadius_; }
  bool&     enabled()       { return enabled_; }

 private:
  void createPipelines(const std::filesystem::path& shaderDir);
  void destroyTextures();

  std::shared_ptr<container::gpu::VulkanDevice> device_;
  container::gpu::AllocationManager&            allocationManager_;
  container::gpu::PipelineManager&              pipelineManager_;
  VkCommandPool                                  commandPool_{VK_NULL_HANDLE};

  // Mip chain images
  struct MipLevel {
    VkImage       image{VK_NULL_HANDLE};
    VmaAllocation allocation{nullptr};
    VkImageView   view{VK_NULL_HANDLE};
    uint32_t      width{0};
    uint32_t      height{0};
  };
  std::vector<MipLevel> mips_;
  std::vector<VkImageView> mipViews_;  // redundant pointers for fast access
  std::vector<MipLevel> upsampleMips_;
  std::vector<VkImageView> upsampleViews_;
  uint32_t mipCount_{0};

  VkSampler linearSampler_{VK_NULL_HANDLE};

  // Downsample pipeline
  VkPipeline            downsamplePipeline_{VK_NULL_HANDLE};
  VkPipelineLayout      downsamplePipelineLayout_{VK_NULL_HANDLE};
  VkDescriptorSetLayout downsampleSetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool      downsampleDescriptorPool_{VK_NULL_HANDLE};
  std::vector<VkDescriptorSet> downsampleSets_;  // one per mip transition

  // Upsample pipeline
  VkPipeline            upsamplePipeline_{VK_NULL_HANDLE};
  VkPipelineLayout      upsamplePipelineLayout_{VK_NULL_HANDLE};
  VkDescriptorSetLayout upsampleSetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool      upsampleDescriptorPool_{VK_NULL_HANDLE};
  std::vector<VkDescriptorSet> upsampleSets_;

  // Settings
  float    threshold_{1.0f};
  float    knee_{0.1f};
  float    intensity_{0.3f};
  float    filterRadius_{1.0f};
  bool     enabled_{true};
};

}  // namespace container::renderer
