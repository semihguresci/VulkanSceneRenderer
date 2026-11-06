#pragma once

#include <cstdint>
#include <array>
#include <cstddef>
#include <compare>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "tiny_gltf.h"

#include "Container/app/AppConfig.h"
#include "Container/common/CommonMath.h"
#include "Container/common/CommonVulkan.h"
#include "Container/geometry/GltfModelLoader.h"
#include "Container/geometry/GltfAnimation.h"
#include "Container/geometry/GltfAnimationRuntime.h"
#include "Container/geometry/GltfCameraAuthoring.h"
#include "Container/geometry/GltfMaterialVariants.h"
#include "Container/geometry/GltfMaterialExport.h"
#include "Container/geometry/GltfMergeGeometry.h"
#include "Container/geometry/GltfMergeRuntimeData.h"
#include "Container/geometry/GltfSessionMerge.h"
#include "Container/utility/SceneMaterialVariants.h"
#include "Container/utility/MaterialTailReferences.h"
#include "Container/utility/ResourceLifecycle.h"
#include "Container/utility/GltfMergeGraph.h"
#include "Container/utility/GraphCloneProjection.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/GltfSceneSelection.h"
#include "Container/utility/GltfMaterialConversion.h"
#include "Container/geometry/Model.h"
#include "Container/utility/MaterialManager.h"
#include "Container/utility/MaterialSamplerKey.h"
#include "Container/utility/MaterialXIntegration.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/TextureManager.h"
#include "Container/utility/TextureSamplingPlan.h"
#include "Container/utility/TextureImageOwnership.h"
#include "Container/utility/TextureResourceLifecycle.h"
#include "Container/utility/CameraProjectionTarget.h"
#include "Container/utility/CameraViewPoseTarget.h"
#include "Container/utility/SceneGraph.h"

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

// A node binding remains distinct when several nodes share one light resource.
// An empty extension identifies node.light; area dialect bindings use their
// exact node extension name.
struct AuthoredLightBinding {
  int32_t sourceNode{-1};
  std::string extension{};
  auto operator<=>(const AuthoredLightBinding&) const = default;
};

struct AuthoredSceneLights {
  std::vector<container::gpu::PointLightData> points{};
  std::vector<AuthoredDirectionalLight> directionals{};
  std::vector<container::gpu::AreaLightData> areas{};
  std::vector<AuthoredLightBinding> pointBindings{};
  std::vector<AuthoredLightBinding> directionalBindings{};
  std::vector<AuthoredLightBinding> areaBindings{};
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

struct SceneMergeGenerationLimits {
  container::geometry::gltf::GltfMergeLimits source{};
  container::geometry::gltf::GltfMergeGeometryLimits geometry{};
  container::geometry::gltf::GltfMergeRuntimeDataLimits runtime{};
  GltfMergeGraphLimits graph{};
  uint64_t maxPreparedTextureBytes{512ull * 1024 * 1024};
  // Logical texel/block bytes of newly uploaded images, including generated
  // mip pyramids. Driver allocation padding is outside this data-size budget.
  uint64_t maxNewTextureBytes{512ull * 1024 * 1024};
  uint64_t maxMaterialBufferBytes{64ull * 1024 * 1024};
  uint64_t maxTextureMetadataBufferBytes{16ull * 1024 * 1024};
  // Detached geometry/runtime payloads, temporary source-ordered bind copies,
  // source plans and prepared images are charged together before GPU uploads.
  uint64_t maxPreparedCpuBytes{2ull * 1024 * 1024 * 1024};
};

struct MaterialGenerationLimits {
  container::geometry::gltf::GltfMaterialAuthoringLimits authoring{};
  container::geometry::gltf::GltfMaterialAnimationEditLimits animation{};
  uint64_t maxPreparedCpuBytes{512ull * 1024 * 1024};
  uint64_t maxMaterialBufferBytes{64ull * 1024 * 1024};
  uint32_t maxDescriptorFrames{1024};
  uint64_t maxMaterialCount{1'000'000};
  container::geometry::gltf::MaterialVariantGrowthLimits variantGrowth{};
  MaterialTailReferenceLimits references{};
};
enum class MaterialTailState : uint8_t { Detached, Present };
enum class MaterialRetirementState : uint8_t { Active, Retired };

struct CameraProjectionLimits {
  container::geometry::gltf::GltfCameraAuthoringLimits authoring{};
  container::geometry::gltf::GltfCameraAnimationEditLimits animation{};
  uint64_t maxPreparedCpuBytes{128ull*1024*1024}, maxGraphNodes{262144}, maxGraphWork{4'000'000};
};
struct CameraViewPoseLimits {
  container::geometry::gltf::GltfCameraPoseAuthoringLimits authoring{};
  container::geometry::gltf::GltfCameraNodeAnimationEditLimits animation{};
  ScenePlacementLimits placement{};
  uint64_t maxPreparedCpuBytes{256ull*1024*1024}, maxGraphNodes{262144}, maxGraphWork{4'000'000};
  uint64_t maxDerivedWork{512ull*1024*1024};
};

enum class MaterialTextureRoles : uint8_t { Color = 1, Data = 2, ColorAndData = 3 };
struct MaterialResourcesLimits {
  uint64_t maxPreparedCpuBytes{1024ull * 1024 * 1024};
  uint64_t maxEncodedBytes{256ull * 1024 * 1024};
  uint64_t maxNewTextureBytes{512ull * 1024 * 1024};
  uint64_t maxMetadataBufferBytes{16ull * 1024 * 1024};
  uint64_t maxSourceCount{1'000'000};
  uint64_t maxMetadataValues{4'000'000};
  uint32_t maxMetadataDepth{64};
};
struct MaterialResourceSourceView {
  std::span<const tinygltf::Image> images;
  std::span<const tinygltf::Sampler> samplers;
  std::span<const tinygltf::Texture> textures;
  std::span<const uint32_t> colorResources, dataResources;
  std::span<const int32_t> archiveToEvaluated;
  std::span<const uint8_t> imageActive,textureActive,samplerActive;
};

struct SceneBindingsLimits {
  uint64_t maxPreparedCpuBytes{32ull * 1024 * 1024};
  uint32_t maxDescriptorFrames{1024};
};

class SceneManager {
 public:
  class PreparedMergeGeneration;
  class PreparedMaterialGeneration;
  class PreparedSceneBindings;
  class MaterialAppendCommand;
  class CameraProjectionCommand;
  class PreparedCameraProjectionEdit;
  class CameraViewPoseCommand;
  class PreparedCameraViewPoseEdit;
  class MaterialRetirementCommand;
  class SourceResourceRetirementCommand;
  class PreparedSourceResourceRetirement;
  class PreparedMaterialResources;
  class MaterialResourceCommand;
  // A source texture identity and one explicit color/data resource role.
  // Resource deduplication never creates or authorizes this reference.
  class MaterialTextureReference {
   public:
    MaterialTextureReference() noexcept = default;
    explicit operator bool() const noexcept { return ownerIdentity_ && sourceIdentity_ && runtime_!=UINT32_MAX && archive_!=UINT32_MAX; }
    [[nodiscard]] uint32_t runtimeResource() const noexcept { return runtime_; }
    [[nodiscard]] uint32_t archiveTexture() const noexcept { return archive_; }
    [[nodiscard]] std::optional<uint32_t> evaluatedTexture() const noexcept {
      return evaluated_==UINT32_MAX?std::nullopt:std::optional<uint32_t>(evaluated_);
    }
    [[nodiscard]] bool isSrgb() const noexcept { return color_; }
    bool operator==(const MaterialTextureReference&) const noexcept = default;
   private:
    friend class SceneManager;
    friend class PreparedMaterialResources;
    std::shared_ptr<const uint8_t> ownerIdentity_,sourceIdentity_;
    uint32_t runtime_{UINT32_MAX},archive_{UINT32_MAX},evaluated_{UINT32_MAX};
    bool color_{};
  };
  using MaterialTextureProvenance = std::array<std::optional<MaterialTextureReference>,23>;
  [[nodiscard]] uint64_t materialResourceRevision() const noexcept { return materialResourceRevision_; }
  [[nodiscard]] size_t materialTextureResourceCount() const noexcept { return textureManager_.textureCount(); }
  [[nodiscard]] container::material::TextureImageOwnershipPtr textureImageOwnership() const noexcept { return textureImageOwnership_; }
  [[nodiscard]] TextureResourceLifecycleGenerationPtr resourceLifecycleGeneration() const noexcept { return textureResourceLifecycle_; }
  [[nodiscard]] TextureResourceReferenceOwnerPtr retainMaterialResourceReferences(
      std::span<const uint32_t> runtimeMaterials,uint64_t maxCpuBytes=64ull*1024*1024) const;
  [[nodiscard]] TextureResourceReferenceOwnerPtr retainMaterialPlanResourceReferences(
      const container::geometry::gltf::GltfMaterialAuthoringPlan&,
      const PreparedMaterialResources* resources=nullptr,uint64_t maxCpuBytes=64ull*1024*1024) const;
  [[nodiscard]] TextureResourceReferenceOwnerPtr retainSceneResourceReferences(
      const PreparedMaterialResources* resources=nullptr,uint64_t maxCpuBytes=64ull*1024*1024) const;
  [[nodiscard]] container::material::TextureImageOwnershipPtr retainMaterialImages(
      std::span<const uint32_t> runtimeMaterials,uint64_t maxCpuBytes=64ull*1024*1024) const;
  [[nodiscard]] container::material::TextureImageOwnershipPtr retainMaterialPlanImages(
      const container::geometry::gltf::GltfMaterialAuthoringPlan&,
      const PreparedMaterialResources* resources=nullptr,uint64_t maxCpuBytes=64ull*1024*1024) const;
  [[nodiscard]] std::span<const container::gpu::GpuTextureMetadata> materialTextureMetadata() const noexcept { return textureMetadata_; }
  [[nodiscard]] MaterialResourceSourceView authoredMaterialResources() const noexcept {
    return {exportSource_.images,exportSource_.samplers,exportSource_.textures,
        archiveTextureResources_.color,archiveTextureResources_.data,archiveTextureToEvaluated_,
        archiveResourcePhases_.images,archiveResourcePhases_.textures,archiveResourcePhases_.samplers};
  }
  // Preparation owns only resource tables, image payloads and new GPU leases.
  // The live catalog, sampler cache, descriptors and material remain untouched.
  [[nodiscard]] PreparedMaterialResources prepareMaterialTextureFile(
      int32_t sourceMaterial, const std::filesystem::path&, MaterialTextureRoles,
      uint32_t samplerKey = 0, const MaterialResourcesLimits& = {}) const;
  [[nodiscard]] PreparedMaterialResources prepareMaterialTextureBytes(
      int32_t sourceMaterial, const std::string& name, std::span<const std::byte>,
      MaterialTextureRoles, uint32_t samplerKey = 0, const MaterialResourcesLimits& = {}) const;
  [[nodiscard]] PreparedMaterialResources prepareMaterialTextureSource(
      int32_t sourceMaterial, uint32_t authoredTexture, MaterialTextureRoles,
      std::optional<uint32_t> samplerKey = std::nullopt, const MaterialResourcesLimits& = {}) const;
  [[nodiscard]] PreparedMaterialResources prepareMaterialResourcesChange(
      const MaterialResourceCommand&, bool forward, const MaterialResourcesLimits& = {}) const;
  [[nodiscard]] bool canExchangeMaterialResources(const PreparedMaterialResources&) const noexcept;
  // Outer material publication has already validated prospective references and
  // retired readers. Advances only resource revision; material exchange owns
  // the transaction's generation/material revisions.
  void exchangeMaterialResources(PreparedMaterialResources&) noexcept;
  // Source-only retirement leaves runtime sampling/images in service. The
  // renderer coordinator must additionally validate external provider authority
  // and retire readers before exposing an editor action.
  [[nodiscard]] PreparedSourceResourceRetirement prepareSourceResourceRetirement(
      std::span<const TextureResourceSelection>,const container::geometry::gltf::AnimationPlayer* player=nullptr,
      uint64_t maxCpuBytes=512ull*1024*1024) const;
  [[nodiscard]] std::vector<TextureResourceSelection> planResourceRetirement(TextureResourceSelection,
      const container::geometry::gltf::AnimationPlayer* player=nullptr,uint64_t maxCpuBytes=64ull*1024*1024) const;
  [[nodiscard]] PreparedSourceResourceRetirement prepareSourceResourceRetirement(
      const SourceResourceRetirementCommand&,container::util::ResourceLifecyclePhase target,
      const container::geometry::gltf::AnimationPlayer* player=nullptr,uint64_t maxCpuBytes=512ull*1024*1024) const;
  [[nodiscard]] bool canExchangeSourceResourceRetirement(const PreparedSourceResourceRetirement&,
      const container::geometry::gltf::AnimationPlayer* player=nullptr) const noexcept;
  void exchangeSourceResourceRetirement(PreparedSourceResourceRetirement&,
      const container::geometry::gltf::AnimationPlayer* player=nullptr) noexcept;
  class EditorSceneRootMembership {
   public:
    [[nodiscard]] size_t sceneIndex() const noexcept { return scene_; }
    [[nodiscard]] uint32_t nodeIndex() const noexcept { return node_; }
    [[nodiscard]] bool requiresExplicitRoot() const noexcept { return explicitRoot_; }
    explicit operator bool() const noexcept { return owner_ != nullptr && ownerIdentity_ != nullptr; }
   private:
    friend class SceneManager;
    const SceneManager* owner_{};
    std::shared_ptr<const uint8_t> ownerIdentity_;
    uint64_t graphIdentity_{}, catalogIdentity_{};
    size_t scene_{}, sceneCount_{}, sourceNodeCount_{}, insertionIndex_{};
    uint32_t node_{SceneGraph::kInvalidNode};
    bool explicitRoot_{};
  };
  class PreparedEditorSceneRootChange {
   public:
    PreparedEditorSceneRootChange() noexcept;
    ~PreparedEditorSceneRootChange();
    PreparedEditorSceneRootChange(PreparedEditorSceneRootChange&&) noexcept;
    PreparedEditorSceneRootChange& operator=(PreparedEditorSceneRootChange&&) noexcept;
    PreparedEditorSceneRootChange(const PreparedEditorSceneRootChange&) = delete;
    PreparedEditorSceneRootChange& operator=(const PreparedEditorSceneRootChange&) = delete;
    explicit operator bool() const noexcept;
    // Copy this lightweight receipt into history before initial publication.
    [[nodiscard]] const EditorSceneRootMembership& membership() const noexcept;
    [[nodiscard]] std::span<const uint32_t> candidateRoots() const noexcept;
   private:
    friend class SceneManager;
    struct State;
    std::unique_ptr<State> state_;
  };
  class ReparentSceneRootMembership {
   public:
    ReparentSceneRootMembership() = default;
    explicit operator bool() const noexcept { return bool(state_); }
    [[nodiscard]] const std::optional<std::vector<uint32_t>>& activeRoots(bool redo) const noexcept;
   private:
    friend class SceneManager;
    struct State;
    std::shared_ptr<const State> state_;
  };
  class PreparedReparentSceneRootChange {
   public:
    PreparedReparentSceneRootChange() noexcept;
    ~PreparedReparentSceneRootChange();
    PreparedReparentSceneRootChange(PreparedReparentSceneRootChange&&) noexcept;
    PreparedReparentSceneRootChange& operator=(PreparedReparentSceneRootChange&&) noexcept;
    PreparedReparentSceneRootChange(const PreparedReparentSceneRootChange&) = delete;
    PreparedReparentSceneRootChange& operator=(const PreparedReparentSceneRootChange&) = delete;
    explicit operator bool() const noexcept;
   private:
    friend class SceneManager;
    struct State;
    std::unique_ptr<State> state_;
  };
  SceneManager(container::gpu::AllocationManager& allocationManager,
               container::gpu::PipelineManager& pipelineManager,
               std::shared_ptr<container::gpu::VulkanDevice> deviceWrapper,
               const container::app::AppConfig& config);
  ~SceneManager();

  SceneManager(const SceneManager&) = delete;
  SceneManager& operator=(const SceneManager&) = delete;

  // Render-thread staging only. Incoming is a validated resident loader result;
  // all source identities are independent of runtime/procedural mesh slots.
  // New GPU resources belong solely to the returned generation. No live graph,
  // material table, descriptors, source paths or allocator arenas are replaced.
  [[nodiscard]] PreparedMergeGeneration prepareMergeGeneration(
      const container::geometry::GltfLoadResult& incoming,
      const std::filesystem::path& incomingPath,
      const container::geometry::gltf::GltfMergeOptions& options,
      const SceneGraph& baseGraph,
      const SceneMergeGenerationLimits& limits = {}) const;
  // Copies the actual current runtime subtree, including graph-only edits and
  // shared procedural/imported primitive instances. Source node/skin/animation
  // identities append without importing images or decoding geometry. Existing
  // materials, textures and leases are retained; only detached buffers,
  // samplers/descriptors and graph/source/runtime metadata are prepared.
  [[nodiscard]] PreparedMergeGeneration prepareSubtreeCloneGeneration(
      const SceneGraph& baseGraph, uint32_t rootGraphId,
      std::optional<std::string> name = std::nullopt,
      const SceneMergeGenerationLimits& limits = {}) const;
  // Requires externally retired GPU readers and a previously checked snapshot
  // belonging to this manager. Graph/controller publication is a separate step.
  // Exchanges owned asset generations, never this object's stable dependencies.
  // Reusing the receipt exchanges back for undo/redo; revisions keep increasing.
  void exchangeMergeGeneration(PreparedMergeGeneration&,
      const container::geometry::gltf::AnimationPlayer* targetPlayer=nullptr) noexcept;
  [[nodiscard]] uint64_t generationEpoch() const noexcept { return generationEpoch_; }
  [[nodiscard]] uint64_t materialAssetIdentity() const noexcept { return sceneRootCatalogIdentity_; }
  [[nodiscard]] uint64_t authoredMaterialRevision() const noexcept { return authoredMaterialRevision_; }
  // Only selected material metadata is copied; geometry, images and source
  // resource tables remain resident. UI/history prepare against current time.
  [[nodiscard]] int32_t sourceMaterialForRuntime(uint32_t runtime) const;
  // Borrowed source-to-runtime identity table for bounded inspector snapshots.
  [[nodiscard]] std::span<const uint32_t> sourceMaterialRuntimeMapping() const noexcept { return sourceMaterialToRuntime_; }
  [[nodiscard]] const tinygltf::Material& authoredMaterial(int32_t sourceMaterial) const;
  [[nodiscard]] container::geometry::gltf::GltfMaterialAuthoringPlan planMaterialEdit(
      int32_t sourceMaterial, const container::geometry::gltf::GltfMaterialPatch&,
      const container::geometry::gltf::GltfMaterialAuthoringLimits& = {}) const;
  [[nodiscard]] container::geometry::gltf::GltfMaterialAuthoringPlan planMaterialTextureEdit(
      int32_t sourceMaterial, const container::geometry::gltf::GltfMaterialPatch&,
      const PreparedMaterialResources* prospective = nullptr,
      const container::geometry::gltf::GltfMaterialAuthoringLimits& = {}) const;
  [[nodiscard]] container::geometry::gltf::GltfMaterialAuthoringPlan rebaseMaterialTextureEdit(
      const container::geometry::gltf::GltfMaterialAuthoringPlan&,
      container::geometry::gltf::GltfMaterialEditSide,
      const PreparedMaterialResources* prospective = nullptr,
      const container::geometry::gltf::GltfMaterialAuthoringLimits& = {}) const;
  // Duplicate preserves separate authored/evaluated snapshots. Make unique
  // freezes the currently visible definition into both new rests. Neither
  // operation copies animation channels or changes any graph binding.
  [[nodiscard]] container::geometry::gltf::GltfMaterialAuthoringPlan planMaterialDuplicate(
      int32_t sourceMaterial, const container::geometry::gltf::GltfMaterialPatch& = {},
      const container::geometry::gltf::GltfMaterialAuthoringLimits& = {}) const;
  [[nodiscard]] container::geometry::gltf::GltfMaterialAuthoringPlan planMaterialMakeUnique(
      int32_t sourceMaterial, const container::geometry::gltf::GltfMaterialPatch& = {},
      const container::geometry::gltf::GltfMaterialAuthoringLimits& = {}) const;
  [[nodiscard]] container::geometry::gltf::GltfMaterialAuthoringPlan rebaseMaterialEdit(
      const container::geometry::gltf::GltfMaterialAuthoringPlan&,
      container::geometry::gltf::GltfMaterialEditSide,
      const container::geometry::gltf::GltfMaterialAuthoringLimits& = {}) const;
  [[nodiscard]] container::geometry::gltf::GltfMaterialAuthoringPlan coalesceMaterialEdits(
      const container::geometry::gltf::GltfMaterialAuthoringPlan& cumulative,
      const container::geometry::gltf::GltfMaterialAuthoringPlan& next,
      const container::geometry::gltf::GltfMaterialAuthoringLimits& = {}) const;
  [[nodiscard]] PreparedMaterialGeneration prepareMaterialGeneration(
      const container::geometry::gltf::GltfMaterialAuthoringPlan&,
      const container::geometry::gltf::AnimationPlayer* currentPlayer,
      const MaterialGenerationLimits& = {}) const;
  [[nodiscard]] PreparedMaterialGeneration prepareMaterialGeneration(
      const container::geometry::gltf::GltfMaterialAuthoringPlan&,
      const container::geometry::gltf::AnimationPlayer* currentPlayer,
      PreparedMaterialResources&&, const MaterialGenerationLimits& = {}) const;
  [[nodiscard]] PreparedMaterialGeneration prepareRuntimeMaterialMakeUnique(
      uint32_t runtimeMaterial, const container::geometry::gltf::GltfMaterialPatch&,
      const container::geometry::gltf::AnimationPlayer* currentPlayer,
      const MaterialGenerationLimits& = {}) const;
  // Fresh current-prefix staging; never exchange a consumed append generation
  // for history. The owned source/runtime tail keeps its original IDs. Graph
  // bindings publish first, while the old material context still exists.
  // Audits this manager's source/graph references; the outer renderer separately
  // proves provider references and retires all GPU readers before exchange.
  [[nodiscard]] PreparedMaterialGeneration prepareMaterialTailChange(
      const MaterialAppendCommand&, MaterialTailState target, const SceneGraph&,
      const container::geometry::gltf::AnimationPlayer* currentPlayer,
      const PreparedMaterialBindingChange* prospectiveBindings = nullptr,
      const MaterialGenerationLimits& = {}) const;
  [[nodiscard]] PreparedMaterialGeneration prepareMaterialRetirement(
      uint32_t runtimeMaterial,const SceneGraph&,
      const container::geometry::gltf::AnimationPlayer* currentPlayer,
      const MaterialGenerationLimits& = {}) const;
  [[nodiscard]] PreparedMaterialGeneration prepareMaterialRetirementChange(
      const MaterialRetirementCommand&,MaterialRetirementState,const SceneGraph&,
      const container::geometry::gltf::AnimationPlayer* currentPlayer,
      const MaterialGenerationLimits& = {}) const;
  [[nodiscard]] bool materialIsActive(uint32_t runtimeMaterial) const noexcept;
  [[nodiscard]] container::util::ResourceHandle materialResourceHandle(uint32_t runtimeMaterial) const noexcept {
    return materialIsActive(runtimeMaterial) && runtimeMaterial < materialLifecycleHandles_.size()
        ? materialLifecycleHandles_[runtimeMaterial] : container::util::ResourceHandle{};
  }
  [[nodiscard]] bool authoredMaterialIsActive(int32_t sourceMaterial) const noexcept;
  [[nodiscard]] uint64_t materialIncarnation(uint32_t runtimeMaterial) const noexcept;
  [[nodiscard]] std::vector<int32_t> retiredMaterialIndices() const;
  [[nodiscard]] container::util::ResourceCatalogSnapshot materialLifecycleInventory() const;
  // Selected-row CPU availability feedback; publication independently repeats
  // all source/graph/owner checks and the frontend proves external providers.
  [[nodiscard]] std::string materialRetirementBlockReason(uint32_t runtimeMaterial,
      const SceneGraph&,const MaterialGenerationLimits& = {}) const;
  void compactRetiredMaterialExport(tinygltf::Model&) const;
  [[nodiscard]] bool canExchangeMaterialGeneration(const PreparedMaterialGeneration&,
      const container::geometry::gltf::AnimationPlayer* currentPlayer) const noexcept;
  // Caller checks the complete renderer transaction and retires readers first.
  // Edits swap selected records. Append/duplicate swap detached material tables
  // and their explicit source/runtime identity map and variant count. Geometry,
  // images, samplers, animation channels and graph bindings remain resident.
  // One-shot; history rebases its original command into a fresh generation.
  void exchangeMaterialGeneration(PreparedMaterialGeneration&,
      container::geometry::gltf::AnimationPlayer* currentPlayer) noexcept;
  // Assignment-only staging borrows the current resident material, metadata,
  // clip and image resources. It creates only a detached scene descriptor pool
  // for the prospective object buffers; authored/material tables are unchanged.
  [[nodiscard]] PreparedSceneBindings prepareSceneBindings(const SceneBindingsLimits& = {}) const;
  [[nodiscard]] PreparedSceneBindings prepareSceneBindings(const PreparedSourceResourceRetirement&,
      const SceneBindingsLimits& = {},const container::geometry::gltf::AnimationPlayer* currentPlayer=nullptr) const;
  [[nodiscard]] bool canExchangeSceneBindings(const PreparedSceneBindings&,
      const container::geometry::gltf::AnimationPlayer* currentPlayer=nullptr) const noexcept;
  // Caller checks its graph/controller transaction and retires readers first.
  // One-shot, allocation-free publication advances only the generation epoch.
  // The token retains the replaced descriptor pool until its destruction.
  void exchangeSceneBindings(PreparedSceneBindings&,
      const container::geometry::gltf::AnimationPlayer* currentPlayer=nullptr) noexcept;
  // Conservative logical CPU storage owned by this asset generation. Shared
  // GPU texture allocations, renderer/controller resources, SceneGraph/tails
  // and allocator capacity padding are accounted by their respective owners.
  // Arithmetic overflow or excessive retained metadata nesting throws before
  // a caller reserves/publishes history. Does not allocate or mutate assets.
  [[nodiscard]] uint64_t mergeHistoryResidentBytes() const;

  void initialize(const std::string& initialModelPath,
                  float importScale,
                  uint32_t descriptorSetCount);

  // CPU-only file/image decode and mesh conversion. Safe on the serial import
  // worker; the returned payload is consumed by reloadModel on the render thread.
  [[nodiscard]] static container::geometry::GltfLoadResult prepareGltfModel(
      const std::string& path,
      uintmax_t maxInputBytes = std::numeric_limits<uintmax_t>::max());

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
  const container::gpu::AllocatedBuffer& sceneClipBuffer() const {
    return sceneClipStateBuffer_;
  }
  [[nodiscard]] const container::gpu::SceneClipState& sceneClipState() const noexcept {
    return sceneClipState_;
  }

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
  [[nodiscard]] bool hasExtendedPbrMaterials() const { return hasExtendedPbrMaterials_; }
  [[nodiscard]] float importScale() const;
  const std::vector<container::geometry::PrimitiveRange>& primitiveRanges() const {
    return model_.primitiveRanges();
  }
  const container::geometry::Model& model() const { return model_; }
  // Source mesh-major definitions and runtime geometry slots have independent
  // identities. An inverse value of -1 denotes procedural geometry.
  [[nodiscard]] const std::vector<uint32_t>& sourcePrimitiveToRuntime() const {
    return sourcePrimitiveToRuntime_;
  }
  [[nodiscard]] const std::vector<int32_t>& runtimePrimitiveToSource() const {
    return runtimePrimitiveToSource_;
  }
  VkIndexType indexType() const { return indexType_; }
  uint32_t defaultMaterialIndex() const { return defaultMaterialIndex_; }
  [[nodiscard]] size_t materialCount() const {
    return materialManager_.materialCount();
  }
  [[nodiscard]] size_t gpuMaterialCount() const noexcept { return gpuMaterials_.size(); }
  [[nodiscard]] std::span<const container::gpu::GpuMaterial> gpuMaterialData() const noexcept { return gpuMaterials_; }
  [[nodiscard]] bool hasVolumeMaterials() const noexcept;
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
  uint32_t createMaterial(const container::material::Material& material,
      const MaterialTextureProvenance& provenance);
  [[nodiscard]] bool hasRuntimeMaterialProvenance(uint32_t runtimeMaterial) const noexcept;
  [[nodiscard]] MaterialTextureReference loadMaterialTextureSource(
      const std::filesystem::path& texturePath, bool isSrgb, uint32_t samplerIndex = 0);
  [[nodiscard]] MaterialTextureReference loadMaterialTextureSourceFromBytes(
      const std::string& textureName, std::span<const std::byte> encodedBytes,
      bool isSrgb, uint32_t samplerIndex = 0);
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
  [[nodiscard]] std::span<const AuthoredLightBinding> authoredPointLightBindings() const noexcept { return authoredPointBindings_; }
  [[nodiscard]] std::span<const AuthoredLightBinding> authoredDirectionalLightBindings() const noexcept { return authoredDirectionalBindings_; }
  [[nodiscard]] std::span<const AuthoredLightBinding> authoredAreaLightBindings() const noexcept { return authoredAreaBindings_; }
  // Pure CPU extraction for an explicit source scene, including inactive ones.
  // Values retain Import Scale and exclude the outer editor root transform,
  // matching the records consumed by LightingManager's scene anchor.
  [[nodiscard]] AuthoredSceneLights evaluateAuthoredSceneLights(
      const SceneGraph& graph, size_t scene, bool includeHidden = false) const;
  // Matches the ordinal tables currently consumed by LightingManager, even
  // after an editor flag changed but before the next light publication.
  [[nodiscard]] AuthoredSceneLights publishedAuthoredSceneLights() const {
    return {authoredPointLights_, authoredDirectionalLights_, authoredAreaLights_,
            authoredPointBindings_, authoredDirectionalBindings_, authoredAreaBindings_};
  }
  bool isDefaultTestSceneActive() const;
  void populateSceneGraph(SceneGraph& sceneGraph);
  [[nodiscard]] const GltfSceneCatalog& sceneCatalog() const { return sceneCatalog_; }
  [[nodiscard]] size_t selectedScene() const { return selectedScene_; }
  [[nodiscard]] GltfSceneActivation planSceneSelection(const SceneGraph&, size_t) const;
  void applySceneSelection(SceneGraph&, const GltfSceneActivation&);
  void registerActiveSceneNode(SceneGraph&, uint32_t node);
  void preserveActiveSceneMembership(uint32_t node);
  // Stage exactly one appended source-unmapped empty node. Parent activity is
  // inspected on baseGraph, before the candidate's new active view exists.
  [[nodiscard]] PreparedEditorSceneRootChange prepareEditorSceneRootRegistration(
      const SceneGraph& baseGraph, const SceneGraph& candidateGraph, uint32_t newNode) const;
  // Separate strict primitive factory contract: one new source-unmapped
  // renderable leaf, already attached to its final parent and sharing valid
  // existing runtime geometry/material identities. Always owns one selected-
  // scene additional entry, even below an active parent. The candidate view
  // lists the new root only when that parent does not already activate it.
  [[nodiscard]] PreparedEditorSceneRootChange prepareEditorPrimitiveSceneRootRegistration(
      const SceneGraph& baseGraph, const SceneGraph& candidateGraph, uint32_t newNode) const;
  // Undo sees the live new node (present=false); redo sees its retired tail ID
  // at graph.nodeCount() (present=true). PreparedTailChange owns graph payload
  // validation/restoration; this prepares only the receipt-owned membership.
  [[nodiscard]] PreparedEditorSceneRootChange prepareEditorSceneRootChange(
      const SceneGraph& graph, const EditorSceneRootMembership&, bool present) const;
  [[nodiscard]] bool canPublishEditorSceneRootChange(const PreparedEditorSceneRootChange&) const noexcept;
  // Call after the matching PreparedAppend/TailChange publication. Owner,
  // catalog/scene/generation and membership proof are checked, but graph
  // revision is deliberately independent of this metadata-only publication.
  [[nodiscard]] bool publishEditorSceneRootChange(PreparedEditorSceneRootChange&) noexcept;
  // Prototype afterGraph has the desired parent/local/sibling placement; its
  // active view need not yet include the newly uncovered subtree. The receipt
  // supplies each exact final view for SceneGraph::preparePlacement. Existing
  // registrations/order hints survive; a newly uncovered active node receives
  // one selected-scene registration owned by this reversible reparent command.
  [[nodiscard]] ReparentSceneRootMembership planReparentSceneRoots(
      const SceneGraph& beforeGraph, const SceneGraph& afterGraph, uint32_t node) const;
  [[nodiscard]] PreparedReparentSceneRootChange prepareReparentSceneRootChange(
      const SceneGraph& liveGraph, const ReparentSceneRootMembership&, bool redo) const;
  [[nodiscard]] bool canPublishReparentSceneRootChange(const PreparedReparentSceneRootChange&) const noexcept;
  // Graph placement publishes first; final metadata guards do not inspect the
  // old graph view/revision. Publication swaps prepared rows and clears dirty.
  [[nodiscard]] bool publishReparentSceneRootChange(PreparedReparentSceneRootChange&) noexcept;
  void reconcileActiveSceneRoots(SceneGraph&);
  [[nodiscard]] bool isAuthoredSceneRoot(const SceneGraph&, uint32_t node) const;
  void refreshActiveScene(const SceneGraph&);
  // Saves the primary editable scene. BIM/USD sidecars and simulation setup
  // belong to their own authoring formats and are excluded by the caller.
  void saveScene(const SceneGraph& sceneGraph,
                 const std::filesystem::path& destination) const;
  [[nodiscard]] tinygltf::Model buildSceneExport(const SceneGraph& sceneGraph) const;
  // Raw graph-slot IDs in buildSceneExport's initial node table. Apply this
  // projection only after light/camera overlays; appended export wrappers use
  // different IDs and never belong to a retired graph subtree.
  [[nodiscard]] std::vector<int32_t> retiredSceneExportNodes(const SceneGraph& sceneGraph) const;
  [[nodiscard]] const container::geometry::gltf::MaterialVariantLibrary& materialVariants() const { return materialVariants_; }
  [[nodiscard]] std::optional<size_t> selectedMaterialVariant() const { return selectedMaterialVariant_; }
  [[nodiscard]] MaterialVariantGraphEdit planMaterialVariant(const SceneGraph&,std::optional<size_t>) const;
  bool applyMaterialVariant(SceneGraph&,const MaterialVariantGraphEdit&,bool forward=true);
  [[nodiscard]] tinygltf::Model& animationSource() { return gltfModel_; }
  [[nodiscard]] const tinygltf::Model& animationSource() const { return gltfModel_; }
  [[nodiscard]] container::geometry::gltf::GltfCameraAuthoringSourceView authoredCameraSourceView() const noexcept {
    return container::geometry::gltf::GltfCameraSourceView(exportSource_);
  }
  [[nodiscard]] const tinygltf::Camera& authoredCamera(int32_t camera) const;
  [[nodiscard]] uint64_t cameraAssetIdentity() const noexcept { return cameraAssetIdentity_; }
  [[nodiscard]] uint64_t cameraRevision() const noexcept { return cameraProjectionRevision_; }
  [[nodiscard]] std::optional<CameraProjectionTarget> cameraProjectionTarget(const SceneGraph&,uint32_t graphNode) const noexcept;
  [[nodiscard]] PreparedCameraProjectionEdit prepareCameraProjectionEdit(const CameraProjectionTarget&,
      const container::geometry::gltf::GltfCameraProjectionPatch&,const SceneGraph&,
      const container::geometry::gltf::AnimationPlayer*,float viewportAspect,const CameraProjectionLimits& = {}) const;
  [[nodiscard]] PreparedCameraProjectionEdit prepareCameraProjectionReplay(const CameraProjectionCommand&,
      container::geometry::gltf::GltfCameraEditSide,const SceneGraph&,
      const container::geometry::gltf::AnimationPlayer*,float viewportAspect,const CameraProjectionLimits& = {}) const;
  [[nodiscard]] bool canExchangeCameraProjectionEdit(const PreparedCameraProjectionEdit&,const SceneGraph&,
      const container::geometry::gltf::AnimationPlayer*,float viewportAspect) const noexcept;
  bool exchangeCameraProjectionEdit(PreparedCameraProjectionEdit&,const SceneGraph&,
      container::geometry::gltf::AnimationPlayer*,float viewportAspect) noexcept;
  [[nodiscard]] std::optional<CameraViewPoseTarget> cameraViewPoseTarget(const SceneGraph&,uint32_t graphNode) const noexcept;
  [[nodiscard]] PreparedCameraViewPoseEdit prepareCameraViewPoseEdit(const CameraViewPoseTarget&,
      const glm::mat4& desiredUnjitteredView,const SceneGraph&,
      const container::geometry::gltf::AnimationPlayer*,float viewportAspect,const CameraViewPoseLimits& = {}) const;
  [[nodiscard]] PreparedCameraViewPoseEdit prepareCameraViewPoseReplay(const CameraViewPoseCommand&,
      container::geometry::gltf::GltfCameraEditSide,const SceneGraph&,
      const container::geometry::gltf::AnimationPlayer*,float viewportAspect,const CameraViewPoseLimits& = {}) const;
  // Fresh Write requires the current unjittered view. History replay restores
  // the original local endpoint at today's parent/clock without navigating.
  [[nodiscard]] bool canExchangeCameraViewPoseEdit(const PreparedCameraViewPoseEdit&,const SceneGraph&,
      const container::geometry::gltf::AnimationPlayer*,float viewportAspect,
      const glm::mat4* currentUnjitteredView = nullptr) const noexcept;
  bool exchangeCameraViewPoseEdit(PreparedCameraViewPoseEdit&,SceneGraph&,
      container::geometry::gltf::AnimationPlayer*,float viewportAspect,
      const glm::mat4* currentUnjitteredView = nullptr) noexcept;
  // Exact live glTF texture-ID space, independent of archive-only export
  // textures and manually registered runtime provider resources. UINT32_MAX
  // means this role has no uploaded resource; a future binding requires its
  // own staged upload rather than an implicit fallback or handle lookup.
  [[nodiscard]] const container::material::GltfTextureResources& gltfTextureResources() const noexcept {
    return gltfTextureResources_;
  }
  [[nodiscard]] const container::geometry::gltf::DeformationData* deformationData() const {
    return deformation_ ? &*deformation_ : nullptr;
  }
  [[nodiscard]] const container::geometry::gltf::GltfInstancingData& instancingData() const noexcept {
    return gltfInstancing_;
  }
  void publishAnimationChanges(
      const container::geometry::gltf::AnimationDirtySet& dirty,
      const SceneGraph& graph);
  [[nodiscard]] uint32_t appendRuntimeMesh(
      container::geometry::Mesh mesh);
  [[nodiscard]] MaterialRenderProperties materialRenderProperties(
      uint32_t materialIndex) const;
  // Explicit resident normal-slot provenance. No map returns nullopt; an
  // invalid runtime material throws instead of resolving the scene fallback.
  [[nodiscard]] std::optional<uint32_t> materialNormalTexCoord(uint32_t runtime) const;
  // Imported primitive's retained tangent UV basis, independent of current
  // material bindings. Empty for unproved/procedural primitives, inconsistent
  // source mappings or an absent source TEXCOORD accessor. Never allocates.
  [[nodiscard]] std::optional<uint32_t> retainedPrimitiveNormalTexCoord(uint32_t runtimePrimitive) const noexcept;

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
  friend struct SceneTextureSamplingTestAccess;
  friend struct SceneSamplerPublicationTestAccess;
  friend struct SceneTextureImageOwnershipTestAccess;
  struct TextureSamplingFallback {
    container::material::TextureResource resource;
    std::shared_ptr<const container::gpu::AllocationManager::TextureAllocationLease> lease;
  };
  struct PreparedTextureDescriptorState {
    PreparedTextureDescriptorState()=default;
    PreparedTextureDescriptorState(const PreparedTextureDescriptorState&)=delete;
    PreparedTextureDescriptorState& operator=(const PreparedTextureDescriptorState&)=delete;
    container::gpu::AllocationManager* allocation{};
    std::shared_ptr<container::gpu::VulkanDevice> device;
    std::shared_ptr<const TextureSamplingFallback> fallback;
    container::material::MaterialSamplerSlotPlan slots;
    std::vector<VkSampler> samplers,createdSamplers,descriptorSamplers;
    std::vector<VkDescriptorImageInfo> images;
    std::vector<container::gpu::GpuTextureMetadata> metadata;
    container::material::TextureSamplingPlan sampling;
    container::gpu::AllocatedBuffer metadataBuffer;
    uint64_t ownedBytes{};
    ~PreparedTextureDescriptorState();
  };
  [[nodiscard]] std::shared_ptr<PreparedTextureDescriptorState> prepareTextureDescriptorState(
      const container::material::TextureManager&,uint64_t maxCpuBytes,uint64_t maxMetadataBufferBytes) const;
  [[nodiscard]] bool textureDescriptorStateMatches(const PreparedTextureDescriptorState&,
      const container::material::TextureManager&) const noexcept;
  void writeTextureDescriptorSetContents(VkDescriptorSet,const container::gpu::AllocatedBuffer& camera,
      const container::gpu::AllocatedBuffer& objects,const container::gpu::AllocatedBuffer& materials,size_t materialCount,
      const PreparedTextureDescriptorState&) const;
  void writeTextureDescriptorSetContents(VkDescriptorSet,const container::gpu::AllocatedBuffer& camera,
      const container::gpu::AllocatedBuffer& objects,const container::gpu::AllocatedBuffer& materials,size_t materialCount,
      const container::gpu::AllocatedBuffer& metadata,size_t metadataCount,std::span<const VkDescriptorImageInfo>,
      std::span<const VkSampler>) const;
  void exchangeTextureDescriptorState(PreparedTextureDescriptorState&) noexcept;
  void ensureTextureSamplingFallback();
  [[nodiscard]] bool textureSamplingFallbackValid() const noexcept;
  [[nodiscard]] bool textureSamplingStateValid() const noexcept;
  [[nodiscard]] bool textureSamplingStateValidForResources() const noexcept;
  [[nodiscard]] bool textureSamplingStateValidImpl(bool allowAppendedPrefix) const noexcept;
  void finishTextureImageOwnershipPublication();
  [[nodiscard]] TextureResourceLifecycleView textureResourceLifecycleView() const noexcept;
  [[nodiscard]] bool sourceResourceActive(TextureResourceSelection) const noexcept;
  void refreshTextureResourceLifecycle(container::util::ResourceRetirementPolicy policy=
      container::util::ResourceRetirementPolicy::BlockPreparations,uint64_t maxCpuBytes=512ull*1024*1024,
      const container::geometry::gltf::AnimationPlayer* player=nullptr) const;
  struct RuntimeMaterialProvenance;
  struct SourceResourcePhaseTable {
    std::vector<uint8_t> images,textures,samplers;
    // Empty tables retain the compact all-active representation. Appends and
    // owned suffix replay extend only an already explicit phase table.
    void resize(size_t imageCount,size_t textureCount,size_t samplerCount) {
      if(!images.empty()) images.resize(imageCount,1);
      if(!textures.empty()) textures.resize(textureCount,1);
      if(!samplers.empty()) samplers.resize(samplerCount,1);
    }
    bool operator==(const SourceResourcePhaseTable&) const noexcept=default;
  };
  struct MaterialResourceDependencyState;
  using MaterialResourceDependencyStatePtr=std::shared_ptr<const MaterialResourceDependencyState>;
  struct MaterialResourceDependencyInput {
    uint32_t runtime{};
    int32_t source{INT32_MIN};
    container::util::ResourceHandle material;
    const container::material::Material* value{};
    const tinygltf::Material* authored{};
    const tinygltf::Material* evaluated{};
    const tinygltf::Material* exported{};
    const tinygltf::Material* rest{};
    const container::geometry::gltf::ImportedMaterialTextureBindings* exportBindings{};
    const RuntimeMaterialProvenance* provenance{};
    std::optional<std::span<const int32_t>> exportMappingOverride;
  };
  [[nodiscard]] MaterialResourceDependencyStatePtr stageMaterialResourceDependency(
      const MaterialResourceDependencyInput&,PreparedTextureResourceLifecycle* mutation,
      PreparedMaterialResources* resources,const MaterialResourceDependencyStatePtr& outgoing={},
      uint64_t maxCpuBytes=512ull*1024*1024) const;
  [[nodiscard]] std::vector<MaterialResourceDependencyStatePtr> stageCurrentMaterialResourceDependencies(
      PreparedTextureResourceLifecycle&,uint64_t maxCpuBytes=512ull*1024*1024,
      std::span<const MaterialResourceDependencyStatePtr> outgoingOverride={},
      const container::geometry::gltf::AnimationPlayer* player=nullptr,
      std::span<const RuntimeMaterialProvenance> provenanceOverride={},uint32_t skipRuntime=UINT32_MAX,
      std::optional<std::span<const int32_t>> exportMappingOverride=std::nullopt) const;
  [[nodiscard]] std::vector<MaterialResourceDependencyStatePtr> stageResourceMaterialDependencies(
      PreparedMaterialResources&,uint32_t runtime,container::util::ResourceHandle material,
      const container::geometry::gltf::AnimationPlayer* player,uint64_t maxCpuBytes) const;
  [[nodiscard]] TextureResourceLiveOwnerPtr materialResourceLiveOwner(uint32_t runtime) const noexcept;
  [[nodiscard]] uint64_t materialResourceDependencyBytes() const noexcept;
  [[nodiscard]] static uint64_t materialResourceDependencyMapBytes(
      std::span<const MaterialResourceDependencyStatePtr> next,
      std::span<const MaterialResourceDependencyStatePtr> prior={}) noexcept;
  [[nodiscard]] bool materialResourceDependenciesValid(
      const container::geometry::gltf::AnimationPlayer* player=nullptr) const noexcept;
  [[nodiscard]] bool materialResourceDependencyMapMatches(
      std::span<const MaterialResourceDependencyStatePtr>,const TextureResourceLifecycleGenerationPtr&,
      const container::geometry::gltf::AnimationPlayer* player=nullptr,bool prospective=false,
      std::optional<std::span<const int32_t>> exportMappingOverride=std::nullopt) const noexcept;
  [[nodiscard]] bool materialResourceDependencyMapWillBeActive(
      std::span<const MaterialResourceDependencyStatePtr>,const PreparedTextureResourceLifecycle&) const noexcept;
  [[nodiscard]] bool canPrepareMaterialResources(const PreparedMaterialResources&) const noexcept;
  [[nodiscard]] PreparedSourceResourceRetirement prepareSourceResourceRetirementImpl(
      std::span<const TextureResourceSelection>,const SourceResourceRetirementCommand*,
      container::util::ResourceLifecyclePhase,const container::geometry::gltf::AnimationPlayer*,uint64_t,bool prepareGpu=true) const;
  uint32_t createMaterialTracked(const container::material::Material&,RuntimeMaterialProvenance);
  [[nodiscard]] PreparedMaterialGeneration prepareMaterialGenerationImpl(
      const container::geometry::gltf::GltfMaterialAuthoringPlan&,
      const container::geometry::gltf::AnimationPlayer*, const MaterialGenerationLimits&,
      PreparedMaterialResources*,const MaterialAppendCommand* replayAppend=nullptr) const;
  void resetMaterialLifecycle(std::span<const uint8_t> retired={});
  void resetCameraProjectionNamespace();
  void inheritCameraProjectionNamespace(const SceneManager&);
  [[nodiscard]] uint64_t cameraProjectionNamespaceBytes() const noexcept;
  [[nodiscard]] PreparedCameraProjectionEdit prepareCameraProjectionImpl(
      const CameraProjectionTarget&,container::geometry::gltf::GltfCameraProjectionPlan&&,
      const CameraProjectionCommand*,container::geometry::gltf::GltfCameraEditSide,const SceneGraph&,
      const container::geometry::gltf::AnimationPlayer*,float,const CameraProjectionLimits&) const;
  [[nodiscard]] PreparedCameraViewPoseEdit prepareCameraViewPoseImpl(
      const CameraViewPoseTarget&,container::geometry::gltf::GltfCameraPosePlan&&,
      const CameraViewPoseCommand*,container::geometry::gltf::GltfCameraEditSide,const SceneGraph&,
      const container::geometry::gltf::AnimationPlayer*,float,const CameraViewPoseLimits&) const;
  [[nodiscard]] static uint64_t materialLifecyclePreparationBytes(size_t materialCount);
  [[nodiscard]] std::vector<uint8_t> materialRetirementMask() const;
  [[nodiscard]] MaterialReferenceAudit auditUnusedMaterialReferences(
      uint32_t,const SceneGraph&,const MaterialGenerationLimits&) const;
  [[nodiscard]] PreparedMaterialGeneration prepareMaterialRetirementImpl(
      const MaterialRetirementCommand&,MaterialRetirementState,const SceneGraph&,
      const container::geometry::gltf::AnimationPlayer*,const MaterialGenerationLimits&) const;
  [[nodiscard]] PreparedMaterialResources prepareMaterialResourcesBase(
      int32_t sourceMaterial, const MaterialResourcesLimits&,bool resourceOnly=false) const;
  uint32_t queryTextureDescriptorCapacity() const;
  uint32_t queryMaterialSamplerDescriptorCapacity() const;
  void validateMaterialSamplerPolicies(std::span<const container::material::MaterialSamplerKey> keys) const;
  void validateGltfSamplerPolicies(const tinygltf::Model& model) const;
  uint32_t resolveLoadedMaterialIndex(int32_t materialIndex) const;
  uint32_t resolvePrimitiveMaterialIndex(size_t sourcePrimitive,int32_t materialIndex) const;
  void createDescriptorSetLayout();
  void createSampler();
  void loadMaterialXMaterial();
  void loadGltfAssets(container::geometry::GltfLoadResult* prepared = nullptr,
      const container::material::PreparedGltfTextures* preparedTextures = nullptr);
  void loadDefaultTestSceneAssets();
  void uploadMaterialBuffer();
  void uploadTextureMetadataBuffer(uint64_t maxLogicalCpuBytes=512ull*1024*1024);
  static void validateGltfTextureResourceTable(
      const container::material::GltfTextureResources&, size_t sourceTextureCount,
      size_t runtimeResourceCount);
  void createSceneClipStateBuffer();
  void writeSceneClipStateBuffer();
  void collectAuthoredPunctualLights(const SceneGraph* graph = nullptr);
  [[nodiscard]] AuthoredSceneLights evaluateAuthoredSceneLights(
      const SceneGraph* graph, size_t scene, bool includeHidden = false) const;
  [[nodiscard]] GltfSceneActivation planSceneSelection(
      const SceneGraph&, size_t, uint64_t maxObjects) const;
  void updateModelBounds(const SceneGraph* graph = nullptr);
  [[nodiscard]] ModelBounds calculateModelBounds(const SceneGraph* graph = nullptr) const;
  void allocateDescriptorSets(uint32_t descriptorSetCount);
  void writeDescriptorSetContents(
      VkDescriptorSet descriptorSet,
      const container::gpu::AllocatedBuffer& cameraBuffer,
      const container::gpu::AllocatedBuffer& objectBuffer);
  void writeMaterialDescriptorSetContents(VkDescriptorSet,
      const container::gpu::AllocatedBuffer& cameraBuffer,
      const container::gpu::AllocatedBuffer& objectBuffer,
      const container::gpu::AllocatedBuffer& materialBuffer, size_t materialCount) const;
  [[nodiscard]] static container::gpu::GpuMaterial packMaterial(const container::material::Material&);
  [[nodiscard]] bool materialGenerationMatches(const PreparedMaterialGeneration&,
      const container::geometry::gltf::AnimationPlayer*) const noexcept;
  [[nodiscard]] bool materialGenerationProvenanceMatches(const PreparedMaterialGeneration&) const noexcept;
  [[nodiscard]] bool materialRetirementMatches(const PreparedMaterialGeneration&) const noexcept;
  [[nodiscard]] bool sceneBindingsMatch(const PreparedSceneBindings&,
      const container::geometry::gltf::AnimationPlayer* currentPlayer=nullptr) const noexcept;
  void resetLoadedAssets();
  void invalidateEditorSceneRootMembership() noexcept;
  [[nodiscard]] PreparedEditorSceneRootChange prepareEditorSceneRootMembership(
      const SceneGraph& baseGraph, uint32_t newNode, bool explicitRoot) const;
  void rememberExportMaterials(const tinygltf::Model& source,
                               const std::filesystem::path& sourcePath,
                               const container::material::GltfTextureResources& textureToResource,
                               std::span<const uint32_t> sourceMaterialToRuntime);
  void validateMaterialResourceSource(const tinygltf::Model& source) const;
  [[nodiscard]] MaterialTextureReference loadMaterialTextureSourceBytes(
      const std::string& requestKey, const std::string& textureName,
      std::span<const std::byte> encodedBytes, bool isSrgb, uint32_t samplerIndex);
  [[nodiscard]] bool materialTextureReferenceMatches(const MaterialTextureReference&,bool color,
      uint32_t runtime) const noexcept;
  [[nodiscard]] MaterialTextureReference materialTextureReference(uint32_t archive,bool color) const;
  void rememberRuntimeMaterialProvenance(uint32_t runtime,const tinygltf::Material& authored,
      const tinygltf::Material& evaluated);
  [[nodiscard]] RuntimeMaterialProvenance makeRuntimeMaterialProvenance(uint32_t runtime,
      const container::material::Material& candidate,const tinygltf::Material& authored,
      const tinygltf::Material& evaluated) const;
  [[nodiscard]] bool runtimeMaterialProvenanceMatches(const RuntimeMaterialProvenance&,
      const container::material::Material&) const noexcept;
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
  std::vector<uint32_t> sourcePrimitiveToRuntime_{};
  std::vector<int32_t> runtimePrimitiveToSource_{};
  tinygltf::Model gltfModel_{};
  // Construct map sentinels during ordinary manager preparation. Generic
  // std::swap(Model) move-constructs a temporary and allocates on MSVC despite
  // TinyGLTF's declared noexcept moves; publication reuses this scratch instead.
  tinygltf::Model modelExchangeScratch_{};
  container::geometry::gltf::GltfInstancingData gltfInstancing_{};
  GltfSceneCatalog sceneCatalog_{};
  size_t selectedScene_{};
  std::vector<std::vector<uint32_t>> additionalSceneRoots_{};
  std::vector<std::vector<uint32_t>> sceneRootOrder_{};
  bool sceneRootsDirty_{};
  // Manager-owned proof counters deliberately do not exchange with an asset
  // generation. Lightweight history receipts survive later merge+undo, while
  // one-shot prepared tokens pin the current generation and these revisions.
  uint64_t sceneRootMembershipRevision_{}, sceneRootCatalogIdentity_{1};
  // Receipts retain this unique marker, preventing placement-new/address-reuse
  // from making a destroyed manager's metadata authority valid again.
  std::shared_ptr<const uint8_t> sceneRootOwnerIdentity_{std::make_shared<const uint8_t>(0)};
  bool mergedAssets_{false};
  std::optional<container::geometry::gltf::DeformationData> deformation_{};
  container::geometry::gltf::MaterialVariantLibrary materialVariants_{};
  std::optional<size_t> selectedMaterialVariant_{};
  std::optional<container::geometry::Model> variantOriginalModel_{};
  std::optional<container::geometry::gltf::DeformationData> variantOriginalDeformation_{};
  // Source-flat tangent UV policy captured for every imported primitive,
  // including static assets without variants; updated on frame regeneration.
  std::vector<uint32_t> variantNormalTexCoords_{};
  tinygltf::Model exportSource_{};
  struct CameraProjectionIdentity;
  struct CameraProjectionNamespace;
  std::shared_ptr<const CameraProjectionNamespace> cameraProjectionNamespace_;
  uint64_t cameraAssetIdentity_{},cameraProjectionRevision_{1},cameraEvaluationRevision_{1};
  container::material::GltfTextureResources gltfTextureResources_{};
  // These are forward source-indexed identities, never inverse GPU lookups.
  container::material::GltfTextureResources archiveTextureResources_{};
  std::vector<int32_t> archiveTextureToEvaluated_{};
  std::vector<std::shared_ptr<const uint8_t>> archiveTextureIdentities_{};
  std::shared_ptr<const uint8_t> materialResourceOwnerIdentity_{std::make_shared<const uint8_t>(0)};
  uint64_t materialResourceRevision_{1};
  std::shared_ptr<const uint8_t> materialResourceVacancy_;
  struct RuntimeMaterialProvenance {
    bool known{};
    MaterialTextureProvenance slots;
    bool operator==(const RuntimeMaterialProvenance&) const noexcept = default;
  };
  std::vector<RuntimeMaterialProvenance> runtimeMaterialProvenance_{};
  struct MaterialTextureSourceRequest {
    std::string key;
    std::shared_ptr<const std::vector<std::byte>> encoded;
    uint32_t archive{},evaluated{};
    std::shared_ptr<const uint8_t> sourceIdentity;
    // Sampler-only aliases prove their original source with this retained
    // identity, so a reused numeric archive ID cannot hit an old request.
    std::shared_ptr<const uint8_t> inputSourceIdentity;
    bool active{true};
  };
  std::vector<MaterialTextureSourceRequest> materialTextureSourceRequests_{};
  std::filesystem::path sourcePath_{};
  std::vector<std::optional<tinygltf::Material>> exportMaterialSources_{};
  std::vector<std::optional<container::geometry::gltf::ImportedMaterialTextureBindings>> exportMaterialTextureBindings_{};
  std::vector<int32_t> exportTextureIndices_{};
  std::vector<container::geometry::Vertex> vertices_{};
  std::vector<uint32_t> indices_{};
  uint64_t geometryRevision_{0};
  uint64_t materialRevision_{0};
  uint64_t generationEpoch_{0};
  uint64_t authoredMaterialRevision_{0};
  ModelBounds modelBounds_{};
  std::vector<container::gpu::PointLightData> authoredPointLights_{};
  std::vector<AuthoredDirectionalLight> authoredDirectionalLights_{};
  std::vector<container::gpu::AreaLightData> authoredAreaLights_{};
  std::vector<AuthoredLightBinding> authoredPointBindings_{};
  std::vector<AuthoredLightBinding> authoredDirectionalBindings_{};
  std::vector<AuthoredLightBinding> authoredAreaBindings_{};

  VkIndexType indexType_{VK_INDEX_TYPE_UINT32};

  glm::vec4 materialBaseColor_{1.0f};
  uint32_t defaultMaterialIndex_{std::numeric_limits<uint32_t>::max()};
  uint32_t diagnosticMaterialIndex_{std::numeric_limits<uint32_t>::max()};
  // Source definitions retain their IDs when runtime/manual materials are
  // interleaved by imports. Runtime material identity is never a base offset.
  std::vector<uint32_t> sourceMaterialToRuntime_{};
  // Missing baseline/manual entries mean null ownership. Owned appended rows
  // carry non-reused markers through merge/clone, independent of numeric IDs.
  std::vector<std::shared_ptr<const uint8_t>> materialEntryOwners_{};
  std::shared_ptr<const uint8_t> materialTailVacancy_{};
  // Catalog slots are monotonic internal identities, NOT runtime row numbers.
  // A physically detached owned tail may acquire a new numeric incarnation;
  // arbitrary retired middle rows remain resident and are never reused.
  std::shared_ptr<container::util::ResourceLifecycleCatalog> materialLifecycle_{};
  container::util::ResourceLifecycleDomain materialLifecycleDomain_{};
  std::vector<container::util::ResourceHandle> materialLifecycleHandles_{};
  uint32_t materialLifecycleNextSlot_{};
  // Implicit (-1) material defaults belong to their source import, rather than
  // sharing a previously edited scene fallback. Empty is the legacy table.
  std::vector<uint32_t> sourcePrimitiveImplicitMaterials_{};

  std::vector<container::gpu::GpuMaterial> gpuMaterials_{};
  bool hasExtendedPbrMaterials_{false};
  std::vector<uint64_t> gpuMaterialTemporalRevisions_{};
  container::gpu::AllocatedBuffer materialBuffer_{};
  size_t materialBufferCapacity_{0};
  std::vector<container::gpu::GpuTextureMetadata> textureMetadata_{};
  container::material::TextureSamplingPlan textureSamplingPlan_{};
  std::shared_ptr<const TextureSamplingFallback> textureSamplingFallback_{};
  container::gpu::AllocatedBuffer textureMetadataBuffer_{};
  size_t textureMetadataBufferCapacity_{0};
  container::gpu::SceneClipState sceneClipState_{};
  container::gpu::AllocatedBuffer sceneClipStateBuffer_{};

  VkSampler baseColorSampler_{VK_NULL_HANDLE};
  std::vector<VkSampler> materialSamplers_{};
  container::material::MaterialSamplerSlotPlan materialSamplerSlots_{};
  VkDescriptorSetLayout descriptorSetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool descriptorPool_{VK_NULL_HANDLE};
  bool ownsDescriptorPool_{false};
  std::vector<VkDescriptorSet> descriptorSets_{};
  std::vector<VkDescriptorSet> auxiliaryDescriptorSets_{};
  uint32_t textureDescriptorCapacity_{1};
  uint32_t materialSamplerDescriptorCapacity_{1};
  // Detached generations retain complete immutable per-image coverage.
  // Ordinary replacement keeps its explicit reader-retired force-reset contract.
  bool resetTextureAllocationsOnReset_{true};
  container::material::TextureImageOwnershipPtr textureImageOwnership_{};
  mutable TextureResourceLifecycleGenerationPtr textureResourceLifecycle_{};
  mutable SourceResourcePhaseTable archiveResourcePhases_,gltfResourcePhases_;
  mutable std::vector<MaterialResourceDependencyStatePtr> materialResourceDependencies_{};
};

class SceneManager::MaterialResourceCommand {
 public:
  MaterialResourceCommand() noexcept = default;
  explicit operator bool() const noexcept;
  [[nodiscard]] int32_t sourceMaterial() const noexcept;
  [[nodiscard]] bool published() const noexcept;
  [[nodiscard]] bool present() const noexcept;
  [[nodiscard]] uint64_t ownedBytes() const noexcept;
  [[nodiscard]] container::material::TextureImageOwnershipPtr textureImageOwnership() const noexcept;
  [[nodiscard]] TextureResourceReferenceOwnerPtr resourceReferences() const noexcept;
 private:
  friend class SceneManager;
  friend class PreparedMaterialResources;
  struct State;
  std::shared_ptr<State> state_;
};

class SceneManager::PreparedMaterialResources {
 public:
  PreparedMaterialResources() noexcept;
  ~PreparedMaterialResources();
  PreparedMaterialResources(PreparedMaterialResources&&) noexcept;
  PreparedMaterialResources& operator=(PreparedMaterialResources&&) noexcept;
  PreparedMaterialResources(const PreparedMaterialResources&) = delete;
  PreparedMaterialResources& operator=(const PreparedMaterialResources&) = delete;
  explicit operator bool() const noexcept;
  [[nodiscard]] uint32_t sourceTexture() const noexcept;
  [[nodiscard]] uint32_t evaluatedTexture() const noexcept;
  [[nodiscard]] MaterialTextureReference reference(bool color) const;
  [[nodiscard]] MaterialTextureReference reference(uint32_t authoredTexture, bool color) const;
  [[nodiscard]] bool matchesReference(const MaterialTextureReference&, bool color, uint32_t runtime) const noexcept;
  [[nodiscard]] MaterialTextureProvenance makeProvenance(
      const container::material::Material&, const tinygltf::Material& authored,
      const tinygltf::Material& evaluated) const;
  [[nodiscard]] MaterialTextureProvenance forwardProvenance(
      const MaterialTextureProvenance&, const container::material::Material&) const;
  [[nodiscard]] bool provenanceMatches(const MaterialTextureProvenance&, const container::material::Material&) const noexcept;
  [[nodiscard]] container::geometry::gltf::GltfMaterialAuthoringSourceView authoredSourceView() const noexcept;
  [[nodiscard]] container::geometry::gltf::GltfMaterialAuthoringSourceView evaluatedSourceView() const noexcept;
  [[nodiscard]] container::material::GltfMaterialConversionContext conversionContext() const noexcept;
  [[nodiscard]] const container::material::TextureManager& textures() const;
  [[nodiscard]] const container::gpu::AllocatedBuffer& metadataBuffer() const;
  [[nodiscard]] std::span<const VkDescriptorImageInfo> imageInfos() const noexcept;
  [[nodiscard]] std::span<const VkSampler> descriptorSamplers() const noexcept;
  [[nodiscard]] container::material::TextureImageOwnershipPtr textureImageOwnership() const noexcept;
  [[nodiscard]] TextureResourceLifecycleGenerationPtr resourceLifecycleGeneration() const noexcept;
  [[nodiscard]] std::span<const int32_t> exportTextureIndices() const noexcept;
  [[nodiscard]] TextureResourceReferenceOwnerPtr retainResourceReferences(
      std::span<const TextureResourceSelection>,container::util::ResourcePinClass,
      uint64_t maxCpuBytes=64ull*1024*1024,bool opaque=false) const;
  [[nodiscard]] TextureResourceReferenceOwnerPtr retainAllResourceReferences(
      container::util::ResourcePinClass,uint64_t maxCpuBytes=64ull*1024*1024) const;
  [[nodiscard]] TextureResourceLiveOwnerPtr stageLiveOwner(TextureResourceLiveOwnerKey,
      std::span<const TextureResourceSelection>,const TextureResourceLiveOwnerPtr& outgoing={},
      uint64_t maxCpuBytes=64ull*1024*1024,bool opaque=false);
  void releaseLiveOwners(std::span<const TextureResourceLiveOwnerPtr>);
  void sealLiveDependencies();
  [[nodiscard]] bool liveOwnerWillBeActive(const TextureResourceLiveOwnerPtr&) const noexcept;
  [[nodiscard]] MaterialResourceCommand command() const noexcept;
  [[nodiscard]] uint64_t ownedBytes() const noexcept;
  void writeDescriptorSetContents(VkDescriptorSet, const container::gpu::AllocatedBuffer& camera,
      const container::gpu::AllocatedBuffer& objects, const container::gpu::AllocatedBuffer& materials,
      size_t materialCount) const;
 private:
  friend class SceneManager;
  struct State;
  std::unique_ptr<State> state_;
  void prepareSource(std::optional<uint32_t> source, const std::string& request,
      const std::string& name, std::span<const std::byte> bytes, MaterialTextureRoles,
      std::optional<uint32_t> sampler);
  void finishPreparation();
};

class SceneManager::CameraProjectionCommand {
 public:
  CameraProjectionCommand() noexcept = default;
  explicit operator bool() const noexcept;
  [[nodiscard]] bool published() const noexcept;
  [[nodiscard]] bool applied() const noexcept;
  [[nodiscard]] uint64_t ownedBytes() const noexcept;
  [[nodiscard]] CameraProjectionTarget target() const noexcept;
 private:
  friend class SceneManager;
  struct State;
  std::shared_ptr<State> state_;
};

class SceneManager::PreparedCameraProjectionEdit {
 public:
  PreparedCameraProjectionEdit() noexcept;
  ~PreparedCameraProjectionEdit();
  PreparedCameraProjectionEdit(PreparedCameraProjectionEdit&&) noexcept;
  PreparedCameraProjectionEdit& operator=(PreparedCameraProjectionEdit&&) noexcept;
  PreparedCameraProjectionEdit(const PreparedCameraProjectionEdit&) = delete;
  PreparedCameraProjectionEdit& operator=(const PreparedCameraProjectionEdit&) = delete;
  explicit operator bool() const noexcept;
  [[nodiscard]] bool changed() const noexcept;
  [[nodiscard]] uint64_t ownedBytes() const noexcept;
  [[nodiscard]] CameraProjectionCommand command() const noexcept;
  [[nodiscard]] const container::geometry::gltf::AnimatedCameraState& candidateView() const;
 private:
  friend class SceneManager;
  struct State;
  std::unique_ptr<State> state_;
};

class SceneManager::CameraViewPoseCommand {
 public:
  CameraViewPoseCommand() noexcept = default;
  explicit operator bool() const noexcept;
  [[nodiscard]] bool published() const noexcept;
  [[nodiscard]] bool applied() const noexcept;
  [[nodiscard]] uint64_t ownedBytes() const noexcept;
  [[nodiscard]] CameraViewPoseTarget target() const noexcept;
 private:
  friend class SceneManager;
  struct State;
  std::shared_ptr<State> state_;
};

class SceneManager::PreparedCameraViewPoseEdit {
 public:
  PreparedCameraViewPoseEdit() noexcept;
  ~PreparedCameraViewPoseEdit();
  PreparedCameraViewPoseEdit(PreparedCameraViewPoseEdit&&) noexcept;
  PreparedCameraViewPoseEdit& operator=(PreparedCameraViewPoseEdit&&) noexcept;
  PreparedCameraViewPoseEdit(const PreparedCameraViewPoseEdit&) = delete;
  PreparedCameraViewPoseEdit& operator=(const PreparedCameraViewPoseEdit&) = delete;
  explicit operator bool() const noexcept;
  [[nodiscard]] bool changed() const noexcept;
  [[nodiscard]] uint64_t ownedBytes() const noexcept;
  [[nodiscard]] CameraViewPoseCommand command() const noexcept;
  [[nodiscard]] const SceneGraph& candidateGraph() const;
  [[nodiscard]] const container::geometry::gltf::AnimatedCameraState& candidateView() const;
  [[nodiscard]] const AuthoredSceneLights& candidateLights() const;
  [[nodiscard]] const ModelBounds& candidateBounds() const;
 private:
  friend class SceneManager;
  struct State;
  std::unique_ptr<State> state_;
};

class SceneManager::MaterialAppendCommand {
 public:
  MaterialAppendCommand() noexcept = default;
  explicit operator bool() const noexcept;
  [[nodiscard]] int32_t sourceMaterial() const noexcept;
  [[nodiscard]] uint32_t runtimeMaterial() const noexcept;
  [[nodiscard]] bool published() const noexcept;
  [[nodiscard]] bool present() const noexcept;
  [[nodiscard]] uint64_t ownedBytes() const noexcept;
  [[nodiscard]] container::material::TextureImageOwnershipPtr textureImageOwnership() const noexcept;
  [[nodiscard]] TextureResourceReferenceOwnerPtr resourceReferences() const noexcept;
 private:
  friend class SceneManager;
  struct State;
  std::shared_ptr<State> state_;
};

class SceneManager::MaterialRetirementCommand {
 public:
  MaterialRetirementCommand() noexcept=default;
  explicit operator bool() const noexcept;
  [[nodiscard]] int32_t sourceMaterial() const noexcept;
  [[nodiscard]] uint32_t runtimeMaterial() const noexcept;
  [[nodiscard]] bool published() const noexcept;
  [[nodiscard]] bool retired() const noexcept;
  [[nodiscard]] uint64_t ownedBytes() const noexcept;
  [[nodiscard]] container::material::TextureImageOwnershipPtr textureImageOwnership() const noexcept;
  [[nodiscard]] TextureResourceReferenceOwnerPtr resourceReferences() const noexcept;
 private:
  friend class SceneManager;
  struct State;
  std::shared_ptr<State> state_;
};

class SceneManager::PreparedMaterialGeneration {
 public:
  PreparedMaterialGeneration() noexcept;
  ~PreparedMaterialGeneration();
  PreparedMaterialGeneration(PreparedMaterialGeneration&&) noexcept;
  PreparedMaterialGeneration& operator=(PreparedMaterialGeneration&&) noexcept;
  PreparedMaterialGeneration(const PreparedMaterialGeneration&) = delete;
  PreparedMaterialGeneration& operator=(const PreparedMaterialGeneration&) = delete;
  explicit operator bool() const noexcept;
  [[nodiscard]] int32_t sourceMaterial() const noexcept;
  [[nodiscard]] uint32_t runtimeMaterial() const noexcept;
  [[nodiscard]] bool routingChanged() const noexcept;
  [[nodiscard]] bool hasExtendedPbrMaterials() const noexcept;
  [[nodiscard]] uint64_t materialRevision() const noexcept;
  [[nodiscard]] uint64_t generationEpoch() const noexcept;
  [[nodiscard]] std::span<const MaterialRenderProperties> materialProperties() const noexcept;
  [[nodiscard]] std::span<const container::gpu::GpuMaterial> gpuMaterials() const noexcept;
  [[nodiscard]] uint64_t temporalMaterialRevision(uint32_t material) const noexcept;
  [[nodiscard]] bool rayGeometryCompatible(uint32_t material) const noexcept;
  [[nodiscard]] const container::gpu::AllocatedBuffer& materialBuffer() const;
  [[nodiscard]] VkDescriptorSet descriptorSet(uint32_t image) const noexcept;
  [[nodiscard]] VkDescriptorSet auxiliaryDescriptorSet(uint32_t image) const noexcept;
  [[nodiscard]] const tinygltf::Material& evaluatedMaterial() const;
  // Copy into bounded history before initial publication. Empty for factor
  // edits. Receipts retain CPU material records/leases, never frame resources.
  [[nodiscard]] MaterialAppendCommand appendCommand() const noexcept;
  [[nodiscard]] MaterialRetirementCommand retirementCommand() const noexcept;
  [[nodiscard]] container::material::TextureImageOwnershipPtr historyTextureImageOwnership() const noexcept;
  [[nodiscard]] TextureResourceReferenceOwnerPtr historyResourceReferences() const noexcept;
  [[nodiscard]] std::optional<uint32_t> retiringMaterial() const noexcept;
  // Detached descriptor pool; every frame binds either the matching candidate
  // routing buffer or the unchanged live buffer. No live descriptor is written.
  void prepareDescriptorFrames(std::span<const container::gpu::AllocatedBuffer> cameras,
      std::span<const container::gpu::AllocatedBuffer> primaryObjects,
      std::span<const container::gpu::AllocatedBuffer> auxiliaryObjects = {});
 private:
  friend class SceneManager;
  struct State;
  std::unique_ptr<State> state_;
};

class SceneManager::PreparedSceneBindings {
 public:
  PreparedSceneBindings() noexcept;
  ~PreparedSceneBindings();
  PreparedSceneBindings(PreparedSceneBindings&&) noexcept;
  PreparedSceneBindings& operator=(PreparedSceneBindings&&) noexcept;
  PreparedSceneBindings(const PreparedSceneBindings&) = delete;
  PreparedSceneBindings& operator=(const PreparedSceneBindings&) = delete;
  explicit operator bool() const noexcept;
  [[nodiscard]] uint64_t generationEpoch() const noexcept;
  [[nodiscard]] VkDescriptorSet descriptorSet(uint32_t image) const noexcept;
  [[nodiscard]] VkDescriptorSet auxiliaryDescriptorSet(uint32_t image) const noexcept;
  // Every frame receives fresh primary and auxiliary sets. A one-entry object
  // table broadcasts to every frame; omitted auxiliary objects use primary.
  void prepareDescriptorFrames(std::span<const container::gpu::AllocatedBuffer> cameras,
      std::span<const container::gpu::AllocatedBuffer> primaryObjects,
      std::span<const container::gpu::AllocatedBuffer> auxiliaryObjects = {},
      const container::geometry::gltf::AnimationPlayer* currentPlayer=nullptr);
 private:
  friend class SceneManager;
  struct State;
  std::unique_ptr<State> state_;
};

class SceneManager::SourceResourceRetirementCommand {
 public:
  SourceResourceRetirementCommand() noexcept=default;
  explicit operator bool() const noexcept;
  [[nodiscard]] bool published() const noexcept;
  [[nodiscard]] bool retired() const noexcept;
  [[nodiscard]] uint64_t ownedBytes() const noexcept;
  [[nodiscard]] std::span<const TextureResourceSelection> selections() const noexcept;
  [[nodiscard]] TextureResourceReferenceOwnerPtr references() const noexcept;
  [[nodiscard]] container::material::TextureImageOwnershipPtr textureImageOwnership() const noexcept;
 private:
  struct State;
  std::shared_ptr<State> state_;
  friend class SceneManager;
  friend class PreparedSourceResourceRetirement;
};

class SceneManager::PreparedSourceResourceRetirement {
 public:
  PreparedSourceResourceRetirement() noexcept;
  ~PreparedSourceResourceRetirement();
  PreparedSourceResourceRetirement(PreparedSourceResourceRetirement&&) noexcept;
  PreparedSourceResourceRetirement& operator=(PreparedSourceResourceRetirement&&) noexcept;
  PreparedSourceResourceRetirement(const PreparedSourceResourceRetirement&)=delete;
  PreparedSourceResourceRetirement& operator=(const PreparedSourceResourceRetirement&)=delete;
  explicit operator bool() const noexcept;
  [[nodiscard]] SourceResourceRetirementCommand command() const noexcept;
  [[nodiscard]] bool runtimeChanged() const noexcept;
  [[nodiscard]] size_t textureResourceCount() const noexcept;
  [[nodiscard]] const container::material::TextureManager& textureResources() const;
  [[nodiscard]] bool textureActive(uint32_t) const noexcept;
  [[nodiscard]] container::material::TextureImageOwnershipPtr textureImageOwnership() const noexcept;
  [[nodiscard]] TextureResourceLifecycleGenerationPtr resourceLifecycleGeneration() const noexcept;
  [[nodiscard]] const container::gpu::AllocatedBuffer& metadataBuffer() const;
  [[nodiscard]] std::span<const VkDescriptorImageInfo> imageInfos() const noexcept;
  [[nodiscard]] std::span<const VkSampler> descriptorSamplers() const noexcept;
  [[nodiscard]] const container::material::TextureSamplingPlan& samplingPlan() const;
  [[nodiscard]] TextureResourceReferenceOwnerPtr retainAllResourceReferences(container::util::ResourcePinClass,
      uint64_t maxCpuBytes=64ull*1024*1024) const;
  void writeDescriptorSetContents(VkDescriptorSet,const container::gpu::AllocatedBuffer& camera,
      const container::gpu::AllocatedBuffer& objects,const container::gpu::AllocatedBuffer& materials,size_t materialCount,
      const container::geometry::gltf::AnimationPlayer* currentPlayer=nullptr) const;
  [[nodiscard]] bool publicationMatches(const SceneManager&,bool consumed=false,
      const container::geometry::gltf::AnimationPlayer* currentPlayer=nullptr) const noexcept;
 private:
  [[nodiscard]] bool markDescriptorFramesReady(const std::shared_ptr<const uint8_t>&) const noexcept;
  struct State;
  std::shared_ptr<State> state_;
  friend class SceneManager;
  friend class PreparedSceneBindings;
};

class SceneManager::PreparedMergeGeneration {
 public:
  PreparedMergeGeneration(PreparedMergeGeneration&&) noexcept;
  PreparedMergeGeneration& operator=(PreparedMergeGeneration&&) noexcept;
  ~PreparedMergeGeneration();
  PreparedMergeGeneration(const PreparedMergeGeneration&) = delete;
  PreparedMergeGeneration& operator=(const PreparedMergeGeneration&) = delete;

  [[nodiscard]] SceneManager& manager() noexcept { return *manager_; }
  [[nodiscard]] const SceneManager& manager() const noexcept { return *manager_; }
  [[nodiscard]] SceneGraph& graph() noexcept { return *graph_; }
  [[nodiscard]] const SceneGraph& graph() const noexcept { return *graph_; }
  [[nodiscard]] const container::geometry::gltf::GltfSessionMergePlan& sourcePlan() const noexcept { return *sources_; }
  [[nodiscard]] bool isSubtreeClone() const noexcept { return cloneRoot_!=SceneGraph::kInvalidNode; }
  [[nodiscard]] const container::geometry::gltf::GltfProjectedSubtreeClonePlan& projectedClonePlan() const noexcept {
    return *cloneSources_;
  }
  [[nodiscard]] const container::geometry::gltf::GltfProjectedCloneOwnershipProof& cloneOwnership() const noexcept {
    return cloneOwnership_;
  }
  [[nodiscard]] uint32_t cloneRoot() const noexcept { return cloneRoot_; }
  // Appended-tail order, including nonvisual graph-only dependency contexts.
  // Frontend product/physics safety checks must examine these original IDs.
  [[nodiscard]] std::span<const uint32_t> cloneOriginalGraphNodes() const noexcept { return cloneOriginalGraphNodes_; }
  [[nodiscard]] uint64_t expectedGenerationEpoch() const noexcept { return expectedGenerationEpoch_; }
  [[nodiscard]] uint64_t expectedGraphRevision() const noexcept { return expectedGraphRevision_; }
  // Initial publication proof only. Undo/redo validate their own graph receipt.
  [[nodiscard]] bool readyForExchange(const SceneManager&, const SceneGraph&,
      const container::geometry::gltf::AnimationPlayer* targetPlayer=nullptr) const noexcept;
  // Only the inactive generation is touched. History replay after a swapchain
  // image-count change must replace these sets before binding current buffers.
  void prepareDescriptorFrames(uint32_t imageCount);
  // Fallible initial/history preparation. Shared-catalog clones rebuild the
  // target's live resource edges against the actual current generation before
  // the allocation-free asset exchange.
  void prepareResourceDependencies(const SceneManager& live,
      const container::geometry::gltf::AnimationPlayer* targetPlayer=nullptr,
      uint64_t maxCpuBytes=512ull*1024*1024);
  [[nodiscard]] bool resourceDependenciesReadyForExchange(const SceneManager& live,
      const container::geometry::gltf::AnimationPlayer* targetPlayer=nullptr) const noexcept;
  [[nodiscard]] uint64_t resourceDependencyPreparationBytes() const noexcept;
  void releaseResourceDependencyPreparation() noexcept;
  // After initial publication, replay needs only manager(), plus the caller's
  // graph tail receipt. Release the full source-plan/candidate-graph copies;
  // graph()/sourcePlan()/projectedClonePlan()/cloneOwnership() require retained
  // preparation data; cloneOriginalGraphNodes() becomes empty. The initial proof
  // readyForExchange() returns false after release.
  // Resource ownership and descriptor restaging remain intact for undo/redo.
  void releasePreparationData() noexcept;

 private:
  friend class SceneManager;
  PreparedMergeGeneration() = default;
  const SceneManager* owner_{};
  std::shared_ptr<const uint8_t> ownerIdentity_;
  std::unique_ptr<SceneManager> manager_;
  struct ResourceDependencyPreparation;
  std::unique_ptr<ResourceDependencyPreparation> resourceDependencies_;
  std::unique_ptr<SceneGraph> graph_;
  std::unique_ptr<container::geometry::gltf::GltfSessionMergePlan> sources_;
  std::unique_ptr<container::geometry::gltf::GltfProjectedSubtreeClonePlan> cloneSources_;
  container::geometry::gltf::GltfProjectedCloneOwnershipProof cloneOwnership_;
  std::vector<uint32_t> cloneOriginalGraphNodes_;
  uint32_t cloneRoot_{SceneGraph::kInvalidNode};
  uint64_t expectedGenerationEpoch_{}, expectedGeometryRevision_{}, expectedMaterialRevision_{};
  uint64_t expectedMaterialResourceRevision_{};
  std::weak_ptr<const container::material::TextureImageOwnership> expectedTextureImageOwnership_;
  std::weak_ptr<const TextureResourceLifecycleGeneration> expectedTextureResourceLifecycle_;
  uint64_t expectedTextureSamplingRevision_{};
  size_t expectedTextureSamplingRows_{};
  uint64_t expectedGraphIdentity_{}, expectedGraphRevision_{};
  uint64_t expectedSceneRootMembershipRevision_{};
  size_t expectedSelectedScene_{};
};

}  // namespace container::scene
