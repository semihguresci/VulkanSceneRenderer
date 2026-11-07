#pragma once

#include "Container/utility/ComputePipeline.h"

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/debug/DebugOverlayRenderer.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <array>
#include <cstddef>
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

class ShadowCullManager {
 public:
  ShadowCullManager(
	  std::shared_ptr<container::gpu::VulkanDevice> device,
	  container::gpu::AllocationManager&            allocationManager,
	  container::gpu::PipelineManager&              pipelineManager);

  ~ShadowCullManager();
  ShadowCullManager(const ShadowCullManager&) = delete;
  ShadowCullManager& operator=(const ShadowCullManager&) = delete;

  class PreparedGeneration {
   public:
    PreparedGeneration() noexcept;
    ~PreparedGeneration();
    PreparedGeneration(PreparedGeneration&&) noexcept;
    PreparedGeneration& operator=(PreparedGeneration&&) noexcept;
    PreparedGeneration(const PreparedGeneration&) = delete;
    PreparedGeneration& operator=(const PreparedGeneration&) = delete;
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] uint32_t maxDrawCount(uint32_t image) const noexcept;
    [[nodiscard]] VkBuffer indirectDrawBuffer(uint32_t image,uint32_t cascade) const noexcept;
    [[nodiscard]] VkBuffer drawCountBuffer(uint32_t image,uint32_t cascade) const noexcept;
    [[nodiscard]] ShadowCullManager& manager();
   private:
    friend class ShadowCullManager;
    struct State;
    std::unique_ptr<State> state_;
  };
  // Stages independent per-image/cascade buffers and descriptor sets while
  // borrowing immutable pipelines/layouts and current shadow UBOs. Retire GPU
  // readers before exchange/destruction; histories must die before this manager.
  [[nodiscard]] PreparedGeneration prepareGeneration(
      std::span<const container::gpu::AllocatedBuffer> objects,
      std::span<const size_t> objectCapacities,std::span<const VkBuffer> shadowUbos,
      VkDeviceSize shadowUboBytes,std::span<const DrawCommand> commands,
      uint64_t sourceRevision) const;
  [[nodiscard]] bool canExchangeGeneration(const PreparedGeneration&) const noexcept;
  bool exchangeGeneration(PreparedGeneration&) noexcept;

  void createResources(const std::filesystem::path& shaderDir,
					   uint32_t descriptorSetCount);
  void recreatePerFrameResources(uint32_t descriptorSetCount);
  bool ensureBufferCapacity(uint32_t maxDrawCount);
  bool ensureBufferCapacity(uint32_t imageIndex, uint32_t maxDrawCount);
  void uploadDrawCommands(uint32_t imageIndex,
                          const std::vector<DrawCommand>& commands,
                          uint64_t sourceRevision);
  void uploadDrawCommands(uint32_t imageIndex,std::span<const DrawCommand> commands,
                          uint64_t sourceRevision);

  void updateObjectSsboDescriptor(VkBuffer objectBuffer,
								  VkDeviceSize objectBufferSize) noexcept;
  void updateObjectSsboDescriptor(uint32_t imageIndex,
								  VkBuffer objectBuffer,
								  VkDeviceSize objectBufferSize) noexcept;
	void updateShadowCullDescriptor(uint32_t imageIndex,
								  VkBuffer shadowCullBuffer,
								  VkDeviceSize shadowCullBufferSize);

  [[nodiscard]] bool dispatchCascadeCull(VkCommandBuffer cmd,
					                     uint32_t imageIndex,
						                 uint32_t cascadeIndex,
						                 uint32_t drawCount,
						                 uint32_t outputOffset = 0);

  [[nodiscard]] VkBuffer indirectDrawBuffer(uint32_t imageIndex,
                                            uint32_t cascadeIndex) const {
	return imageIndex < indirectDrawBuffers_.size() &&
	           cascadeIndex < container::gpu::kShadowCascadeCount
			   ? indirectDrawBuffers_[imageIndex][cascadeIndex].buffer
			   : VK_NULL_HANDLE;
  }
  [[nodiscard]] VkBuffer drawCountBuffer(uint32_t imageIndex,
                                         uint32_t cascadeIndex) const {
	return imageIndex < drawCountBuffers_.size() &&
	           cascadeIndex < container::gpu::kShadowCascadeCount
			   ? drawCountBuffers_[imageIndex][cascadeIndex].buffer
			   : VK_NULL_HANDLE;
  }
  [[nodiscard]] uint32_t maxDrawCount() const { return maxDrawCount_; }
  [[nodiscard]] uint32_t maxDrawCount(uint32_t imageIndex) const {
	return imageIndex < drawCapacities_.size() ? drawCapacities_[imageIndex]
	                                           : 0u;
  }
  [[nodiscard]] bool isReady() const;
  [[nodiscard]] bool canDispatchCascadeCull(uint32_t imageIndex,
                                            uint32_t cascadeIndex) const;

 private:
	void createShadowCullPipeline(const std::filesystem::path& shaderDir);
	void writeDescriptorSets(uint32_t imageIndex) noexcept;
	void resizePerImageBufferState(uint32_t imageCount);
	void updateGlobalDrawCapacity();
  void destroyGenerationPool();
  [[nodiscard]] size_t descriptorSetIndex(uint32_t imageIndex,
										  uint32_t cascadeIndex) const;

  std::shared_ptr<container::gpu::VulkanDevice> device_;
  container::gpu::AllocationManager&            allocationManager_;
  container::gpu::PipelineManager&              pipelineManager_;
  bool ownsSharedResources_{true};
  bool ownsGenerationPool_{false};

  uint32_t maxDrawCount_{0};
  VkDeviceSize shadowCullUboSize_{0};
  std::vector<container::gpu::GpuDrawIndexedIndirectCommand> uploadScratch_{};
  std::vector<const DrawCommand *> lastUploadSourceData_{};
  std::vector<size_t> lastUploadSourceSize_{};
  std::vector<uint64_t> lastUploadSourceRevision_{};

	std::vector<VkBuffer> shadowCullBuffers_{};
	std::vector<VkBuffer> objectSsboBuffers_{};
	std::vector<VkDeviceSize> objectSsboSizes_{};
	std::vector<uint32_t> objectCounts_{};
	container::gpu::AllocatedBuffer ownedShadowCullUbo_{};
  std::vector<container::gpu::AllocatedBuffer> inputDrawBuffers_{};
  std::vector<std::array<container::gpu::AllocatedBuffer, container::gpu::kShadowCascadeCount>>
	  indirectDrawBuffers_{};
  std::vector<std::array<container::gpu::AllocatedBuffer, container::gpu::kShadowCascadeCount>>
	  drawCountBuffers_{};
  std::vector<uint32_t> drawCapacities_{};

  container::gpu::ComputePipeline shadowCullPipeline_{};
  VkPipelineLayout      shadowCullPipelineLayout_{VK_NULL_HANDLE};
  VkDescriptorSetLayout shadowCullSetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool      shadowCullPool_{VK_NULL_HANDLE};
	std::vector<VkDescriptorSet> shadowCullSets_{};
};

}  // namespace container::renderer
