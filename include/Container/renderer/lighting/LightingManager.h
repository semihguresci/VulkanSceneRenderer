#pragma once

#include "Container/utility/ComputePipeline.h"

#include "Container/common/CommonMath.h"
#include "Container/common/CommonVulkan.h"
#include "Container/renderer/lighting/EditableLight.h"
#include "Container/renderer/lighting/LightEditReceipt.h"
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
#include <string_view>
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
struct AuthoredSceneLights;
struct ModelBounds;
} // namespace container::scene

namespace container::renderer {

class LtcLutResources;
struct LightEditIdentityState;

// Struct carrying the scene-space anchor used for light positioning.
struct SceneLightingAnchor {
  glm::mat4 sceneTransform{1.0f};
  glm::vec3 center{0.0f, 0.0f, 0.0f};
  float localRadius{1.0f};
  float worldRadius{1.0f};
};

// Scene-local edits retain only explicit overrides and manual lights. The
// evaluated authored light table is rebuilt from the selected source scene.
struct SceneLightEdits {
  std::optional<container::gpu::LightingSettings> generationSettings{};
  std::optional<EditableLightEntity> directionalOverride{};
  std::vector<std::optional<container::gpu::PointLightData>>
      generatedPointOverrides{}, importedPointOverrides{};
  std::vector<std::optional<container::gpu::AreaLightData>>
      generatedAreaOverrides{}, importedAreaOverrides{};
  std::vector<container::gpu::PointLightData> manualPointLights{};
  std::vector<container::gpu::AreaLightData> manualAreaLights{};
  // Editor lifetime authority travels with properly retained generations. It
  // never enters glTF output and is not inferred from equal vector payloads.
  std::shared_ptr<const LightEditIdentityState> identities;
  [[nodiscard]] uint64_t identityStorageBytes() const noexcept;
};
struct LightingGraphPoseGenerationLimits {
  uint64_t maxPreparedCpuBytes{64ull * 1024 * 1024};
  uint64_t maxPreparedGpuBytes{256ull * 1024 * 1024};
  uint64_t maxGraphNodes{1'000'000};
};

enum class ClusteredLightCapacityStatus {
  NotInitialized,
  Available,
  StorageRangeExceeded,
  DispatchLimitExceeded,
  ShaderIndexLimitExceeded,
};

struct ClusteredLightCapacity {
  ClusteredLightCapacityStatus status{ClusteredLightCapacityStatus::NotInitialized};
  VkDeviceSize storageRangeLimit{};
  VkDeviceSize tileGridBytes{};
  VkDeviceSize lightIndexListBytes{};
  uint32_t tileCountX{};
  uint32_t tileCountY{};
  uint64_t clusterCount{};

  [[nodiscard]] bool available() const noexcept {
    return status == ClusteredLightCapacityStatus::Available;
  }
};

[[nodiscard]] ClusteredLightCapacity planClusteredLightCapacity(
    VkExtent2D extent, VkDeviceSize storageRangeLimit,
    const std::array<uint32_t, 3> &dispatchLimits) noexcept;
[[nodiscard]] std::string_view clusteredLightCapacityStatusName(
    ClusteredLightCapacityStatus status) noexcept;

// Manages the LightingData uniform buffer, directional + point/area lights,
// tiled light culling resources, and the light-gizmo draw helper.
class LightingManager {
public:
  using LightEditCommand=container::renderer::LightEditCommand;
  class PreparedLightEdit {
  public:
    PreparedLightEdit() noexcept;
    ~PreparedLightEdit();
    PreparedLightEdit(PreparedLightEdit&&) noexcept;
    PreparedLightEdit& operator=(PreparedLightEdit&&) noexcept;
    PreparedLightEdit(const PreparedLightEdit&)=delete;
    PreparedLightEdit& operator=(const PreparedLightEdit&)=delete;
    explicit operator bool() const noexcept;
    [[nodiscard]] bool changed() const noexcept;
    [[nodiscard]] uint64_t ownedBytes() const noexcept;
    [[nodiscard]] uint64_t ownedGpuBytes() const noexcept;
    [[nodiscard]] LightEditCommand command() const noexcept;
    [[nodiscard]] LightEditTarget target() const noexcept;
    [[nodiscard]] const SceneLightEdits& edits() const;
    [[nodiscard]] const SceneLightEdits& beforeEdits() const;
    [[nodiscard]] const EditableLightEntity& selectedLight() const;
    [[nodiscard]] const container::gpu::LightingData& lightingData() const;
    [[nodiscard]] const std::vector<container::gpu::PointLightData>& pointLightsSsbo() const;
    [[nodiscard]] const std::vector<container::gpu::AreaLightData>& areaLightsSsbo() const;
    [[nodiscard]] std::span<const container::gpu::AllocatedBuffer> lightingBuffers() const noexcept;
    [[nodiscard]] const container::gpu::AllocatedBuffer& pointLightBuffer(uint32_t image = 0) const;
    [[nodiscard]] const container::gpu::AllocatedBuffer& areaLightBuffer(uint32_t image = 0) const;
    [[nodiscard]] std::span<const container::gpu::AllocatedBuffer> pointLightBuffers() const noexcept;
    [[nodiscard]] std::span<const container::gpu::AllocatedBuffer> areaLightBuffers() const noexcept;
    [[nodiscard]] VkDescriptorSet lightDescriptorSet(uint32_t) const noexcept;
    [[nodiscard]] VkDescriptorSet tiledDescriptorSet(uint32_t image = 0) const noexcept;
    void updateRayShadowDescriptors(uint32_t,VkBuffer,VkDeviceSize,VkImageView);
  private:
    struct State;
    std::unique_ptr<State> state_;
    friend class LightingManager;
  };
  [[nodiscard]] LightEditSnapshot lightEditSnapshot(EditableLightId) const;
  [[nodiscard]] uint64_t lightEditRevision() const noexcept {return lightEditRevision_;}
  [[nodiscard]] PreparedLightEdit prepareLightEdit(const LightEditTarget&,const LightEditPatch&,
      const LightEditCommand* originalGesture=nullptr,const LightEditLimits& = {}) const;
  [[nodiscard]] PreparedLightEdit prepareLightEditReplay(const LightEditCommand&,bool applied,const LightEditLimits& = {}) const;
  [[nodiscard]] PreparedLightEdit prepareLightAdd(EditableLightType,const LightEditLimits& = {}) const;
  [[nodiscard]] bool canExchangeLightEdit(const PreparedLightEdit&) const noexcept;
  [[nodiscard]] bool lightEditCommandMatches(const LightEditCommand&) const noexcept;
  [[nodiscard]] bool exchangeLightEdit(PreparedLightEdit&) noexcept;
  // Owns one detached CPU/GPU lighting payload. Layouts, LTC/icon images,
  // pipelines and tiled scratch resources remain owned by the live manager.
  // Retire all GPU readers before exchange or destruction. Exchange is
  // reversible: the token then retains the previous generation for history.
  class PreparedGeneration {
  public:
    PreparedGeneration() noexcept;
    ~PreparedGeneration();
    PreparedGeneration(PreparedGeneration&&) noexcept;
    PreparedGeneration& operator=(PreparedGeneration&&) noexcept;
    PreparedGeneration(const PreparedGeneration&) = delete;
    PreparedGeneration& operator=(const PreparedGeneration&) = delete;
    explicit operator bool() const noexcept;
    [[nodiscard]] const container::gpu::LightingData& lightingData() const;
    [[nodiscard]] const std::vector<container::gpu::PointLightData>& pointLightsSsbo() const;
    [[nodiscard]] const std::vector<container::gpu::AreaLightData>& areaLightsSsbo() const;
    [[nodiscard]] std::span<const container::gpu::AllocatedBuffer> lightingBuffers() const noexcept;
    [[nodiscard]] const container::gpu::AllocatedBuffer& pointLightBuffer(uint32_t image = 0) const;
    [[nodiscard]] const container::gpu::AllocatedBuffer& areaLightBuffer(uint32_t image = 0) const;
    [[nodiscard]] std::span<const container::gpu::AllocatedBuffer> pointLightBuffers() const noexcept;
    [[nodiscard]] std::span<const container::gpu::AllocatedBuffer> areaLightBuffers() const noexcept;
    [[nodiscard]] VkDescriptorSet lightDescriptorSet(uint32_t imageIndex) const noexcept;
    [[nodiscard]] VkDescriptorSet tiledDescriptorSet(uint32_t image = 0) const noexcept;
    [[nodiscard]] uint64_t ownedBytes() const noexcept;
    [[nodiscard]] uint64_t ownedGpuBytes() const noexcept;
    // Complete bindings 3/4 against the matching detached ray generation
    // before publishing any live subsystem. Other bindings are already ready.
    void updateRayShadowDescriptors(uint32_t imageIndex, VkBuffer settings,
        VkDeviceSize settingsSize, VkImageView visibility);
  private:
    friend class LightingManager;
    struct State;
    std::unique_ptr<State> state_;
  };

  // Descriptor-only, one-shot stage. It borrows this manager's unchanged
  // lighting buffers and LTC resources; no CPU light or World state is copied.
  // Complete bindings 3/4 for every image before exchange. The caller retires
  // all readers of the old sets before exchange, which consumes this token.
  class PreparedRayShadowBindings {
  public:
    PreparedRayShadowBindings() noexcept;
    ~PreparedRayShadowBindings();
    PreparedRayShadowBindings(PreparedRayShadowBindings&&) noexcept;
    PreparedRayShadowBindings& operator=(PreparedRayShadowBindings&&) noexcept;
    PreparedRayShadowBindings(const PreparedRayShadowBindings&) = delete;
    PreparedRayShadowBindings& operator=(const PreparedRayShadowBindings&) = delete;
    explicit operator bool() const noexcept;
    [[nodiscard]] VkDescriptorSet lightDescriptorSet(uint32_t imageIndex) const noexcept;
    void updateRayShadowDescriptors(uint32_t imageIndex, VkBuffer settings,
        VkDeviceSize settingsSize, VkImageView visibility);
  private:
    friend class LightingManager;
    struct State;
    std::unique_ptr<State> state_;
  };

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
  [[nodiscard]] PreparedGeneration prepareGeneration(
      container::scene::SceneManager& sceneManager,
      container::scene::SceneGraph& graph, container::ecs::World& world,
      uint32_t rootNode, const SceneLightEdits& edits, uint32_t imageCount) const;
  // One-shot graph-pose presentation against this manager's unchanged live
  // source. Candidate graph/World are detached; candidate source lights/bounds
  // come from the matching prepared SceneManager pose operation. Only detached
  // CPU evaluation borrows those overrides; live sources are never mutated.
  // canExchangeGeneration checks BEFORE source/graph publication. The actual
  // exchange checks their exact prospective phase, while generationEpoch is
  // still unchanged; scene bindings publish last in the outer transaction.
  [[nodiscard]] PreparedGeneration prepareGraphPoseGeneration(
      container::scene::SceneManager& liveSceneManager,
      const container::scene::SceneGraph& candidateGraph, container::ecs::World& detachedWorld,
      uint32_t rootNode, const container::scene::AuthoredSceneLights& candidateLights,
      const container::scene::ModelBounds& candidateBounds, const SceneLightEdits& edits, uint32_t imageCount,
      const LightingGraphPoseGenerationLimits& limits = {}) const;
  [[nodiscard]] bool canExchangeGeneration(const PreparedGeneration&) const noexcept;
  // No allocation, buffer upload, map or flush. Only owned payloads swap;
  // stable live world/graph/scene-manager references are never rebound.
  bool exchangeGeneration(PreparedGeneration&) noexcept;
  [[nodiscard]] PreparedRayShadowBindings prepareRayShadowBindings(uint32_t imageCount) const;
  [[nodiscard]] bool canExchangeRayShadowBindings(const PreparedRayShadowBindings&) const noexcept;
  // No allocation, upload or descriptor writes: only the light descriptor
  // payload changes. The previous descriptor pool is retired immediately.
  bool exchangeRayShadowBindings(PreparedRayShadowBindings&) noexcept;
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
  // Uploads lighting UBO and point/area SSBO snapshots. Bulk upload requires
  // every live reader to have retired. Indexed upload requires only that image.
  void uploadLightingData() const;
  void uploadLightingData(uint32_t imageIndex) const;
  void collectStats();

  // Dispatches the tile culling compute shader using this image's published
  // light snapshot. Must be called between G-Buffer and Lighting passes.
  void dispatchTileCull(VkCommandBuffer cmd, uint32_t imageIndex, VkExtent2D screenExtent,
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
  [[nodiscard]] SceneLightEdits snapshotSceneLightEdits() const;
  // Logical bytes required by a detached edit snapshot, without allocating it.
  // Saturates on overflow so callers can reject before copying unbounded edits.
  [[nodiscard]] uint64_t sceneLightEditsStorageBytes() const noexcept;
  // Restores CPU edit state and clears selection. The caller publishes the
  // selected source scene before calling updateLightingData() and uploading.
  void restoreSceneLightEdits(const SceneLightEdits &edits);
  // Scene light indices belong to one loaded source. Drop their edits and
  // selection when replacing that source; preserve user-created manual lights.
  // Recomputes CPU lighting only. The caller uploads after retiring GPU reads.
  void clearSceneLightOverrides();
  [[nodiscard]] bool ltcReady() const;
  [[nodiscard]] const std::string &ltcStatus() const;
  [[nodiscard]] uint64_t ltcMemoryBytes() const;
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
  const container::gpu::AllocatedBuffer& pointLightBuffer(uint32_t image = 0) const {
    return pointLightBuffers_.at(image);
  }
  const container::gpu::AllocatedBuffer& areaLightBuffer(uint32_t image = 0) const {
    return areaLightBuffers_.at(image);
  }
  std::span<const container::gpu::AllocatedBuffer> pointLightBuffers() const noexcept {
    return pointLightBuffers_;
  }
  std::span<const container::gpu::AllocatedBuffer> areaLightBuffers() const noexcept {
    return areaLightBuffers_;
  }

  SceneLightingAnchor computeSceneLightingAnchor() const;
  glm::vec3 directionalLightPosition() const;

  // The root node must be kept in sync with the scene graph after
  // buildSceneGraph.
  void setRootNode(uint32_t rootNode) { rootNode_ = rootNode; }

  // ---- Tiled lighting accessors -------------------------------------------
  bool isTiledLightingReady() const noexcept {
    return tileCullPipeline_ != VK_NULL_HANDLE &&
           tileCullPipelineLayout_ != VK_NULL_HANDLE &&
           tiledSets_.size() == lightingBuffers_.size() && !tiledSets_.empty() &&
           tiledScratchResourcesReady();
  }
  [[nodiscard]] const ClusteredLightCapacity &clusteredLightCapacity() const noexcept {
    return clusteredLightCapacity_;
  }
  [[nodiscard]] bool tiledCapacityLimited() const noexcept;
  VkDescriptorSetLayout tiledDescriptorSetLayout() const {
    return tiledDescriptorSetLayout_;
  }
  VkDescriptorSet tiledDescriptorSet(uint32_t image = 0) const noexcept {
    return image < tiledSets_.size() ? tiledSets_[image][3] : VK_NULL_HANDLE;
  }
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
    return tiledScratchResourcesReady() ? clusteredLightCapacity_.tileGridBytes : 0;
  }

private:
  friend struct LightingScratchRecoveryTestAccess;
  struct DetachedGraphTag {};
  LightingManager(std::shared_ptr<container::gpu::VulkanDevice>, container::gpu::AllocationManager&,
      container::gpu::PipelineManager&, container::scene::SceneManager*, const container::scene::SceneGraph&,
      container::ecs::World&, DetachedGraphTag);
  [[nodiscard]] PreparedGeneration prepareGenerationImpl(container::scene::SceneManager&,
      const container::scene::SceneGraph&, container::ecs::World&, uint32_t, const SceneLightEdits&, uint32_t,
      const container::scene::AuthoredSceneLights*, const container::scene::ModelBounds*,
      const LightingGraphPoseGenerationLimits*,bool lightOnly=false) const;
  void exchangeGenerationPayload(PreparedGeneration&) noexcept;
  void synchronizeLightEditIdentities() const;
  [[nodiscard]] bool lightEditIdentitiesValid() const noexcept;
  [[nodiscard]] std::optional<LightEditTarget> currentLightEditTarget(EditableLightId) const noexcept;
  [[nodiscard]] std::optional<EditableLightEntity> lightOverride(EditableLightId) const;
  [[nodiscard]] std::optional<LightEditValues> lightOverrideValues(EditableLightId) const noexcept;
  [[nodiscard]] EditableLightEntity lightBase(EditableLightId,LightEditCanonicalPayload* raw=nullptr) const;
  [[nodiscard]] LightEditCanonicalPayload lightRawOverride(EditableLightId) const noexcept;
  [[nodiscard]] PreparedLightEdit prepareLightEditImpl(const LightEditTarget&,const LightEditPatch*,
      const LightEditCommand*,bool replay,bool applied,const LightEditLimits&) const;
  [[nodiscard]] PreparedLightEdit prepareLightAddImpl(EditableLightType,const LightEditCommand*,bool applied,const LightEditLimits&) const;
  [[nodiscard]] bool generationResourcesReady(const PreparedGeneration&) const noexcept;
  [[nodiscard]] bool tiledScratchResourcesReady() const noexcept;
  [[nodiscard]] bool graphPoseGenerationMatches(const PreparedGeneration&, bool prospective) const noexcept;
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
  void createLightingFrameResources(LightingManager& target, uint32_t imageCount,
                                    bool inheritRayBindings) const;
  void createTiledFrameDescriptors(LightingManager& target, uint32_t imageCount) const;
  void swapFrameResources(LightingManager& target) noexcept;
  void uploadLightingData(const LightingManager& source) const;
  void uploadLightingData(uint32_t imageIndex, const LightingManager& source) const;
  void allocateClusterBuffers(VkExtent2D extent);
  void writeTiledResourceDescriptors(const LightingManager& target) const noexcept;
  void destroyLightDescriptorPool() noexcept;
  void invalidateRayShadowBindingPreparations() noexcept;
  void destroyTileCullDescriptorPool() noexcept;
  void createLightGizmoIconDescriptorResources();
  void loadLightGizmoIconResources(const std::filesystem::path &assetRoot);
  void writeLightGizmoIconDescriptor() const;
  std::shared_ptr<container::gpu::VulkanDevice> device_;
  container::gpu::AllocationManager &allocationManager_;
  container::gpu::PipelineManager &pipelineManager_;
  container::scene::SceneManager *sceneManager_{nullptr};
  const container::scene::SceneGraph &sceneGraph_;
  container::ecs::World &world_;
  // Set only on a detached payload during fallible CPU evaluation. Always
  // cleared before return/publication; these are never exchanged or retained.
  const container::scene::AuthoredSceneLights* authoredLightsOverride_{};
  const container::scene::ModelBounds* boundsOverride_{};
  bool suppressWorldPublication_{};
  uint64_t lightEditOwnerIdentity_{},lightEditRevision_{1},generatedLightRevision_{1};
  mutable std::shared_ptr<const LightEditIdentityState> lightEditIdentities_;

  container::gpu::LightingData lightingData_{};
  container::gpu::LightingSettings lightingSettings_{};
  std::unique_ptr<LtcLutResources> ltcLuts_;
  container::gpu::LightCullingStats lastStats_{};
  uint32_t lightVolumeIndexCount_{0};
  uint32_t rootNode_{container::scene::SceneGraph::kInvalidNode};

  // Descriptor resources created by createDescriptorResources()
  std::vector<container::gpu::AllocatedBuffer> lightingBuffers_{};
  VkDescriptorSetLayout lightDescriptorSetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool lightDescriptorPool_{VK_NULL_HANDLE};
  std::vector<VkDescriptorSet> lightDescriptorSets_{};
  struct RayShadowBinding {
    VkBuffer settings{VK_NULL_HANDLE};
    VkDeviceSize settingsSize{};
    VkImageView visibility{VK_NULL_HANDLE};
  };
  std::vector<RayShadowBinding> rayShadowBindings_{};
  // Never exchanged with asset generations: tokens belong to this lifetime
  // and to precisely the binding resources against which they were prepared.
  std::shared_ptr<const uint8_t> descriptorBindingIdentity_{std::make_shared<const uint8_t>(0)};
  uint64_t descriptorBindingEpoch_{1};
  // Named fixture-only seam: one-shot failure immediately before one of the
  // two scratch replacement allocations, after the real invalidation path.
  uint8_t scratchAllocationFailureForTest_{};

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
  std::vector<container::gpu::AllocatedBuffer> pointLightBuffers_{};
  std::vector<container::gpu::AllocatedBuffer> areaLightBuffers_{};
  container::gpu::AllocatedBuffer tileGridSsbo_{};
  container::gpu::AllocatedBuffer lightIndexListSsbo_{};
  container::gpu::AllocatedBuffer lightStatsBuffer_{};
  VkQueryPool timestampQueryPool_{VK_NULL_HANDLE};
  bool timestampQueriesSupported_{false};
  float timestampPeriodNs_{1.0f};
  mutable uint32_t lastTimingQueryBase_{0};
  mutable bool timingQueriesWritten_{false};

  // Compute pipeline
  container::gpu::ComputePipeline tileCullPipeline_{};
  VkPipelineLayout tileCullPipelineLayout_{VK_NULL_HANDLE};

  // Descriptor resources for the compute cull shader (set 0 = camera+depth,
  // set 1 = light SSBO, set 2 = tile grid + index list SSBOs).
  VkDescriptorSetLayout tileCullSet0Layout_{VK_NULL_HANDLE}; // camera + depth
  VkDescriptorSetLayout tileCullSet1Layout_{VK_NULL_HANDLE}; // light SSBO
  VkDescriptorSetLayout tileCullSet2Layout_{
      VK_NULL_HANDLE}; // tile grid + index list
  VkDescriptorPool tileCullDescriptorPool_{VK_NULL_HANDLE};
  // Three cull sets followed by the tiled fragment set for each image.
  std::vector<std::array<VkDescriptorSet, 4>> tiledSets_{};

  // Descriptor for the tiled lighting fragment shader (set 1).
  // Contains light SSBO + tile grid + light index list.
  VkDescriptorSetLayout tiledDescriptorSetLayout_{VK_NULL_HANDLE};

  VkDescriptorSetLayout lightGizmoIconDescriptorSetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool lightGizmoIconDescriptorPool_{VK_NULL_HANDLE};
  VkDescriptorSet lightGizmoIconDescriptorSet_{VK_NULL_HANDLE};
  container::material::TextureArrayResource lightGizmoIconAtlas_{};
  VkSampler lightGizmoIconSampler_{VK_NULL_HANDLE};
  bool lightGizmoIconsReady_{false};

  VkDeviceSize clusteredStorageRangeLimit_{};
  std::array<uint32_t, 3> clusteredDispatchLimits_{};
  ClusteredLightCapacity clusteredLightCapacity_{};
  mutable uint32_t lastDispatchClusterCount_{0};

};

} // namespace container::renderer
