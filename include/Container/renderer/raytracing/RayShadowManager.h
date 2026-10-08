#pragma once

#include "Container/renderer/raytracing/RaySceneExtraction.h"
#include "Container/renderer/raytracing/RayShadowSettings.h"
#include "Container/renderer/resources/FrameResources.h"
#include "Container/utility/VulkanDevice.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <filesystem>
#include <functional>
#include <memory>

namespace container::renderer {
class RenderGraphBuilder;
struct FrameRecordParams;
struct RaySceneBuildStats;

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
  void prepare(VkExtent2D extent, uint32_t imageCount);
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
  [[nodiscard]] uint64_t allocatedBytes() const;
  [[nodiscard]] const RaySceneBuildStats *buildStats() const;
  [[nodiscard]] uint64_t sceneGeneration() const;
  [[nodiscard]] uint64_t historyResets() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
void registerRayShadowPasses(RenderGraphBuilder &graph);
} // namespace container::renderer
