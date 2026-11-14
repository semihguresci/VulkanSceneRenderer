#pragma once

#include "Container/utility/ComputePipeline.h"

#include "Container/common/CommonVMA.h"
#include "Container/common/CommonVulkan.h"
#include "Container/geometry/OpenVdbVolume.h"
#include "Container/renderer/effects/OpenVdbVolumeSettings.h"
#include "Container/renderer/effects/OpenVdbVolumeGpuData.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <filesystem>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace container::gpu {
class VulkanDevice;
class AllocationManager;
class PipelineManager;
}

namespace container::renderer {
class FrameResourceManager;
class ShadowManager;
struct FrameRecordParams;

// Bounded dense/native media, combined in one raymarch. GPU ownership and
// descriptor lifetimes follow frame resources; frame replacement keeps pipelines.
class OpenVdbVolumeRenderer {
public:
  class PreparedSceneBindings {
  public:
    PreparedSceneBindings() noexcept;
    ~PreparedSceneBindings();
    PreparedSceneBindings(PreparedSceneBindings&&) noexcept;
    PreparedSceneBindings& operator=(PreparedSceneBindings&&) noexcept;
    PreparedSceneBindings(const PreparedSceneBindings&) = delete;
    PreparedSceneBindings& operator=(const PreparedSceneBindings&) = delete;
    explicit operator bool() const noexcept;
  private:
    friend class OpenVdbVolumeRenderer;
    struct State;
    std::unique_ptr<State> state_;
  };
  OpenVdbVolumeRenderer(std::shared_ptr<container::gpu::VulkanDevice> device,
                       container::gpu::AllocationManager& allocator,
                       container::gpu::PipelineManager& pipelines,
                       VkCommandPool commandPool);
  ~OpenVdbVolumeRenderer();
  OpenVdbVolumeRenderer(const OpenVdbVolumeRenderer&) = delete;
  OpenVdbVolumeRenderer& operator=(const OpenVdbVolumeRenderer&) = delete;
  void initialize(const std::filesystem::path& shaderRoot,
                  const container::geometry::OpenVdbVolume& volume,
                  float densityScale = 1.0f);
  void initialize(const std::filesystem::path& shaderRoot,
                  std::span<const container::geometry::OpenVdbVolume> volumes,
                  const OpenVdbVolumeSettings& settings = {});
  // Synchronously retires graphics readers, uploads the new frame(s), and
  // rewrites texture descriptors while retaining pipelines/frame resources.
  void replaceVolumes(std::span<const container::geometry::OpenVdbVolume> volumes);
  void replaceVolume(uint32_t index, const container::geometry::OpenVdbVolume& volume);
  // Main-thread-only staged replacement. A successful begin owns a copy of the
  // payload; the CPU source may immediately be released. At most one candidate
  // is retained. These calls never wait for a queue, device, or upload fence.
  // begin returns false while busy; ready/publish/discard return false when no
  // candidate exists or its upload is unfinished. Failures throw and preserve
  // the currently displayed medium.
  bool beginReplaceVolume(uint32_t index, const container::geometry::OpenVdbVolume& volume);
  [[nodiscard]] bool volumeReplacementPending() const noexcept;
  [[nodiscard]] bool volumeReplacementReady() const;
  // Caller MUST retire all graphics readers before publication, since all
  // swapchain descriptor sets are rewritten and the old medium is destroyed.
  bool publishVolumeReplacement();
  // Discarding only destroys the unpublished candidate; no reader wait needed.
  bool discardVolumeReplacement();
  // Caller retires GPU readers before replacing swapchain descriptors.
  void prepare(const FrameResourceManager& frames,
               std::span<const container::gpu::AllocatedBuffer> cameras,
               std::span<const container::gpu::AllocatedBuffer> lighting,
               std::span<const container::gpu::AllocatedBuffer> sceneClips,
               const container::gpu::AllocatedBuffer& pointLights,
               const container::gpu::AllocatedBuffer& areaLights,
               const ShadowManager& shadows);
  // Stage only bindings 1/6/7/15 against typed lighting/clip/point/area buffers.
  // No media upload, pool replacement, live write or medium-buffer changes.
  // Active descriptor tables require one lighting and clip buffer per existing set;
  // a renderer with no prepared sets produces a valid no-op token.
  // Buffers are borrowed: retain their owning generations through GPU readers.
  [[nodiscard]] PreparedSceneBindings prepareSceneBindings(
      std::span<const container::gpu::AllocatedBuffer> lighting,
      std::span<const container::gpu::AllocatedBuffer> sceneClips,
      const container::gpu::AllocatedBuffer& pointLights,
      const container::gpu::AllocatedBuffer& areaLights) const;
  [[nodiscard]] bool canPublishSceneBindings(const PreparedSceneBindings&) const noexcept;
  // Caller retires graphics/DDGI readers. All identity checks precede writes;
  // success consumes the token using fixed stack Vulkan writes, with no
  // allocation, map/flush, upload or GPU wait.
  bool publishSceneBindings(PreparedSceneBindings&) noexcept;
  void setSettings(const OpenVdbVolumeSettings& settings) noexcept {
    settings_ = sanitizeOpenVdbVolumeSettings(settings);
  }
  [[nodiscard]] const OpenVdbVolumeSettings& settings() const noexcept { return settings_; }
  void setVolumeSettings(uint32_t index, const OpenVdbVolumeSettings& settings);
  void clearVolumeSettings(uint32_t index);
  [[nodiscard]] uint32_t volumeCount() const noexcept { return static_cast<uint32_t>(volumes_.size()); }
  // Exactly mirrors record() eligibility. Its per-pixel alpha coverage is only
  // consumed by temporal composition when this predicate is true.
  [[nodiscard]] bool records(const FrameRecordParams& frame) const noexcept;
  // OIT interleaving requires both this frame's clear and later resolve passes
  // to be active. Standalone callers default to preserving the original lists.
  void record(VkCommandBuffer commandBuffer, const FrameRecordParams& frame,
              bool transparentOitActive = false) const;
  [[nodiscard]] const std::array<float, 3>& minimum() const noexcept { return minimum_; }
  [[nodiscard]] const std::array<float, 3>& maximum() const noexcept { return maximum_; }

private:
  struct MediumResources {
    VkImage densityImage{VK_NULL_HANDLE};
    VmaAllocation densityAllocation{nullptr};
    VkImageView densityView{VK_NULL_HANDLE};
    VkImage temperatureImage{VK_NULL_HANDLE};
    VmaAllocation temperatureAllocation{nullptr};
    VkImageView temperatureView{VK_NULL_HANDLE};
    container::gpu::AllocatedBuffer sparseDensity{}; // packed density + temperature
    uint32_t temperatureByteOffset{0};
    float importScale{1.0f};
    std::array<float, 3> minimum{}, maximum{};
    float stepSize{0.01f};
  };
  struct PendingUpload;
  std::unique_ptr<PendingUpload> submitVolumeUpload(const container::geometry::OpenVdbVolume& volume);
  void destroyUpload(PendingUpload& upload, bool waitForCompletion) noexcept;
  MediumResources uploadVolume(const container::geometry::OpenVdbVolume& volume);
  void destroyMedium(MediumResources& medium) noexcept;
  void updateTextureDescriptors();
  void initializeTransparencyFallback();
  void updateBounds();
  std::shared_ptr<container::gpu::VulkanDevice> device_;
  container::gpu::AllocationManager& allocator_;
  container::gpu::PipelineManager& pipelines_;
  // Dedicated pool: frame command-buffer recreation cannot invalidate uploads.
  VkCommandPool commandPool_{VK_NULL_HANDLE};
  std::unique_ptr<PendingUpload> pendingUpload_;
  std::vector<MediumResources> volumes_;
  std::vector<container::gpu::AllocatedBuffer> mediumBuffers_;
  container::gpu::AllocatedBuffer dummySparseBuffer_{};
  VkImage dummyOitHeadImage_{VK_NULL_HANDLE};
  VmaAllocation dummyOitHeadAllocation_{nullptr};
  VkImageView dummyOitHeadView_{VK_NULL_HANDLE};
  std::array<std::optional<OpenVdbVolumeSettings>, kMaxOpenVdbVolumes> volumeSettings_{};
  VkSampler sampler_{VK_NULL_HANDLE};
  VkDescriptorSetLayout setLayout_{VK_NULL_HANDLE};
  VkDescriptorPool descriptorPool_{VK_NULL_HANDLE};
  std::vector<VkDescriptorSet> sets_;
  std::shared_ptr<const uint8_t> descriptorBindingOwner_{std::make_shared<const uint8_t>(0)};
  uint64_t descriptorBindingEpoch_{1};
  VkPipelineLayout layout_{VK_NULL_HANDLE};
  container::gpu::ComputePipeline pipeline_{};
  std::array<float, 3> minimum_{}, maximum_{};
  OpenVdbVolumeSettings settings_{};
};
} // namespace container::renderer
