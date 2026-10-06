#pragma once

#include "Container/common/CommonMath.h"
#include "Container/common/CommonVulkan.h"
#include "Container/renderer/lighting/EditableLight.h"
#include "Container/renderer/lighting/LightPushConstants.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/SceneGraph.h"
#include "Container/utility/TextureResource.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace container::gpu {
class AllocationManager;
class PipelineManager;
class VulkanDevice;
} // namespace container::gpu

namespace container::ecs {
class World;
} // namespace container::ecs

namespace container::scene {
class BaseCamera;
class SceneManager;
} // namespace container::scene

namespace container::renderer {

// Struct carrying the scene-space anchor used for light positioning.
struct SceneLightingAnchor {
  glm::mat4 sceneTransform{1.0f};
  glm::vec3 center{0.0f, 0.0f, 0.0f};
  float localRadius{1.0f};
  float worldRadius{1.0f};
};

// Manages the LightingData uniform buffer, directional + point/area lights,
// tiled light culling resources, and the light-gizmo draw helper.
class LightingManager {
public:
  LightingManager(std::shared_ptr<container::gpu::VulkanDevice> device,
                  container::gpu::AllocationManager &allocationManager,
                  container::gpu::PipelineManager &pipelineManager,
                  container::scene::SceneManager *sceneManager,
                  container::scene::SceneGraph &sceneGraph,
                  container::ecs::World &world);

  ~LightingManager();
  LightingManager(const LightingManager &) = delete;
  LightingManager &operator=(const LightingManager &) = delete;

  // ---- Lifecycle ----------------------------------------------------------

  // Trivial: sets lightVolumeIndexCount = 36.
  void createLightVolumeGeometry();

  // Creates lightDescriptorSetLayout, lightingBuffer, lightDescriptorPool,
  // lightDescriptorSet, and writes the initial descriptor.
  void createDescriptorResources(uint32_t descriptorSetCount);
  void updateRayShadowDescriptors(uint32_t imageIndex, VkBuffer settings,
                                  VkDeviceSize settingsSize,
                                  VkImageView visibility);

  // Creates the tiled light culling compute pipeline, descriptor sets,
  // and SSBO buffers.  Must be called after createDescriptorResources().
  void createTiledResources(const std::filesystem::path &shaderDir,
                            VkExtent2D initialExtent);
  void resizeTiledResources(VkExtent2D extent);

  // ---- Per-frame updates --------------------------------------------------

  // Recomputes CPU lighting state from the current scene anchor.
  void updateLightingData();
  void updateLightingData(const container::scene::BaseCamera *camera);
  void updateLightingDataForActiveCamera();
  // Uploads the compact lighting UBO and the point-light SSBO once per frame.
  void uploadLightingData() const;
  void uploadLightingData(uint32_t imageIndex) const;
  void collectStats();

  // Uploads point lights to the SSBO and dispatches the tile culling compute
  // shader.  Must be called between G-Buffer and Lighting passes.
  void dispatchTileCull(VkCommandBuffer cmd, VkExtent2D screenExtent,
                        VkBuffer cameraBuffer, VkDeviceSize cameraBufferSize,
                        VkImageView depthView, float cameraNear,
                        float cameraFar) const;
  void resetGpuTimers(VkCommandBuffer cmd, uint32_t frameSlot) const;
  void beginClusterCullTimer(VkCommandBuffer cmd) const;
  void endClusterCullTimer(VkCommandBuffer cmd) const;
  void beginClusteredLightingTimer(VkCommandBuffer cmd) const;
  void endClusteredLightingTimer(VkCommandBuffer cmd) const;

  // ---- Draw helpers -------------------------------------------------------

  // Records the directional-light gizmo + point-light gizmos into
  // commandBuffer.
  void
  drawLightGizmos(VkCommandBuffer commandBuffer,
                  const std::array<VkDescriptorSet, 2> &lightingDescriptorSets,
                  VkPipeline lightGizmoPipeline,
                  VkPipeline lightGizmoCoveragePipeline,
                  VkPipelineLayout lightingPipelineLayout,
                  const container::scene::BaseCamera *camera) const;
  void drawLightGizmoPickIds(
      VkCommandBuffer commandBuffer,
      const std::array<VkDescriptorSet, 2> &lightingDescriptorSets,
      VkPipeline lightGizmoPickPipeline,
      VkPipelineLayout lightingPipelineLayout,
      const container::scene::BaseCamera *camera) const;

  // ---- Accessors ----------------------------------------------------------

  const container::gpu::LightingData &lightingData() const {
    return lightingData_;
  }
  container::gpu::LightingData &lightingData() { return lightingData_; }
  const container::gpu::LightingSettings &lightingSettings() const {
    return lightingSettings_;
  }
  void setLightingSettings(const container::gpu::LightingSettings &settings);
  const container::gpu::LightCullingStats &lightCullingStats() const {
    return lastStats_;
  }
  uint32_t lightVolumeIndexCount() const { return lightVolumeIndexCount_; }

  // Descriptor set / buffer accessors (valid after createDescriptorResources())
  VkDescriptorSetLayout lightDescriptorSetLayout() const {
    return lightDescriptorSetLayout_;
  }
  VkDescriptorSet lightDescriptorSet(uint32_t imageIndex) const {
    return imageIndex < lightDescriptorSets_.size()
               ? lightDescriptorSets_[imageIndex]
               : VK_NULL_HANDLE;
  }
  VkDescriptorPool lightDescriptorPool() const { return lightDescriptorPool_; }
  const container::gpu::AllocatedBuffer &
  lightingBuffer(uint32_t imageIndex) const {
    return lightingBuffers_[imageIndex];
  }
  std::span<const container::gpu::AllocatedBuffer> lightingBuffers() const {
    return lightingBuffers_;
  }

  SceneLightingAnchor computeSceneLightingAnchor() const;
  glm::vec3 directionalLightPosition() const;

  // The root node must be kept in sync with the scene graph after
  // buildSceneGraph.
  void setRootNode(uint32_t rootNode) { rootNode_ = rootNode; }

  // ---- Tiled lighting accessors -------------------------------------------
  bool isTiledLightingReady() const {
    return tileCullPipeline_ != VK_NULL_HANDLE;
  }
  VkDescriptorSetLayout tiledDescriptorSetLayout() const {
    return tiledDescriptorSetLayout_;
  }
  VkDescriptorSet tiledDescriptorSet() const { return tiledDescriptorSet_; }
  VkDescriptorSetLayout lightGizmoIconDescriptorSetLayout() const {
    return lightGizmoIconDescriptorSetLayout_;
  }
  VkDescriptorSet lightGizmoIconDescriptorSet() const {
    return lightGizmoIconDescriptorSet_;
  }
  bool lightGizmoIconsReady() const { return lightGizmoIconsReady_; }

  // Returns the point light SSBO contents.  Updated each frame by
  // updateLightingData().
  const std::vector<container::gpu::PointLightData> &pointLightsSsbo() const {
    return pointLightsSsbo_;
  }
  const std::vector<container::gpu::AreaLightData> &areaLightsSsbo() const {
    return areaLightsSsbo_;
  }
  const std::vector<EditableLightEntity> &editableLights() const {
    return editableLights_;
  }
  EditableLightId selectedEditableLightId() const {
    return selectedEditableLight_;
  }
  [[nodiscard]] std::optional<EditableLightEntity>
  selectedEditableLight() const;
  void selectEditableLight(EditableLightId id);
  [[nodiscard]] bool updateEditableLight(const EditableLightEntity &entity);
  [[nodiscard]] EditableLightId addManualEditableLight(EditableLightType type);
  [[nodiscard]] bool translateSelectedEditableLight(const glm::vec3 &delta);
  [[nodiscard]] bool rotateSelectedEditableLight(const glm::vec3 &axis,
                                                 float degrees);
  [[nodiscard]] bool scaleSelectedEditableLight(float factor);

  // Tile grid SSBO accessors for debug visualization (heat map).
  VkBuffer tileGridBuffer() const { return tileGridSsbo_.buffer; }
  VkDeviceSize tileGridBufferSize() const {
    return sizeof(container::gpu::TileLightGrid) * maxClusterCount_;
  }

private:
  bool applyAuthoredDirectionalLight(const SceneLightingAnchor &anchor);
  void appendAuthoredPointLights(const SceneLightingAnchor &anchor);
  void appendAuthoredAreaLights(const SceneLightingAnchor &anchor);
  void applyEditableLightOverrides(EditableLightSource directionalSource,
                                   EditableLightSource pointSource,
                                   size_t sourcePointCount,
                                   EditableLightSource areaSource,
                                   size_t sourceAreaCount);
  void appendManualEditableLights(const SceneLightingAnchor &anchor);
  void rebuildEditableLights(EditableLightSource directionalSource,
                             EditableLightSource pointSource,
                             size_t sourcePointCount,
                             EditableLightSource areaSource,
                             size_t sourceAreaCount);
  void assignLocalShadowLayerMetadata();
  void publishPointLights();
  void publishAreaLights();
  void rebuildPointLightSsboFromEcs();
  void writeLightDescriptorStorageBuffers() const;
  void allocateClusterBuffers(VkExtent2D extent);
  void writeTiledResourceDescriptors() const;
  void createLightGizmoIconDescriptorResources();
  void loadLightGizmoIconResources(const std::filesystem::path &assetRoot);
  void writeLightGizmoIconDescriptor() const;
  std::shared_ptr<container::gpu::VulkanDevice> device_;
  container::gpu::AllocationManager &allocationManager_;
  container::gpu::PipelineManager &pipelineManager_;
  container::scene::SceneManager *sceneManager_{nullptr};
  container::scene::SceneGraph &sceneGraph_;
  container::ecs::World &world_;

  container::gpu::LightingData lightingData_{};
  container::gpu::LightingSettings lightingSettings_{};
  container::gpu::LightCullingStats lastStats_{};
  uint32_t lightVolumeIndexCount_{0};
  uint32_t rootNode_{container::scene::SceneGraph::kInvalidNode};

  // Descriptor resources created by createDescriptorResources()
  std::vector<container::gpu::AllocatedBuffer> lightingBuffers_{};
  VkDescriptorSetLayout lightDescriptorSetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool lightDescriptorPool_{VK_NULL_HANDLE};
  std::vector<VkDescriptorSet> lightDescriptorSets_{};

  // ---- Tiled light culling ------------------------------------------------
  std::vector<container::gpu::PointLightData> pointLightsSsbo_{};
  std::vector<container::gpu::AreaLightData> areaLightsSsbo_{};
  std::vector<EditableLightEntity> editableLights_{};
  EditableLightId selectedEditableLight_{};
  std::optional<EditableLightEntity> directionalOverride_{};
  std::vector<std::optional<container::gpu::PointLightData>>
      generatedPointOverrides_{};
  std::vector<std::optional<container::gpu::PointLightData>>
      importedPointOverrides_{};
  std::vector<std::optional<container::gpu::AreaLightData>>
      generatedAreaOverrides_{};
  std::vector<std::optional<container::gpu::AreaLightData>>
      importedAreaOverrides_{};
  std::vector<container::gpu::PointLightData> manualPointLights_{};
  std::vector<container::gpu::AreaLightData> manualAreaLights_{};

  // SSBOs
  container::gpu::AllocatedBuffer lightSsbo_{};
  container::gpu::AllocatedBuffer areaLightSsbo_{};
  container::gpu::AllocatedBuffer tileGridSsbo_{};
  container::gpu::AllocatedBuffer lightIndexListSsbo_{};
  container::gpu::AllocatedBuffer lightStatsBuffer_{};
  VkQueryPool timestampQueryPool_{VK_NULL_HANDLE};
  bool timestampQueriesSupported_{false};
  float timestampPeriodNs_{1.0f};
  mutable uint32_t lastTimingQueryBase_{0};
  mutable bool timingQueriesWritten_{false};

  // Compute pipeline
  VkPipeline tileCullPipeline_{VK_NULL_HANDLE};
  VkPipelineLayout tileCullPipelineLayout_{VK_NULL_HANDLE};

  // Descriptor resources for the compute cull shader (set 0 = camera+depth,
  // set 1 = light SSBO, set 2 = tile grid + index list SSBOs).
  VkDescriptorSetLayout tileCullSet0Layout_{VK_NULL_HANDLE}; // camera + depth
  VkDescriptorSetLayout tileCullSet1Layout_{VK_NULL_HANDLE}; // light SSBO
  VkDescriptorSetLayout tileCullSet2Layout_{
      VK_NULL_HANDLE}; // tile grid + index list
  VkDescriptorPool tileCullDescriptorPool_{VK_NULL_HANDLE};
  VkDescriptorSet tileCullSet0_{VK_NULL_HANDLE};
  VkDescriptorSet tileCullSet1_{VK_NULL_HANDLE};
  VkDescriptorSet tileCullSet2_{VK_NULL_HANDLE};

  // Descriptor for the tiled lighting fragment shader (set 1).
  // Contains light SSBO + tile grid + light index list.
  VkDescriptorSetLayout tiledDescriptorSetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool tiledDescriptorPool_{VK_NULL_HANDLE};
  VkDescriptorSet tiledDescriptorSet_{VK_NULL_HANDLE};

  VkDescriptorSetLayout lightGizmoIconDescriptorSetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool lightGizmoIconDescriptorPool_{VK_NULL_HANDLE};
  VkDescriptorSet lightGizmoIconDescriptorSet_{VK_NULL_HANDLE};
  container::material::TextureArrayResource lightGizmoIconAtlas_{};
  VkSampler lightGizmoIconSampler_{VK_NULL_HANDLE};
  bool lightGizmoIconsReady_{false};

  uint32_t maxTileCount_{0};
  uint32_t maxClusterCount_{0};
  mutable uint32_t lastDispatchClusterCount_{0};

  void uploadLightSsbo() const;
  void uploadAreaLightSsbo() const;
};

} // namespace container::renderer
