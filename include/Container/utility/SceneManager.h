#pragma once

#include "Container/utility/GltfTextureResources.h"

#include "Container/utility/TextureSamplingPlan.h"
#include "Container/utility/TextureImageOwnership.h"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "tiny_gltf.h"

#include "Container/app/AppConfig.h"
#include "Container/common/CommonMath.h"
#include "Container/common/CommonVulkan.h"
#include "Container/geometry/GltfModelLoader.h"
#include "Container/geometry/Model.h"
#include "Container/utility/MaterialManager.h"
#include "Container/utility/MaterialXIntegration.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/TextureManager.h"

namespace container::gpu {
class AllocationManager;
class PipelineManager;
class VulkanDevice;
struct AllocatedBuffer;
}  // namespace container::gpu

namespace container::renderer { class GltfDeformationScene; }

namespace container::geometry::gltf {
struct AnimationDirtySet;
struct AnimationLibrary;
class AnimationPlayer;
}

namespace container::scene {
class SceneGraph;
}  // namespace container::scene

namespace container::scene {

struct ModelBounds {
  glm::vec3 min{0.0f};
  glm::vec3 max{0.0f};
  glm::vec3 center{0.0f};
  glm::vec3 size{0.0f};
  float radius{0.0f};
  bool valid{false};
};

struct AuthoredDirectionalLight {
  glm::vec4 direction{0.0f, 0.0f, -1.0f, 0.0f};
  glm::vec4 colorIntensity{1.0f, 1.0f, 1.0f, 1.0f};
};

// The source node is part of the light identity even if two nodes share a
// definition. Area extensions retain the exact extension key and index.
struct AuthoredLightBinding {
  int32_t sourceNode{-1};
  std::string extension;
  int32_t definition{-1};
  auto operator<=>(const AuthoredLightBinding&) const = default;
};

struct AuthoredSceneLights {
  std::vector<container::gpu::PointLightData> points;
  std::vector<AuthoredDirectionalLight> directionals;
  std::vector<container::gpu::AreaLightData> areas;
  std::vector<AuthoredLightBinding> pointBindings, directionalBindings, areaBindings;
};

struct MaterialRenderProperties {
  uint32_t gpuMaterialIndex{0};
  bool transparent{false};
  bool alphaMasked{false};
  bool doubleSided{false};
  bool specularGlossiness{false};
  bool unlit{false};
  float heightScale{0.0f};
};

class SceneManager {
 public:
  SceneManager(container::gpu::AllocationManager& allocationManager,
               container::gpu::PipelineManager& pipelineManager,
               std::shared_ptr<container::gpu::VulkanDevice> deviceWrapper,
               const container::app::AppConfig& config);
  ~SceneManager();

  SceneManager(const SceneManager&) = delete;
  SceneManager& operator=(const SceneManager&) = delete;

  void initialize(const std::string& initialModelPath,
                  float importScale,
                  uint32_t descriptorSetCount);

  // CPU-only file/image decode and mesh conversion. Safe on the serial import
  // worker; the returned payload is consumed by reloadModel on the render thread.
  [[nodiscard]] static container::geometry::GltfLoadResult prepareGltfModel(
      const std::string& path);

  bool reloadModel(const std::string& path,
                   float importScale,
                   std::span<const container::gpu::AllocatedBuffer> cameraBuffers,
                   const container::gpu::AllocatedBuffer& objectBuffer,
                   container::geometry::GltfLoadResult* prepared = nullptr);

  void updateDescriptorSets(
      std::span<const container::gpu::AllocatedBuffer> cameraBuffers,
      const container::gpu::AllocatedBuffer& objectBuffer);
  void updateDescriptorSets(
      std::span<const container::gpu::AllocatedBuffer> cameraBuffers,
      std::span<const container::gpu::AllocatedBuffer> objectBuffers);
  void updateAuxiliaryDescriptorSets(
      std::span<const container::gpu::AllocatedBuffer> cameraBuffers,
      const container::gpu::AllocatedBuffer& objectBuffer);
  void updateAuxiliaryDescriptorSets(
      std::span<const container::gpu::AllocatedBuffer> cameraBuffers,
      std::span<const container::gpu::AllocatedBuffer> objectBuffers);
  void updateSceneClipState(const container::gpu::SceneClipState& clipState);

  VkDescriptorSetLayout descriptorSetLayout() const {
    return descriptorSetLayout_;
  }
  VkDescriptorSet descriptorSet(uint32_t imageIndex) const {
    return imageIndex < descriptorSets_.size() ? descriptorSets_[imageIndex]
                                               : VK_NULL_HANDLE;
  }
  VkDescriptorSet auxiliaryDescriptorSet(uint32_t imageIndex) const {
    return imageIndex < auxiliaryDescriptorSets_.size()
               ? auxiliaryDescriptorSets_[imageIndex]
               : VK_NULL_HANDLE;
  }

  const std::vector<container::geometry::Vertex>& vertices() const { return vertices_; }
  const std::vector<uint32_t>& indices() const { return indices_; }
  [[nodiscard]] uint64_t geometryRevision() const { return geometryRevision_; }
  [[nodiscard]] uint64_t materialRevision() const { return materialRevision_; }
  const std::vector<container::geometry::PrimitiveRange>& primitiveRanges() const {
    return model_.primitiveRanges();
  }
  const container::geometry::Model& model() const { return model_; }
  [[nodiscard]] const container::geometry::gltf::DeformationData* deformationData() const {
    return deformation_ ? &*deformation_ : nullptr;
  }
  [[nodiscard]] size_t deformationVertexLimit() const noexcept;
  [[nodiscard]] const container::gpu::SceneClipState& sceneClipState() const noexcept {
    return sceneClipState_;
  }
  [[nodiscard]] size_t gpuMaterialCount() const noexcept { return gpuMaterials_.size(); }
  [[nodiscard]] bool hasVolumeMaterials() const noexcept;
  [[nodiscard]] std::unique_ptr<container::renderer::GltfDeformationScene> takePreparedDeformation() noexcept;
  VkIndexType indexType() const { return indexType_; }
  uint32_t defaultMaterialIndex() const { return defaultMaterialIndex_; }
  [[nodiscard]] size_t materialCount() const {
    return materialManager_.materialCount();
  }
  // Cached hash of uploaded shader-visible fields (excluding C++ ABI padding).
  // Only affected surfaces lose compatibility after a material upload changes.
  [[nodiscard]] uint64_t temporalMaterialRevision(uint32_t gpuIndex) const;
  [[nodiscard]] bool rayGeometryCompatible(uint32_t gpuIndex) const;
  uint32_t diagnosticMaterialIndex() const;
  uint32_t resolveGpuMaterialIndex(uint32_t materialIndex) const;
  uint32_t createSolidMaterial(const glm::vec4& baseColor,
                               bool doubleSided = false,
                               container::material::AlphaMode alphaMode =
                                   container::material::AlphaMode::Opaque);
  uint32_t createMaterial(const container::material::Material& material);
  uint32_t loadMaterialTexture(const std::filesystem::path& texturePath,
                               bool isSrgb,
                               uint32_t samplerIndex = 0);
  uint32_t loadMaterialTextureFromBytes(
      const std::string& textureName,
      std::span<const std::byte> encodedBytes,
      bool isSrgb,
      uint32_t samplerIndex = 0);
  void uploadMaterialResources();
  const ModelBounds& modelBounds() const { return modelBounds_; }
  const std::vector<container::gpu::PointLightData>& authoredPointLights() const {
    return authoredPointLights_;
  }
  const std::vector<AuthoredDirectionalLight>& authoredDirectionalLights() const {
    return authoredDirectionalLights_;
  }
  const std::vector<container::gpu::AreaLightData>& authoredAreaLights() const {
    return authoredAreaLights_;
  }
  bool isDefaultTestSceneActive() const;
  void populateSceneGraph(SceneGraph& sceneGraph) const;
  [[nodiscard]] std::span<const AuthoredLightBinding> authoredPointLightBindings() const noexcept {
    return authoredPointBindings_;
  }
  [[nodiscard]] std::span<const AuthoredLightBinding> authoredDirectionalLightBindings() const noexcept {
    return authoredDirectionalBindings_;
  }
  [[nodiscard]] std::span<const AuthoredLightBinding> authoredAreaLightBindings() const noexcept {
    return authoredAreaBindings_;
  }
  [[nodiscard]] AuthoredSceneLights publishedAuthoredSceneLights() const;
  [[nodiscard]] float importScale() const noexcept { return config_.importScale; }
  [[nodiscard]] AuthoredSceneLights evaluateAuthoredSceneLights(
      const SceneGraph* graph, uint32_t editorRoot) const;
  [[nodiscard]] AuthoredSceneLights evaluateGltfSceneLights(
      const SceneGraph& graph, uint32_t editorRoot, uint32_t scene) const;
  [[nodiscard]] ModelBounds evaluateSceneBounds(const SceneGraph& graph) const;
  void publishSceneSelection(uint32_t scene, const ModelBounds& bounds,
                             AuthoredSceneLights& lights) noexcept;
  void exchangeAuthoredSceneLights(AuthoredSceneLights&) noexcept;
  [[nodiscard]] tinygltf::Model& animationSource() noexcept { return gltfModel_; }
  [[nodiscard]] const tinygltf::Model& animationSource() const noexcept { return gltfModel_; }
  std::unique_ptr<container::geometry::gltf::AnimationPlayer> takePreparedAnimation() noexcept;
  [[nodiscard]] uint64_t animationSourceRevision() const noexcept { return animationSourceRevision_; }
  [[nodiscard]] bool canExportScene(const SceneGraph& graph) const noexcept {
    return graph.nodeCount() != 0 || !gltfModel_.scenes.empty();
  }
  [[nodiscard]] tinygltf::Model buildSceneExport(
      const SceneGraph& graph, uint32_t editorRoot, std::vector<int32_t>& sourceNodes,
      std::vector<AuthoredSceneLights>& allLights,
      std::vector<AuthoredSceneLights>& visibleLights,
      const container::geometry::gltf::AnimationLibrary* animation = nullptr) const;
  void publishAnimationChanges(const container::geometry::gltf::AnimationDirtySet& dirty,
                               const SceneGraph& graph, uint32_t editorRoot);
  struct PreparedRuntimeMeshChange {
    container::geometry::Model model;
    std::vector<container::geometry::Vertex> vertices;
    std::vector<uint32_t> indices;
    ModelBounds bounds;
  };
  [[nodiscard]] PreparedRuntimeMeshChange prepareRuntimeMeshChange(
      uint32_t primitive, const container::geometry::Mesh* append) const;
  void exchangeRuntimeMeshChange(PreparedRuntimeMeshChange&) noexcept;
  [[nodiscard]] MaterialRenderProperties materialRenderProperties(
      uint32_t materialIndex) const;

  glm::vec4 resolveMaterialColor(uint32_t materialIndex) const;
  glm::vec4 resolveMaterialEmissive(uint32_t materialIndex) const;
  glm::vec2 resolveMaterialMetallicRoughnessFactors(
      uint32_t materialIndex) const;
  uint32_t resolveMaterialTextureIndex(uint32_t materialIndex) const;
  uint32_t resolveMaterialNormalTexture(uint32_t materialIndex) const;
  float resolveMaterialNormalTextureScale(uint32_t materialIndex) const;
  uint32_t resolveMaterialOcclusionTexture(uint32_t materialIndex) const;
  float resolveMaterialOcclusionStrength(uint32_t materialIndex) const;
  uint32_t resolveMaterialEmissiveTexture(uint32_t materialIndex) const;
  uint32_t resolveMaterialMetallicRoughnessTexture(
      uint32_t materialIndex) const;
  uint32_t resolveMaterialRoughnessTexture(uint32_t materialIndex) const;
  uint32_t resolveMaterialMetalnessTexture(uint32_t materialIndex) const;
  uint32_t resolveMaterialSpecularTexture(uint32_t materialIndex) const;
  uint32_t resolveMaterialSpecularColorTexture(uint32_t materialIndex) const;
  uint32_t resolveMaterialHeightTexture(uint32_t materialIndex) const;
  uint32_t resolveMaterialOpacityTexture(uint32_t materialIndex) const;
  uint32_t resolveMaterialTransmissionTexture(uint32_t materialIndex) const;
  uint32_t resolveMaterialClearcoatTexture(uint32_t materialIndex) const;
  uint32_t resolveMaterialClearcoatRoughnessTexture(uint32_t materialIndex) const;
  uint32_t resolveMaterialClearcoatNormalTexture(uint32_t materialIndex) const;
  uint32_t resolveMaterialThicknessTexture(uint32_t materialIndex) const;
  uint32_t resolveMaterialSheenColorTexture(uint32_t materialIndex) const;
  uint32_t resolveMaterialSheenRoughnessTexture(uint32_t materialIndex) const;
  uint32_t resolveMaterialIridescenceTexture(uint32_t materialIndex) const;
  uint32_t resolveMaterialIridescenceThicknessTexture(uint32_t materialIndex) const;
  float resolveMaterialOpacityFactor(uint32_t materialIndex) const;
  float resolveMaterialSpecularFactor(uint32_t materialIndex) const;
  glm::vec4 resolveMaterialSpecularColorFactor(uint32_t materialIndex) const;
  float resolveMaterialHeightScale(uint32_t materialIndex) const;
  float resolveMaterialHeightOffset(uint32_t materialIndex) const;
  float resolveMaterialTransmissionFactor(uint32_t materialIndex) const;
  float resolveMaterialEmissiveStrength(uint32_t materialIndex) const;
  float resolveMaterialIor(uint32_t materialIndex) const;
  float resolveMaterialDispersion(uint32_t materialIndex) const;
  float resolveMaterialClearcoatFactor(uint32_t materialIndex) const;
  float resolveMaterialClearcoatRoughnessFactor(uint32_t materialIndex) const;
  float resolveMaterialClearcoatNormalTextureScale(uint32_t materialIndex) const;
  float resolveMaterialThicknessFactor(uint32_t materialIndex) const;
  glm::vec4 resolveMaterialAttenuationColor(uint32_t materialIndex) const;
  float resolveMaterialAttenuationDistance(uint32_t materialIndex) const;
  glm::vec4 resolveMaterialSheenColorFactor(uint32_t materialIndex) const;
  float resolveMaterialSheenRoughnessFactor(uint32_t materialIndex) const;
  float resolveMaterialIridescenceFactor(uint32_t materialIndex) const;
  float resolveMaterialIridescenceIor(uint32_t materialIndex) const;
  float resolveMaterialIridescenceThicknessMinimum(uint32_t materialIndex) const;
  float resolveMaterialIridescenceThicknessMaximum(uint32_t materialIndex) const;
  float resolveMaterialAlphaCutoff(uint32_t materialIndex) const;
  bool isMaterialTransparent(uint32_t materialIndex) const;
  bool isMaterialAlphaMasked(uint32_t materialIndex) const;
  bool isMaterialDoubleSided(uint32_t materialIndex) const;
  bool usesMaterialSpecularGlossiness(uint32_t materialIndex) const;
  bool isMaterialUnlit(uint32_t materialIndex) const;

 private:
  uint32_t queryTextureDescriptorCapacity() const;
  uint32_t queryMaterialSamplerDescriptorCapacity() const;
  void validateGltfSamplerPolicies(const tinygltf::Model& model, bool replacement) const;
  void ensureTextureSamplingFallback();
  void finishTextureImageOwnershipPublication();
  friend struct SceneSamplerPublicationTestAccess;
  struct TextureSamplingFallback {
    container::material::TextureResource resource;
    std::shared_ptr<const container::gpu::AllocationManager::TextureAllocationLease> lease;
  };

  uint32_t resolveLoadedMaterialIndex(int32_t materialIndex) const;
  void createDescriptorSetLayout();
  void createSampler();
  void loadMaterialXMaterial();
  void loadGltfAssets(container::geometry::GltfLoadResult* prepared = nullptr,
      const container::material::PreparedGltfTextures* preparedTextures = nullptr,
                      std::unique_ptr<container::geometry::gltf::AnimationPlayer>* preparedAnimation = nullptr,
                      std::unique_ptr<container::renderer::GltfDeformationScene>* preparedDeformation = nullptr,
                      int requestedScene = -1);
  [[nodiscard]] AuthoredSceneLights evaluateAuthoredSceneLights(
      const SceneGraph* graph, uint32_t editorRoot,
      std::optional<std::span<const uint32_t>> sceneRoots, bool includeHidden = false) const;
  void loadDefaultTestSceneAssets();
  void uploadMaterialBuffer();
  void uploadTextureMetadataBuffer();
  void createSceneClipStateBuffer();
  void writeSceneClipStateBuffer();
  void collectAuthoredPunctualLights(
      const SceneGraph* graph = nullptr, uint32_t editorRoot = std::numeric_limits<uint32_t>::max());
  void updateModelBounds();
  [[nodiscard]] ModelBounds computeModelBounds(
      const container::geometry::Model& model,
      std::span<const container::geometry::Vertex> vertices,
      std::span<const uint32_t> indices, const SceneGraph* graph = nullptr) const;
  void allocateDescriptorSets(uint32_t descriptorSetCount);
  void writeDescriptorSetContents(
      VkDescriptorSet descriptorSet,
      const container::gpu::AllocatedBuffer& cameraBuffer,
      const container::gpu::AllocatedBuffer& objectBuffer);
  void resetLoadedAssets();
  void appendSceneAsset(const std::filesystem::path& assetPath,
                        const glm::mat4& transform,
                        std::vector<container::geometry::Mesh>& mergedMeshes);
  [[nodiscard]] static std::filesystem::path resolveSceneAssetPath(
      std::string_view relativePath);

  container::gpu::AllocationManager* allocationManager_{nullptr};
  container::gpu::PipelineManager* pipelineManager_{nullptr};
  std::shared_ptr<container::gpu::VulkanDevice> deviceWrapper_{};
  container::app::AppConfig config_{};

  container::material::SlangMaterialXBridge materialXBridge_{};
  container::material::TextureManager textureManager_{};
  container::material::MaterialManager materialManager_{};

  container::geometry::Model model_{};
  tinygltf::Model gltfModel_{};
  tinygltf::Model bakedExportResources_{};
  std::map<uint32_t, tinygltf::Material> bakedExportMaterials_;
  std::vector<container::geometry::Vertex> vertices_{};
  std::vector<uint32_t> indices_{};
  uint64_t geometryRevision_{0};
  uint64_t animationSourceRevision_{1};
  std::optional<container::geometry::gltf::DeformationData> deformation_;
  std::unique_ptr<container::renderer::GltfDeformationScene> pendingDeformation_;
  std::unique_ptr<container::geometry::gltf::AnimationPlayer> pendingAnimation_;
  uint64_t materialRevision_{0};
  ModelBounds modelBounds_{};
  std::vector<container::gpu::PointLightData> authoredPointLights_{};
  std::vector<AuthoredDirectionalLight> authoredDirectionalLights_{};
  std::vector<container::gpu::AreaLightData> authoredAreaLights_{};
  std::vector<AuthoredLightBinding> authoredPointBindings_, authoredDirectionalBindings_, authoredAreaBindings_;

  VkIndexType indexType_{VK_INDEX_TYPE_UINT32};

  glm::vec4 materialBaseColor_{1.0f};
  uint32_t defaultMaterialIndex_{std::numeric_limits<uint32_t>::max()};
  uint32_t diagnosticMaterialIndex_{std::numeric_limits<uint32_t>::max()};
  uint32_t gltfMaterialBaseIndex_{0};

  std::vector<container::gpu::GpuMaterial> gpuMaterials_{};
  std::vector<uint64_t> gpuMaterialTemporalRevisions_{};
  container::gpu::AllocatedBuffer materialBuffer_{};
  size_t materialBufferCapacity_{0};
  std::vector<container::gpu::GpuTextureMetadata> textureMetadata_{};
  container::material::TextureSamplingPlan textureSamplingPlan_{};
  std::shared_ptr<const TextureSamplingFallback> textureSamplingFallback_{};
  container::material::TextureImageOwnershipPtr textureImageOwnership_{};
  container::material::MaterialSamplerSlotPlan materialSamplerSlots_{};
  uint32_t materialSamplerDescriptorCapacity_{1};

  container::gpu::AllocatedBuffer textureMetadataBuffer_{};
  size_t textureMetadataBufferCapacity_{0};
  container::gpu::SceneClipState sceneClipState_{};
  container::gpu::AllocatedBuffer sceneClipStateBuffer_{};

  VkSampler baseColorSampler_{VK_NULL_HANDLE};
  std::vector<VkSampler> materialSamplers_{};
  VkDescriptorSetLayout descriptorSetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool descriptorPool_{VK_NULL_HANDLE};
  std::vector<VkDescriptorSet> descriptorSets_{};
  std::vector<VkDescriptorSet> auxiliaryDescriptorSets_{};
  uint32_t textureDescriptorCapacity_{1};
};

}  // namespace container::scene
