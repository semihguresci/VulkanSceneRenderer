#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/utility/ComputePipeline.h"
#include "Container/utility/VulkanDevice.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace container::gpu {
class PipelineManager;
}

namespace container::renderer {

struct FrameRecordParams;

// Match the striped allocation counter buffer in shaders/oit_common.slang.
inline constexpr uint32_t kOitCounterStripeCount = 64;
inline constexpr uint32_t kOitCounterOverflowIndex = kOitCounterStripeCount;
inline constexpr uint32_t kOitCounterWordCount = kOitCounterStripeCount + 1;
inline constexpr VkDeviceSize kOitCounterBufferSize =
    kOitCounterWordCount * sizeof(uint32_t);

struct OitFrameResources {
  VkImage headPointerImage{VK_NULL_HANDLE};
  VkBuffer nodeBuffer{VK_NULL_HANDLE};
  VkBuffer counterBuffer{VK_NULL_HANDLE};
};

struct OitNativeComposeInputs {
  uint32_t imageIndex{0};
  VkImage sceneColor{VK_NULL_HANDLE};
  VkImageView sceneColorView{VK_NULL_HANDLE};
  VkExtent2D extent{};
  VkImage headPointerImage{VK_NULL_HANDLE};
  VkImageView headPointerView{VK_NULL_HANDLE};
  VkBuffer nodeBuffer{VK_NULL_HANDLE};
  VkDeviceSize nodeBufferSize{0};
  VkBuffer metadataBuffer{VK_NULL_HANDLE};
};

[[nodiscard]] OitNativeComposeInputs
oitNativeComposeInputs(const FrameRecordParams& frame);

[[nodiscard]] VkBufferMemoryBarrier2
makeOitCounterReadBarrier(VkBuffer counterBuffer);

[[nodiscard]] uint64_t requiredOitNodeCapacityAfterOverflow(
    std::span<const uint32_t, kOitCounterWordCount> counters,
    uint32_t currentCapacity);

// Clears and synchronizes the borrowed OIT resources and owns the native
// composition pipeline and descriptor sets.
class OitManager {
 public:
  explicit OitManager(std::shared_ptr<container::gpu::VulkanDevice> device);
  ~OitManager();

  OitManager(const OitManager&) = delete;
  OitManager& operator=(const OitManager&) = delete;

  void clearResources(VkCommandBuffer cmd, const OitFrameResources& resources,
                      uint32_t invalidNodeIndex) const;

  void prepareResolve(VkCommandBuffer cmd,
                      const OitFrameResources& resources) const;

  // The pipeline manager outlives this owner. Resize after retiring frames.
  void prepareNativeComposeResources(container::gpu::PipelineManager& pipelines,
                                     const std::filesystem::path& shaderRoot,
                                     uint32_t imageCount);
  [[nodiscard]] bool nativeCompositionReady(
      const OitNativeComposeInputs& inputs) const;
  // Record after the acquired image's previous submission has retired.
  [[nodiscard]] bool recordNativeComposition(
      VkCommandBuffer cmd, const OitNativeComposeInputs& inputs) const;

 private:
  std::shared_ptr<container::gpu::VulkanDevice> device_;
  container::gpu::PipelineManager* pipelines_{nullptr};
  VkDescriptorSetLayout composeSetLayout_{VK_NULL_HANDLE};
  VkPipelineLayout composeLayout_{VK_NULL_HANDLE};
  container::gpu::ComputePipeline composePipeline_;
  VkDescriptorPool composePool_{VK_NULL_HANDLE};
  std::vector<VkDescriptorSet> composeSets_;
};

}  // namespace container::renderer
