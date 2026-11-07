#pragma once

#include "Container/renderer/raytracing/RaySceneExtraction.h"
#include "Container/renderer/raytracing/RayShadowSettings.h"
#include "Container/renderer/resources/FrameResources.h"
#include "Container/utility/VulkanDevice.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace container::renderer {
class RenderGraphBuilder;
struct RayEffectPipelines;
struct FrameRecordParams;
struct RaySceneBuildStats;

// Borrowed handles from a fence-retained scene generation. Valid after
// recordBuild for this image and until that image's next fence retirement.
// Compact visibility metadata retains its default 48/16-byte layouts; closest
// hit surface attributes are allocated only for an opted-in consumer.
struct RaySurfaceGpuBindings {
  VkAccelerationStructureKHR tlas{};
  VkBuffer triangles{};
  VkDeviceSize trianglesSize{};
  VkBuffer instances{};
  VkDeviceSize instancesSize{};
  VkBuffer surfaceTriangles{};
  VkDeviceSize surfaceTrianglesSize{};
  VkBuffer surfaceInstances{};
  VkDeviceSize surfaceInstancesSize{};
  uint32_t triangleCount{}, instanceCount{};
  [[nodiscard]] explicit operator bool() const {
    return tlas && triangles && instances && surfaceTriangles && surfaceInstances;
  }
};

// Retains the positions and transforms associated with one submitted ray scene.
// The topology hash excludes pose changes and includes the ordered triangle
// corner and instance identities used to reconstruct previous hit positions.
struct RaySurfaceMotionSnapshot {
  std::shared_ptr<const void> lifetime;
  uint64_t topology{};
};

// Graphics-queue AS generations and independent visibility history. The
// frontend calls prepare only after resize idle, and update after image fence
// retirement; generations are retained per swapchain image through its fence.
class RayShadowManager {
public:
  RayShadowManager(std::shared_ptr<container::gpu::VulkanDevice> device,
                   container::gpu::VulkanMemoryManager &memory);
  ~RayShadowManager();
  RayShadowManager(const RayShadowManager &) = delete;
  RayShadowManager &operator=(const RayShadowManager &) = delete;
  void createPipelines(const std::filesystem::path &root,
                       VkDescriptorSetLayout scene,
                       VkDescriptorSetLayout light);
  [[nodiscard]] RayEffectPipelines prepareShaderReload(const std::filesystem::path& root) const;
  void publishShaderReload(RayEffectPipelines candidate) noexcept;
  void prepare(VkExtent2D extent, uint32_t imageCount);
  void setGpuTransformProducer(class RayInstanceTransformPass* producer);
  // Independent pipelines, descriptors and upload slots, sharing only immutable
  // old AS/geometry generations for optional build reuse. Update and recordBuild
  // every image on the candidate, then submit/retire that build before exchange.
  // Destruction before exchange leaves the active scene unchanged.
  [[nodiscard]] std::unique_ptr<RayShadowManager> createDetachedGeneration() const;
  [[nodiscard]] bool canExchangeGeneration(const RayShadowManager& other) const noexcept;
  // Retire graphics readers and asynchronous surface consumers first. The
  // manager object address stays stable for consumers retaining its reference.
  [[nodiscard]] bool exchangeGeneration(RayShadowManager& other) noexcept;
  // Call before update. Surface consumers share the shadow AS, including when
  // shadow mode is Raster, without enabling shadow tracing/history images.
  void requestSurfaceScene(bool requested,
                           uint64_t budgetBytes = 128ull * 1024 * 1024);
  void update(uint32_t image, const container::gpu::CameraData &camera,
              const FrameResources &frame, VkDescriptorSet sceneSet,
              VkDescriptorSet lightSet,
              std::span<const RaySceneProviderSource> sources,
              const container::gpu::LightingData &lighting,
              std::span<const container::gpu::PointLightData> points,
              std::span<const container::gpu::AreaLightData> areas,
              uint32_t sectionPlaneEnabled, glm::vec4 sectionPlane,
              const container::gpu::SceneClipState &boxClip,
              const std::function<uint64_t(uint32_t)> &materialRevision,
              bool geometryCompatible = true);
  void recordBuild(VkCommandBuffer cmd, uint32_t image);
  void recordTrace(VkCommandBuffer cmd, uint32_t image);
  void recordFilter(VkCommandBuffer cmd, uint32_t image);
  void commit();
  RayShadowSettings &settings();
  [[nodiscard]] bool supported() const;
  [[nodiscard]] bool active() const;
  [[nodiscard]] VkBuffer readSettings(uint32_t image) const;
  [[nodiscard]] VkDeviceSize readSettingsSize() const;
  [[nodiscard]] VkImageView visibilityView() const;
  // Layout is available once supported pipelines exist. The per-image set is
  // populated by recordBuild before fragment/compute query consumers execute.
  [[nodiscard]] VkDescriptorSetLayout traceDescriptorLayout() const;
  [[nodiscard]] VkDescriptorSet traceDescriptorSet(uint32_t image) const;
  [[nodiscard]] RaySurfaceGpuBindings surfaceBindings(uint32_t image) const;
  [[nodiscard]] RaySurfaceMotionSnapshot surfaceMotionSnapshot(uint32_t image) const;
  // Host metadata for the same fence-retained surface generation.
  [[nodiscard]] std::span<const RayInstanceData> surfaceInstanceMetadata(uint32_t image) const;
  [[nodiscard]] const std::string &surfaceUnsupportedReason() const;
  [[nodiscard]] uint64_t allocatedBytes() const;
  [[nodiscard]] const RaySceneBuildStats *buildStats() const;
  [[nodiscard]] uint64_t sceneGeneration() const;
  // Camera/light-independent content revision, available after update. Zero
  // means no compatible ray scene is enabled for either consumer.
  [[nodiscard]] uint64_t sceneRevision() const;
  [[nodiscard]] uint64_t historyResets() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
void registerRayShadowPasses(RenderGraphBuilder &graph);
} // namespace container::renderer
