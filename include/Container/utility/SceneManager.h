#pragma once

#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "tiny_gltf.h"

#include "Container/app/AppConfig.h"
#include "Container/common/CommonMath.h"
#include "Container/common/CommonVulkan.h"
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

  bool reloadModel(const std::string& path,
                   float importScale,
                   std::span<const container::gpu::AllocatedBuffer> cameraBuffers,
                   const container::gpu::AllocatedBuffer& objectBuffer);

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
  const std::vector<container::geometry::PrimitiveRange>& primitiveRanges() const {
    return model_.primitiveRanges();
  }
  const container::geometry::Model& model() const { return model_; }
  VkIndexType indexType() const { return indexType_; }
  uint32_t defaultMaterialIndex() const { return defaultMaterialIndex_; }
  [[nodiscard]] size_t materialCount() const {
    return materialManager_.materialCount();
  }
  // Cached hash of uploaded shader-visible fields (excluding C++ ABI padding).
  // Only affected surfaces lose compatibility after a material upload changes.
  [[nodiscard]] uint64_t temporalMaterialRevision(uint32_t gpuIndex) const;
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
  [[nodiscard]] uint32_t appendRuntimeMesh(
      container::geometry::Mesh mesh);
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
  uint32_t resolveLoadedMaterialIndex(int32_t materialIndex) const;
  void createDescriptorSetLayout();
  void createSampler();
  void loadMaterialXMaterial();
  void loadGltfAssets();
  void loadDefaultTestSceneAssets();
  void uploadMaterialBuffer();
  void uploadTextureMetadataBuffer();
  void createSceneClipStateBuffer();
  void writeSceneClipStateBuffer();
  void collectAuthoredPunctualLights();
  void updateModelBounds();
  void allocateDescriptorSets(uint32_t descriptorSetCount);
  void writeDescriptorSetContents(
      VkDescriptorSet descriptorSet,
      const container::gpu::AllocatedBuffer& cameraBuffer,
      const container::gpu::AllocatedBuffer& objectBuffer);
  void resetLoadedAssets();
  void appendSceneAsset(const std::filesystem::path& assetPath,
                        const glm::mat4& transform,
                        std::vector<container::geometry::Mesh>& mergedMeshes);
  [[nodiscard]] std::filesystem::path resolveSceneAssetPath(
      std::string_view relativePath) const;

  container::gpu::AllocationManager* allocationManager_{nullptr};
  container::gpu::PipelineManager* pipelineManager_{nullptr};
  std::shared_ptr<container::gpu::VulkanDevice> deviceWrapper_{};
  container::app::AppConfig config_{};

  container::material::SlangMaterialXBridge materialXBridge_{};
  container::material::TextureManager textureManager_{};
  container::material::MaterialManager materialManager_{};

  container::geometry::Model model_{};
  tinygltf::Model gltfModel_{};
  std::vector<container::geometry::Vertex> vertices_{};
  std::vector<uint32_t> indices_{};
  ModelBounds modelBounds_{};
  std::vector<container::gpu::PointLightData> authoredPointLights_{};
  std::vector<AuthoredDirectionalLight> authoredDirectionalLights_{};
  std::vector<container::gpu::AreaLightData> authoredAreaLights_{};

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
