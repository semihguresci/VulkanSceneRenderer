#pragma once

#include <Container/renderer/pathtracing/ReferencePathTracePass.h>

namespace container::renderer {

// Fence-retired frame slots and an independent history for the interactive
// display. The reference accumulation keeps its original sampling and images.
class ReferenceDlssPass {
 public:
  ReferenceDlssPass(std::shared_ptr<gpu::VulkanDevice>, gpu::VulkanMemoryManager&,
      gpu::PipelineManager&, const std::filesystem::path& shaderRoot, UpscaleMode, VkExtent2D output,
      uint32_t imageCount, VkDescriptorSetLayout materials, VkDescriptorSetLayout lights,
      VkDescriptorSetLayout scene, uint64_t guideBudget);
  ~ReferenceDlssPass();

  void record(VkCommandBuffer, uint32_t image, const ReferencePathTraceInput&,
      const ReferencePathTraceData&, VkDescriptorSet sceneSet);
  void commit(uint32_t image) noexcept;
  void invalidateHistory() noexcept;
  void collectTiming(uint32_t image);
  [[nodiscard]] ReferenceDlssStatus status() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace container::renderer
