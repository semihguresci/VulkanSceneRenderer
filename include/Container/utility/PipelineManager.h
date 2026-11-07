#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/utility/ComputePipeline.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace container::gpu {

class PipelineManager {
 public:
  explicit PipelineManager(VkDevice device);
  ~PipelineManager();

  PipelineManager(const PipelineManager&) = delete;
  PipelineManager& operator=(const PipelineManager&) = delete;
  PipelineManager(PipelineManager&&) = delete;
  PipelineManager& operator=(PipelineManager&&) = delete;

  VkDescriptorSetLayout createDescriptorSetLayout(
      const std::vector<VkDescriptorSetLayoutBinding>& bindings,
      const std::vector<VkDescriptorBindingFlags>& bindingFlags,
      VkDescriptorSetLayoutCreateFlags flags = 0, const void* next = nullptr);

  void destroyDescriptorSetLayout(VkDescriptorSetLayout& layout);

  VkDescriptorPool createDescriptorPool(
      const std::vector<VkDescriptorPoolSize>& poolSizes, uint32_t maxSets,
      VkDescriptorPoolCreateFlags flags = 0);

  void destroyDescriptorPool(VkDescriptorPool& pool);

  VkPipelineLayout createPipelineLayout(
      const std::vector<VkDescriptorSetLayout>& setLayouts,
      const std::vector<VkPushConstantRange>& pushConstantRanges,
      const void* next = nullptr);

  void destroyPipelineLayout(VkPipelineLayout& layout);

  VkPipelineCache getOrCreatePipelineCache(
      const std::string& cacheKey,
      const VkPipelineCacheCreateInfo* createInfo = nullptr);

  VkPipeline createGraphicsPipeline(
      const RenderingGraphicsPipelineCreateInfo& pipelineInfo,
      const std::string& cacheKey);

  ComputePipeline createComputePipeline(const std::filesystem::path& shader,
                                        VkPipelineLayout layout, const std::string& cacheKey);

  class ComputeReload {
   public:
    ComputeReload() = default;
    ComputeReload(ComputeReload&&) noexcept = default;
    ComputeReload& operator=(ComputeReload&&) noexcept = default;
    [[nodiscard]] size_t size() const noexcept { return changes_.size(); }
    [[nodiscard]] bool changesShader(std::string_view filename) const;

   private:
    friend class PipelineManager;
    std::filesystem::path directory_;
    std::vector<std::pair<std::shared_ptr<ComputePipeline::State>, ComputePipeline>> changes_;
  };

  [[nodiscard]] ComputeReload prepareComputeReload(const std::filesystem::path& directory);
  void publishComputeReload(ComputeReload& candidate) noexcept;

  void destroyPipeline(VkPipeline& pipeline);
  void destroyPipeline(ComputePipeline& pipeline) noexcept { pipeline.reset(); }

  void destroyManagedResources();

 private:
  ComputePipeline buildComputePipeline(const std::filesystem::path& shader,
                                       VkPipelineLayout layout, const std::string& cacheKey,
                                       std::vector<char> code);
  std::vector<std::weak_ptr<ComputePipeline::State>> computePipelines_;
  std::filesystem::path activeComputeDirectory_;
  VkDevice device_{VK_NULL_HANDLE};
  std::unordered_map<std::string, VkPipelineCache> pipelineCaches_{};
  std::vector<VkDescriptorSetLayout> descriptorSetLayouts_{};
  std::vector<VkDescriptorPool> descriptorPools_{};
  std::vector<VkPipelineLayout> pipelineLayouts_{};
  std::vector<VkPipeline> pipelines_{};
};

}  // namespace container::gpu
