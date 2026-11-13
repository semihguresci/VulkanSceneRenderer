#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/lighting/DdgiSettings.h"
#include "Container/renderer/lighting/DdgiPriorityScheduler.h"
#include "Container/utility/SceneData.h"
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace container::gpu {
class VulkanDevice;
class VulkanMemoryManager;
} // namespace container::gpu
namespace container::renderer {
class RayShadowManager;
class DdgiDebugRenderer;
struct RayEffectPipelines;
class RenderGraphBuilder;

// Bounded packed world-space volumes. Optional second-queue updates publish
// immutable completed history while graphics reads the previous snapshot.
// prepare retires both queues; update touches only a retired image's uploads
// and descriptor sets. Shared AS generations remain owned by RayShadowManager.
class DdgiManager {
public:
  DdgiManager(std::shared_ptr<container::gpu::VulkanDevice> device,
              container::gpu::VulkanMemoryManager &memory,
              RayShadowManager &rayScene, DdgiSettings settings);
  ~DdgiManager();
  DdgiManager(const DdgiManager &) = delete;
  DdgiManager &operator=(const DdgiManager &) = delete;
  void createPipelines(const std::filesystem::path &root,
                       VkDescriptorSetLayout scene, VkDescriptorSetLayout light,
                       VkDescriptorSetLayout postProcess = VK_NULL_HANDLE,
                       VkFormat colorFormat = VK_FORMAT_UNDEFINED);
  [[nodiscard]] RayEffectPipelines prepareShaderReload(const std::filesystem::path& root) const;
  void publishShaderReload(RayEffectPipelines candidate) noexcept;
  [[nodiscard]] std::unique_ptr<DdgiDebugRenderer> prepareDebugReload(const std::filesystem::path& root) const;
  [[nodiscard]] std::unique_ptr<DdgiDebugRenderer> prepareDebugReload(
      const std::filesystem::path& root, VkFormat colorFormat) const;
  // Retire graphics readers before replacing the debug presentation pipeline.
  void publishDebugReload(std::unique_ptr<DdgiDebugRenderer> candidate) noexcept;
  // Call between frames, before assembling their descriptor snapshots. Invalid
  // settings leave the current configuration untouched. Allocation changes
  // retire the device and rebuild; live controls retain history. Camera-follow
  // cell moves remap history on the GPU without reallocating.
  void applySettings(const DdgiSettings &settings);
  // Call between frames when replacing/unloading the scene. Retires the async
  // writer and disables old irradiance immediately; the next graphics trace
  // clears both atlases/states even while paused, without reallocating them.
  void resetSceneHistory();
  // All graphics and asynchronous readers must be retired before replacement.
  void replaceEnvironment(VkImageView environment, VkSampler sampler);
  class PreparedSceneHistoryReset {
  public:
    PreparedSceneHistoryReset() = default;
    PreparedSceneHistoryReset(PreparedSceneHistoryReset&&) noexcept;
    PreparedSceneHistoryReset& operator=(PreparedSceneHistoryReset&&) noexcept;
    PreparedSceneHistoryReset(const PreparedSceneHistoryReset&) = delete;
    PreparedSceneHistoryReset& operator=(const PreparedSceneHistoryReset&) = delete;
  private:
    friend class DdgiManager;
    const void* owner_{};
    size_t probeCount_{};
    bool ready_{}, enabled_{};
    DdgiPriorityScheduler scheduler_;
    std::vector<bool> fresh_;
    std::string status_;
  };
  // Staging allocates only CPU reset state and leaves the live field unchanged.
  // Serialize settings/scene edits and synchronizeAsync before publication.
  [[nodiscard]] PreparedSceneHistoryReset prepareSceneHistoryReset() const;
  // Borrowed CPU admission for a coexisting light-edit reset and per-image
  // snapshot-capacity preparation. Excludes GPU storage, which has its own
  // coexistence budget. Does not allocate, snapshot or mutate live DDGI state.
  [[nodiscard]] uint64_t lightEditPreparationCpuBytes() const;
  [[nodiscard]] bool canPublishSceneHistoryReset(
      const PreparedSceneHistoryReset& reset) const noexcept;
  [[nodiscard]] bool publishSceneHistoryReset(
      PreparedSceneHistoryReset& reset) noexcept;
  class PreparedLightSnapshotCapacity {
  public:
    PreparedLightSnapshotCapacity() noexcept;
    ~PreparedLightSnapshotCapacity();
    PreparedLightSnapshotCapacity(PreparedLightSnapshotCapacity&&) noexcept;
    PreparedLightSnapshotCapacity& operator=(PreparedLightSnapshotCapacity&&) noexcept;
    PreparedLightSnapshotCapacity(const PreparedLightSnapshotCapacity&) = delete;
    PreparedLightSnapshotCapacity& operator=(const PreparedLightSnapshotCapacity&) = delete;
    explicit operator bool() const noexcept;
  private:
    friend class DdgiManager;
    struct State;
    std::unique_ptr<State> state_;
  };
  // Prepare every image's point/area snapshot capacity before a scene commit.
  // Growth stays detached until all allocations fit the storage and aggregate
  // VMA budget (including temporary coexistence with the old buffers). A
  // discarded/failed preparation changes no live resources or history state.
  [[nodiscard]] PreparedLightSnapshotCapacity prepareLightSnapshotCapacity(
      size_t pointCount, size_t areaCount) const;
  // Caller retires graphics readers and synchronizeAsync before publication.
  // No allocation/upload/map/flush or timeline/history reset occurs here. The
  // consumed token retains old buffers until it is destroyed after retirement.
  [[nodiscard]] bool canPublishLightSnapshotCapacity(
      const PreparedLightSnapshotCapacity&) const noexcept;
  [[nodiscard]] bool publishLightSnapshotCapacity(
      PreparedLightSnapshotCapacity&) noexcept;
  void prepare(uint32_t imageCount, VkImageView environment, VkSampler sampler);
  void update(uint32_t image, VkDescriptorSet scene, VkDescriptorSet light,
              const container::gpu::LightingData &lighting,
              std::span<const container::gpu::PointLightData> points,
              std::span<const container::gpu::AreaLightData> areas,
              uint32_t sectionEnabled, glm::vec4 sectionPlane,
              const container::gpu::SceneClipState &boxClip,
              VkImageView environment, VkSampler sampler,
              bool receiverCompatible = true,
              const DdgiSchedulingHints &hints = {});
  void recordTrace(VkCommandBuffer cmd, uint32_t image);
  void recordUpdate(VkCommandBuffer cmd, uint32_t image);
  // Called inside the swapchain rendering scope, after tone mapping and before
  // UI. Uses the retired image's descriptors; off/unavailable states skip
  // draws.
  void recordDebug(VkCommandBuffer cmd, uint32_t image,
                   VkDescriptorSet postProcess);
  void commit();
  // Retire an image before replacing any borrowed scene/light descriptor or AS
  // generation. Its graphics fence alone does not cover the async writer.
  void retireImage(uint32_t image);
  void synchronizeAsync();
  [[nodiscard]] bool asyncActive() const;
  [[nodiscard]] uint64_t asyncSubmittedUpdates() const;
  [[nodiscard]] uint64_t asyncPublishedUpdates() const;
  [[nodiscard]] std::optional<VkSemaphoreSubmitInfo> graphicsWait() const;
  [[nodiscard]] std::optional<VkSemaphoreSubmitInfo> graphicsSignal() const;
  // Call after a successful graphics submission and commit. That submission
  // publishes scene-build and input-upload dependencies to the compute queue.
  void submitAsync(uint32_t image);
  [[nodiscard]] bool wantsScene() const;
  [[nodiscard]] bool supported() const;
  [[nodiscard]] bool active() const;
  [[nodiscard]] VkDescriptorSetLayout descriptorLayout() const;
  [[nodiscard]] VkDescriptorSet readDescriptors(uint32_t image) const;
  [[nodiscard]] uint64_t allocatedBytes() const;
  [[nodiscard]] uint64_t historyResets() const;
  [[nodiscard]] uint64_t frameNumber() const;
  [[nodiscard]] uint32_t contentEpoch() const;
  [[nodiscard]] uint32_t refreshRemainingProbes() const;
  // Next probe after the last submitted batch; retained while paused/disabled.
  [[nodiscard]] uint32_t schedulingCursor() const;
  [[nodiscard]] DdgiProbeVolumeDesc currentPrimaryVolume() const;
  [[nodiscard]] uint64_t scrollCount() const;
  [[nodiscard]] uint32_t lastScrollPreservedProbes() const;
  [[nodiscard]] DdgiUpdateBatch lastBatch() const;
  [[nodiscard]] std::span<const DdgiProbeUpdate> lastUpdates() const;
  [[nodiscard]] const DdgiSettings &settings() const;
  [[nodiscard]] const std::string &status() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
void registerDdgiPasses(RenderGraphBuilder &graph);
} // namespace container::renderer
