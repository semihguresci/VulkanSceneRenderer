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
  // Retire all GPU readers before changing the per-image resources.
  void resizeFrameResources(uint32_t descriptorSetCount);
  void dispatch(uint32_t imageIndex,
                VkCommandBuffer cmd,
                VkImageView sceneColorView,
                uint32_t sceneWidth,
                uint32_t sceneHeight,
                const container::gpu::ExposureSettings& settings,
                VkImageLayout sourceLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  void recordManualExposure(VkCommandBuffer cmd,
                            const container::gpu::ExposureSettings& settings);
  void beginFrame() { recordedWrite_ = {}; }
  // Publish readback ownership and the history-mode change after queue submit succeeds.
  void onSubmitted();
  // The caller retires the image's graphics fence before collecting its copy.
  void collectReadback(uint32_t imageIndex,
                       const container::gpu::ExposureSettings& settings);
  void destroy();

  [[nodiscard]] bool isReady() const {
    return histogramPipeline_ != VK_NULL_HANDLE &&
           adaptPipeline_ != VK_NULL_HANDLE &&
           !descriptorSets_.empty() &&
           !histogramBuffers_.empty() &&
           !readbacks_.empty() &&
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
  void createExposureStateBuffer(container::gpu::AllocatedBuffer& buffer);
  void recordReadback(uint32_t imageIndex, VkCommandBuffer cmd);
  void destroyFrameResources();
  void updateDescriptorSet(uint32_t imageIndex, VkImageView sceneColorView,
                           VkImageLayout sourceLayout);

  std::shared_ptr<container::gpu::VulkanDevice> device_;
  container::gpu::AllocationManager& allocationManager_;
  container::gpu::PipelineManager& pipelineManager_;

  std::vector<container::gpu::AllocatedBuffer> histogramBuffers_{};
  struct ReadbackSlot {
    container::gpu::AllocatedBuffer buffer;
    uint64_t serial{0};
  };
  std::vector<ReadbackSlot> readbacks_{};
  enum class HistoryWrite { None, Automatic, Manual };
  struct RecordedHistoryWrite {
    HistoryWrite mode{HistoryWrite::None};
    uint32_t imageIndex{0};
    float exposure{0.0f};
    float luminance{0.0f};
  } recordedWrite_{};
  uint64_t nextReadbackSerial_{0};
  uint64_t latestReadbackSerial_{0};
  float manualSeedExposure_{0.0f};
  float manualSeedLuminance_{0.0f};
  bool hasManualSeed_{false};
  // Tone mapping and adaptation share one queue-ordered history.
  // Fenced host reads use the per-image snapshots above.
  container::gpu::AllocatedBuffer exposureStateBuffer_{};
  VkDescriptorSetLayout setLayout_{VK_NULL_HANDLE};
  VkDescriptorPool descriptorPool_{VK_NULL_HANDLE};
  std::vector<VkDescriptorSet> descriptorSets_{};
  VkPipelineLayout pipelineLayout_{VK_NULL_HANDLE};
  container::gpu::ComputePipeline histogramPipeline_{};
  container::gpu::ComputePipeline adaptPipeline_{};

  float currentExposure_{0.25f};
  float averageLuminance_{0.18f};
  bool hasCurrentExposure_{false};
};

}  // namespace container::renderer
