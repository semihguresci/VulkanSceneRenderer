#pragma once

#include "Container/utility/ComputePipeline.h"

#include "Container/common/CommonVulkan.h"
#include "Container/common/CommonMath.h"
#include "Container/renderer/debug/DebugOverlayRenderer.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/VulkanMemoryManager.h"

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

// Culling statistics — available one frame after dispatch (no GPU stall).
struct CullStats {
  uint32_t totalInputCount{0};        // Objects submitted for culling.
  uint32_t frustumPassedCount{0};     // Objects that passed frustum culling.
  uint32_t occlusionPassedCount{0};   // Objects that passed occlusion culling.
};

// Manages GPU-driven indirect draw buffers, frustum culling compute
// pipeline, Hi-Z mip chain, and occlusion culling compute pipeline.
class GpuCullManager {
 public:
  GpuCullManager(
      std::shared_ptr<container::gpu::VulkanDevice> device,
      container::gpu::AllocationManager&            allocationManager,
      container::gpu::PipelineManager&            pipelineManager);

  ~GpuCullManager();
  GpuCullManager(const GpuCullManager&) = delete;
  GpuCullManager& operator=(const GpuCullManager&) = delete;

  class PreparedGeneration {
   public:
    PreparedGeneration() noexcept;
    ~PreparedGeneration();
    PreparedGeneration(PreparedGeneration&&) noexcept;
    PreparedGeneration& operator=(PreparedGeneration&&) noexcept;
    PreparedGeneration(const PreparedGeneration&) = delete;
    PreparedGeneration& operator=(const PreparedGeneration&) = delete;
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] uint32_t maxDrawCount() const noexcept;
    [[nodiscard]] VkBuffer indirectDrawBuffer(uint32_t image) const noexcept;
    [[nodiscard]] VkBuffer drawCountBuffer(uint32_t image) const noexcept;
    [[nodiscard]] GpuCullManager& manager();
   private:
    friend class GpuCullManager;
    struct State;
    std::unique_ptr<State> state_;
  };
  // Detached buffer/descriptor preparation sharing immutable pipelines and the
  // live Hi-Z/frozen-camera resources. Command storage must survive publication
  // to retain the upload cache. Retire readers before exchange/destruction and
  // destroy prepared histories before this manager and its shared layouts.
  [[nodiscard]] PreparedGeneration prepareGeneration(
      std::span<const container::gpu::AllocatedBuffer> objects,
      std::span<const size_t> objectCapacities,
      std::span<const container::gpu::AllocatedBuffer> cameras,
      VkDeviceSize cameraBytes,std::span<const DrawCommand> commands,
      uint64_t sourceRevision) const;
  [[nodiscard]] bool canExchangeGeneration(const PreparedGeneration&) const noexcept;
  bool exchangeGeneration(PreparedGeneration&) noexcept;

  // Create compute pipelines and descriptor resources.
  void createResources(const std::filesystem::path& shaderDir,
                       uint32_t descriptorSetCount = 1);
  // Disable binding replay when the caller is replacing the object buffers.
  // The new sets need current object bindings before dispatch.
  void recreatePerFrameResources(uint32_t descriptorSetCount,
                                 bool preserveObjectBindings = true);

  // Ensure indirect draw buffers are large enough for the given object count.
  // Returns true if buffers were recreated (descriptor sets need re-write).
  bool ensureBufferCapacity(uint32_t maxObjectCount);

  // Upload CPU-side draw commands to the input SSBO and prepare for culling.
  void uploadDrawCommands(uint32_t imageIndex,
                          const std::vector<DrawCommand>& commands,
                          uint64_t sourceRevision);
  void uploadDrawCommands(uint32_t imageIndex,std::span<const DrawCommand> commands,
                          uint64_t sourceRevision);

  // Reset per-frame validity bits before recording the render graph.
  void beginFrameCulling(uint32_t imageIndex);

  // Dispatch frustum culling compute shader.  After this call, the indirect
  // draw buffer and draw count buffer are ready for vkCmdDrawIndexedIndirect.
  void dispatchFrustumCull(VkCommandBuffer cmd,
                           uint32_t imageIndex,
                           VkBuffer cameraBuffer,
                           VkDeviceSize cameraBufferSize,
                           uint32_t objectCount);

  // Dispatch Hi-Z mip chain generation from depth image.
  void dispatchHiZGenerate(VkCommandBuffer cmd,
                           uint32_t imageIndex,
                           VkImageView depthView,
                           VkSampler depthSampler,
                           uint32_t width, uint32_t height);

  // Dispatch occlusion culling against Hi-Z pyramid.
  void dispatchOcclusionCull(VkCommandBuffer cmd,
                             uint32_t imageIndex,
                             VkBuffer cameraBuffer,
                             VkDeviceSize cameraBufferSize,
                             uint32_t objectCount);

  // Issue a single vkCmdDrawIndexedIndirectCount or equivalent.
  // Uses frustum-culled results (depth prepass + shadow passes).
  void drawIndirect(VkCommandBuffer cmd, uint32_t imageIndex) const;

  // Issue indirect draw from occlusion-culled results for optional consumers.
  void drawIndirectOccluded(VkCommandBuffer cmd, uint32_t imageIndex) const;

  // Ensure the Hi-Z image matches the given depth buffer dimensions.
  // Call once per swapchain resize.
  void ensureHiZImage(uint32_t imageIndex, uint32_t width, uint32_t height);

  // Access the indirect draw count (for secondary passes like shadow that
  // don't do occlusion culling, we use the frustum-culled count).
  VkBuffer indirectDrawBuffer(uint32_t imageIndex) const {
    return imageIndex < indirectDrawBuffers_.size()
               ? indirectDrawBuffers_[imageIndex].buffer
               : VK_NULL_HANDLE;
  }
  VkBuffer drawCountBuffer(uint32_t imageIndex) const {
    return imageIndex < drawCountBuffers_.size()
               ? drawCountBuffers_[imageIndex].buffer
               : VK_NULL_HANDLE;
  }
  uint32_t maxDrawCount() const { return maxObjectCount_; }

  [[nodiscard]] bool isReady() const;
  [[nodiscard]] bool occlusionCullResourcesReady(uint32_t imageIndex) const;
  [[nodiscard]] bool canRecordOcclusionCull(uint32_t imageIndex) const;
  [[nodiscard]] bool frustumDrawsValid(uint32_t imageIndex) const {
    return imageIndex < frustumDrawsValid_.size() &&
           frustumDrawsValid_[imageIndex];
  }
  [[nodiscard]] bool hizGeneratedThisFrame(uint32_t imageIndex) const {
    return imageIndex < hizFrames_.size() &&
           hizFrames_[imageIndex].generatedThisFrame;
  }
  // Borrowed handle, valid until the Hi-Z images are recreated or destroyed.
  [[nodiscard]] VkImage hiZImage(uint32_t imageIndex) const {
    return imageIndex < hizFrames_.size() ? hizFrames_[imageIndex].image
                                         : VK_NULL_HANDLE;
  }
  [[nodiscard]] bool occlusionDrawsValid(uint32_t imageIndex) const {
    return imageIndex < occlusionDrawsValid_.size() &&
           occlusionDrawsValid_[imageIndex];
  }

  // Update the object SSBO descriptor (binding 1) to point at the scene's
  // object buffer.  Call whenever the object buffer is recreated.
  void updateObjectSsboDescriptor(VkBuffer objectBuffer,
                                  VkDeviceSize objectBufferSize);
  void updateObjectSsboDescriptor(uint32_t imageIndex,
                                  VkBuffer objectBuffer,
                                  VkDeviceSize objectBufferSize);

  // Retrieve culling statistics from the last retired image with a readback.
  // Returns all zeros until the first readback completes.
  CullStats cullStats() const { return lastStats_; }

  // Schedule a readback of the draw count buffers into staging memory.
  // Call once per frame after all cull dispatches complete.
  void scheduleStatsReadback(VkCommandBuffer cmd, uint32_t imageIndex);

  // Collect only the acquired image's readback after its graphics fence.
  void collectStats(uint32_t imageIndex);

  // Freeze the culling camera: subsequent cull dispatches will use a
  // snapshot of the current camera data instead of the live camera.
  // Pass the current camera buffer contents to capture.
  void freezeCulling(uint32_t imageIndex,
                     VkCommandBuffer cmd,
                     VkBuffer liveCameraBuffer,
                     VkDeviceSize cameraBufferSize);

  // Unfreeze: go back to using the live camera for culling.
  void unfreezeCulling();

  // True when culling is using a frozen camera snapshot.
  [[nodiscard]] bool cullingFrozen() const { return cullingFrozen_; }

  // When frozen, returns the frozen camera buffer; otherwise VK_NULL_HANDLE.
  [[nodiscard]] VkBuffer frozenCameraBuffer(uint32_t imageIndex) const {
    if (!cullingFrozen_ || imageIndex >= frozenCameraBuffers_.size()) {
      return VK_NULL_HANDLE;
    }
    const auto& frozenCameraBuffer = frozenCameraBuffers_[imageIndex];
    return frozenCameraBuffer.buffer;
  }

 private:
  void createFrustumCullPipeline(const std::filesystem::path& shaderDir);
  void createHiZPipeline(const std::filesystem::path& shaderDir);
  void createOcclusionCullPipeline(const std::filesystem::path& shaderDir);
  void allocateFrustumCullDescriptorSets(uint32_t descriptorSetCount);
  void allocateOcclusionCullDescriptorSets(uint32_t descriptorSetCount);
  void createHiZDescriptorSets();
  void writeDescriptorSets();
  void destroyHiZImage();
  void destroyCullBuffers();
  void destroyFrozenCameraBuffers();
  void resizePerImageState(uint32_t imageCount);
  void createHiZSampler();
  void destroyGenerationPool(VkDescriptorPool& pool);

  struct HiZFrameResources {
    VkImage image{VK_NULL_HANDLE};
    VmaAllocation allocation{nullptr};
    VkImageView fullView{VK_NULL_HANDLE};
    std::vector<VkImageView> mipViews{};
    std::vector<VkDescriptorSet> descriptorSets{};
    bool initialized{false};
    bool generatedThisFrame{false};
  };

  std::shared_ptr<container::gpu::VulkanDevice> device_;
  container::gpu::AllocationManager&            allocationManager_;
  container::gpu::PipelineManager&            pipelineManager_;
  bool ownsSharedResources_{true};
  bool ownsGenerationPools_{false};

  uint32_t maxObjectCount_{0};
  std::vector<container::gpu::GpuDrawIndexedIndirectCommand> uploadScratch_{};
  std::vector<const DrawCommand *> lastUploadSourceData_{};
  std::vector<size_t> lastUploadSourceSize_{};
  std::vector<uint64_t> lastUploadSourceRevision_{};

  // Input: draw commands + object bounding spheres (read by cull shaders).
  std::vector<container::gpu::AllocatedBuffer> inputDrawBuffers_{};   // DrawCommand[]
  std::vector<VkBuffer> objectSsboBuffers_{};
  std::vector<VkDeviceSize> objectSsboSizes_{};

  // Output: VkDrawIndexedIndirectCommand[] written by cull shader.
  std::vector<container::gpu::AllocatedBuffer> indirectDrawBuffers_{};
  // Output: draw count (single uint32).
  std::vector<container::gpu::AllocatedBuffer> drawCountBuffers_{};

  // Second output: occlusion-culled indirect commands for G-Buffer pass.
  std::vector<container::gpu::AllocatedBuffer> occlusionIndirectBuffers_{};
  std::vector<container::gpu::AllocatedBuffer> occlusionCountBuffers_{};

  // Frustum cull compute pipeline.
  container::gpu::ComputePipeline frustumCullPipeline_{};
  VkPipelineLayout     frustumCullPipelineLayout_{VK_NULL_HANDLE};
  VkDescriptorSetLayout frustumCullSetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool     frustumCullPool_{VK_NULL_HANDLE};
  std::vector<VkDescriptorSet> frustumCullSets_{};

  // Hi-Z generation.
  container::gpu::ComputePipeline hizPipeline_{};
  VkPipelineLayout     hizPipelineLayout_{VK_NULL_HANDLE};
  VkDescriptorSetLayout hizSetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool     hizPool_{VK_NULL_HANDLE};
  std::vector<HiZFrameResources> hizFrames_{};
  VkSampler            hizSampler_{VK_NULL_HANDLE};
  uint32_t             hizWidth_{0};
  uint32_t             hizHeight_{0};
  uint32_t             hizMipLevels_{0};

  // Occlusion cull compute pipeline.
  container::gpu::ComputePipeline occlusionCullPipeline_{};
  VkPipelineLayout     occlusionCullPipelineLayout_{VK_NULL_HANDLE};
  VkDescriptorSetLayout occlusionCullSetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool     occlusionCullPool_{VK_NULL_HANDLE};
  std::vector<VkDescriptorSet> occlusionCullSets_{};

  // Stats readback belongs to the swapchain image that produced it.
  std::vector<container::gpu::AllocatedBuffer> statsReadbackBuffers_{};  // 2 x uint32_t, HOST_VISIBLE
  std::vector<bool> statsReadbackSubmitted_{};
  std::vector<uint32_t> statsInputCounts_{};
  CullStats lastStats_{};

  // Freeze-culling: snapshot of camera data used for cull dispatches.
  std::vector<container::gpu::AllocatedBuffer> frozenCameraBuffers_{};  // CameraData, GPU-only
  bool cullingFrozen_{false};

  std::vector<bool> frustumDrawsValid_{};
  std::vector<bool> occlusionDrawsValid_{};
};

}  // namespace container::renderer
