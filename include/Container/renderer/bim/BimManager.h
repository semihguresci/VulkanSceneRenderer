#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/geometry/Vertex.h"
#include "Container/renderer/bim/BimCoordinationOverlay.h"
#include "Container/renderer/bim/BimDrawingExport.h"
#include "Container/renderer/bim/BimFloorPlanOverlayData.h"
#include "Container/renderer/bim/BimRelationshipGraph.h"
#include "Container/renderer/bim/BimSectionCapBuilder.h"
#include "Container/renderer/bim/BimSemanticColorMode.h"
#include "Container/renderer/debug/DebugOverlayRenderer.h"
#include "Container/scene/SceneProvider.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace container::gpu {
class AllocationManager;
class PipelineManager;
class VulkanDevice;
struct AllocatedBuffer;
} // namespace container::gpu

namespace container::geometry::dotbim {
struct Model;
} // namespace container::geometry::dotbim

namespace container::scene {
class SceneManager;
} // namespace container::scene

namespace container::renderer {

class BimDrawFilterState;
class BimMetadataCatalog;
class BimMetadataIndex;
struct BimDrawFilterStateInputs;

struct BimPickHit {
  uint32_t objectIndex{std::numeric_limits<uint32_t>::max()};
  float distance{std::numeric_limits<float>::max()};
  float depth{0.0f};
  glm::vec3 worldPosition{0.0f};
  bool hasWorldPosition{false};
  bool hit{false};
};

struct BimElementBounds {
  bool valid{false};
  glm::vec3 min{0.0f};
  glm::vec3 max{0.0f};
  glm::vec3 center{0.0f};
  glm::vec3 size{0.0f};
  float radius{0.0f};
  float floorElevation{0.0f};
};

struct BimCoordinationMarkerDrawData {
  container::gpu::BufferSlice vertexSlice{};
  container::gpu::BufferSlice indexSlice{};
  VkIndexType indexType{VK_INDEX_TYPE_UINT32};
  std::vector<DrawCommand> issuePinDrawCommands{};
  std::vector<DrawCommand> clashDrawCommands{};

  [[nodiscard]] bool valid() const {
    return vertexSlice.buffer != VK_NULL_HANDLE &&
           indexSlice.buffer != VK_NULL_HANDLE &&
           (!issuePinDrawCommands.empty() || !clashDrawCommands.empty());
  }
};

struct BimSectionPlaneVisualDrawData {
  container::gpu::BufferSlice vertexSlice{};
  container::gpu::BufferSlice indexSlice{};
  VkIndexType indexType{VK_INDEX_TYPE_UINT32};
  std::vector<DrawCommand> drawCommands{};

  [[nodiscard]] bool valid() const {
    return vertexSlice.buffer != VK_NULL_HANDLE &&
           indexSlice.buffer != VK_NULL_HANDLE && !drawCommands.empty();
  }
};

namespace detail {
[[nodiscard]] size_t bimCoordinationMarkerGeometrySignatureForTesting(
    const BimCoordinationOverlayResult &overlay, float markerRadius,
    uint64_t objectDataRevision);

[[nodiscard]] size_t bimSectionPlaneVisualGeometrySignatureForTesting(
    const glm::vec4 &plane, float size, const glm::vec3 &color,
    uint64_t objectDataRevision);
} // namespace detail

struct BimStoreyRange {
  std::string label{};
  float minElevation{0.0f};
  float maxElevation{0.0f};
  size_t objectCount{0};
};

struct BimModelUnitMetadata {
  bool hasSourceUnits{false};
  std::string sourceUnits{};
  bool hasMetersPerUnit{false};
  float metersPerUnit{1.0f};
  bool hasImportScale{false};
  float importScale{1.0f};
  bool hasEffectiveImportScale{false};
  float effectiveImportScale{1.0f};
};

struct BimModelGeoreferenceMetadata {
  bool hasSourceUpAxis{false};
  std::string sourceUpAxis{};
  bool hasCoordinateOffset{false};
  glm::dvec3 coordinateOffset{0.0};
  std::string coordinateOffsetSource{};
  std::string crsName{};
  std::string crsAuthority{};
  std::string crsCode{};
  std::string mapConversionName{};
};

enum class BimGeometryKind : uint32_t {
  Mesh = 0,
  Points = 1,
  Curves = 2,
};

struct BimElementProperty {
  std::string set{};
  std::string name{};
  std::string value{};
  std::string category{};
};

struct BimElementMetadata {
  uint32_t objectIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t sourceElementIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t meshId{0};
  uint32_t sourceMaterialIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t materialIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t semanticTypeId{std::numeric_limits<uint32_t>::max()};
  uint32_t productIdentityId{0};
  glm::vec4 sourceColor{0.8f, 0.82f, 0.86f, 1.0f};
  std::string guid{};
  std::string type{};
  std::string displayName{};
  std::string objectType{};
  std::string storeyName{};
  std::string storeyId{};
  std::string materialName{};
  std::string materialCategory{};
  std::string discipline{};
  std::string phase{};
  std::string fireRating{};
  std::string loadBearing{};
  std::string status{};
  std::string sourceId{};
  std::vector<BimElementProperty> properties{};
  bool transparent{false};
  bool doubleSided{false};
  BimElementBounds bounds{};
  BimGeometryKind geometryKind{BimGeometryKind::Mesh};
};

[[nodiscard]] inline bool
sameBimProductIdentity(const BimElementMetadata &selected,
                       const BimElementMetadata &candidate) {
  constexpr uint32_t invalidObjectIndex = std::numeric_limits<uint32_t>::max();
  if (selected.objectIndex != invalidObjectIndex &&
      selected.objectIndex == candidate.objectIndex) {
    return true;
  }
  if (!selected.guid.empty() && !candidate.guid.empty()) {
    return selected.guid == candidate.guid;
  }
  if (!selected.sourceId.empty() && selected.sourceId == candidate.sourceId) {
    return true;
  }
  return false;
}

struct BimIdentityStringHash {
  using is_transparent = void;

  [[nodiscard]] size_t operator()(std::string_view value) const noexcept {
    return std::hash<std::string_view>{}(value);
  }

  [[nodiscard]] size_t operator()(const std::string &value) const noexcept {
    return (*this)(std::string_view(value));
  }

  [[nodiscard]] size_t operator()(const char *value) const noexcept {
    return (*this)(std::string_view(value != nullptr ? value : ""));
  }
};

struct BimIdentityStringEqual {
  using is_transparent = void;

  [[nodiscard]] bool operator()(std::string_view lhs,
                                std::string_view rhs) const noexcept {
    return lhs == rhs;
  }
};

using BimObjectIndexLookup =
    std::unordered_map<std::string, std::vector<uint32_t>,
                       BimIdentityStringHash, BimIdentityStringEqual>;

struct BimSceneStats {
  size_t objectCount{0};
  size_t meshObjectCount{0};
  size_t pointObjectCount{0};
  size_t curveObjectCount{0};
  size_t opaqueDrawCount{0};
  size_t transparentDrawCount{0};
  size_t pointOpaqueDrawCount{0};
  size_t pointTransparentDrawCount{0};
  size_t curveOpaqueDrawCount{0};
  size_t curveTransparentDrawCount{0};
  size_t nativePointOpaqueDrawCount{0};
  size_t nativePointTransparentDrawCount{0};
  size_t nativeCurveOpaqueDrawCount{0};
  size_t nativeCurveTransparentDrawCount{0};
  size_t meshletClusterCount{0};
  size_t meshletSourceClusterCount{0};
  size_t meshletEstimatedClusterCount{0};
  size_t meshletObjectReferenceCount{0};
  size_t meshletGpuResidentObjectCount{0};
  size_t meshletGpuResidentClusterCount{0};
  VkDeviceSize meshletGpuBufferBytes{0};
  bool meshletGpuComputeReady{false};
  bool meshletGpuDispatchPending{false};
  uint32_t meshletMaxLodLevel{0};
  size_t gpuCompactionInputDrawCount{0};
  size_t gpuCompactionOutputCapacity{0};
  VkDeviceSize gpuCompactionBufferBytes{0};
  bool gpuCompactionReady{false};
  bool gpuCompactionDispatchPending{false};
  bool optimizedModelMetadataCacheable{false};
  bool optimizedModelMetadataCacheHit{false};
  bool optimizedModelMetadataCacheStale{false};
  bool optimizedModelMetadataCacheWriteSucceeded{false};
  size_t floorPlanDrawCount{0};
  size_t uniqueTypeCount{0};
  size_t uniqueStoreyCount{0};
  size_t uniqueMaterialCount{0};
  size_t uniqueDisciplineCount{0};
  size_t uniquePhaseCount{0};
  size_t uniqueFireRatingCount{0};
  size_t uniqueLoadBearingCount{0};
  size_t uniqueStatusCount{0};
};

enum class BimDisciplinePreset : uint32_t {
  None = 0,
  Architecture = 1,
  MepXray = 2,
};

struct BimDrawFilter {
  bool typeFilterEnabled{false};
  std::string type{};
  bool storeyFilterEnabled{false};
  std::string storey{};
  bool materialFilterEnabled{false};
  std::string material{};
  bool disciplineFilterEnabled{false};
  std::string discipline{};
  bool phaseFilterEnabled{false};
  std::string phase{};
  bool fireRatingFilterEnabled{false};
  std::string fireRating{};
  bool loadBearingFilterEnabled{false};
  std::string loadBearing{};
  bool statusFilterEnabled{false};
  std::string status{};
  bool drawBudgetEnabled{false};
  uint32_t drawBudgetMaxObjects{0};
  bool isolateSelection{false};
  bool hideSelection{false};
  uint32_t selectedObjectIndex{std::numeric_limits<uint32_t>::max()};
  BimDisciplinePreset disciplinePreset{BimDisciplinePreset::None};
  bool phaseTimelineEnabled{false};
  uint32_t phaseTimelineActiveIndex{0};
  bool phaseTimelineShowExisting{true};
  bool phaseTimelineShowNew{true};
  bool phaseTimelineShowDemolished{false};
  bool phaseTimelineGhostFuture{false};

  [[nodiscard]] bool active() const {
    return (typeFilterEnabled && !type.empty()) ||
           (storeyFilterEnabled && !storey.empty()) ||
           (materialFilterEnabled && !material.empty()) ||
           (disciplineFilterEnabled && !discipline.empty()) ||
           (phaseFilterEnabled && !phase.empty()) ||
           (fireRatingFilterEnabled && !fireRating.empty()) ||
           (loadBearingFilterEnabled && !loadBearing.empty()) ||
           (statusFilterEnabled && !status.empty()) ||
           (drawBudgetEnabled && drawBudgetMaxObjects > 0u) ||
           ((isolateSelection || hideSelection) &&
            selectedObjectIndex != std::numeric_limits<uint32_t>::max()) ||
           disciplinePreset != BimDisciplinePreset::None ||
           phaseTimelineEnabled;
  }

  [[nodiscard]] bool requiresCpuFiltering() const {
    return disciplinePreset != BimDisciplinePreset::None ||
           phaseTimelineEnabled;
  }
};

struct BimMeshletClusterMetadata {
  uint32_t meshId{0};
  uint32_t firstIndex{0};
  uint32_t indexCount{0};
  uint32_t triangleCount{0};
  uint32_t lodLevel{0};
  glm::vec3 boundsCenter{0.0f};
  float boundsRadius{0.0f};
  bool estimated{false};
};

struct BimObjectLodStreamingMetadata {
  uint32_t objectIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t sourceElementIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t meshId{0};
  BimGeometryKind geometryKind{BimGeometryKind::Mesh};
  uint32_t firstCluster{0};
  uint32_t clusterCount{0};
  uint32_t maxLodLevel{0};
  uint32_t triangleCount{0};
  bool estimatedClusters{false};
};

struct BimMeshletGpuCluster {
  glm::uvec4 drawRange{0};
  glm::uvec4 lodInfo{0};
  glm::vec4 boundsCenterRadius{0.0f};
};

struct BimMeshletGpuObjectLod {
  glm::uvec4 objectInfo{0};
  glm::uvec4 clusterInfo{0};
};

struct BimMeshletResidencyEntry {
  uint32_t flags{0};
  uint32_t selectedLod{0};
  uint32_t firstCluster{0};
  uint32_t clusterCount{0};
};

struct BimMeshletResidencyPushConstants {
  uint32_t objectCount{0};
  uint32_t forceResident{0};
  uint32_t drawBudgetMaxObjects{0};
  uint32_t selectedObjectIndex{std::numeric_limits<uint32_t>::max()};
  int32_t lodBias{0};
  float screenErrorPixels{2.0f};
  float viewportHeightPixels{1.0f};
  uint32_t flags{0};
};

struct BimMeshletResidencySettings {
  bool autoLod{true};
  bool drawBudgetEnabled{false};
  bool pauseStreaming{false};
  bool keepSelectedResident{true};
  bool forceResident{false};
  uint32_t drawBudgetMaxObjects{0};
  uint32_t selectedObjectIndex{std::numeric_limits<uint32_t>::max()};
  int32_t lodBias{0};
  float screenErrorPixels{2.0f};
  float viewportHeightPixels{1.0f};
};

struct BimMeshletResidencyStats {
  bool gpuResident{false};
  bool computeReady{false};
  bool dispatchPending{false};
  size_t objectCount{0};
  size_t clusterCount{0};
  size_t residentObjectCount{0};
  size_t residentClusterCount{0};
  VkDeviceSize clusterBufferBytes{0};
  VkDeviceSize objectLodBufferBytes{0};
  VkDeviceSize residencyBufferBytes{0};
};

struct BimVisibilityGpuObjectMetadata {
  glm::uvec4 semanticIds{0};
  glm::uvec4 propertyIds{0};
  glm::uvec4 identity{0};
};

struct BimVisibilityFilterPushConstants {
  uint32_t objectCount{0};
  uint32_t flags{0};
  uint32_t drawBudgetMaxObjects{0};
  uint32_t selectedObjectIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t typeId{0};
  uint32_t storeyId{0};
  uint32_t materialId{0};
  uint32_t disciplineId{0};
  uint32_t phaseId{0};
  uint32_t fireRatingId{0};
  uint32_t loadBearingId{0};
  uint32_t statusId{0};
  uint32_t selectedProductId{0};
  uint32_t pad0{0};
  uint32_t pad1{0};
  uint32_t pad2{0};
};

struct BimVisibilityFilterStats {
  bool gpuResident{false};
  bool computeReady{false};
  bool dispatchPending{false};
  size_t objectCount{0};
  VkDeviceSize metadataBufferBytes{0};
  VkDeviceSize visibilityMaskBufferBytes{0};
};

struct BimDrawCompactionPushConstants {
  uint32_t inputDrawCount{0};
  uint32_t outputCapacity{0};
  uint32_t objectCount{0};
  uint32_t flags{0};
};

struct BimDrawCompactionStats {
  bool gpuResident{false};
  bool computeReady{false};
  bool dispatchPending{false};
  bool drawsValid{false};
  size_t inputDrawCount{0};
  size_t outputCapacity{0};
  VkDeviceSize inputBufferBytes{0};
  VkDeviceSize outputBufferBytes{0};
  VkDeviceSize countBufferBytes{0};
};

enum class BimDrawCompactionSlot : uint32_t {
  OpaqueSingleSided = 0,
  OpaqueWindingFlipped = 1,
  OpaqueDoubleSided = 2,
  TransparentSingleSided = 3,
  TransparentWindingFlipped = 4,
  TransparentDoubleSided = 5,
  NativePointOpaque = 6,
  NativePointTransparent = 7,
  NativeCurveOpaque = 8,
  NativeCurveTransparent = 9,
  Count = 10,
};

inline constexpr size_t kBimDrawCompactionSlotCount =
    static_cast<size_t>(BimDrawCompactionSlot::Count);

struct BimDrawCompactionSlotResources {
  std::vector<container::gpu::GpuDrawIndexedIndirectCommand> uploadScratch{};
  container::gpu::AllocatedBuffer inputBuffer{};
  container::gpu::AllocatedBuffer indirectBuffer{};
  container::gpu::AllocatedBuffer countBuffer{};
  BimDrawCompactionStats stats{};
  const DrawCommand *inputSourceData{nullptr};
  size_t inputSourceSize{0};
  uint64_t inputSourceRevision{0};
  bool descriptorsDirty{false};
  bool dispatchPending{false};
};

static_assert(
    sizeof(BimMeshletGpuCluster) == 48,
    "BimMeshletGpuCluster must match shaders/bim_meshlet_residency.slang.");
static_assert(
    sizeof(BimMeshletGpuObjectLod) == 32,
    "BimMeshletGpuObjectLod must match shaders/bim_meshlet_residency.slang.");
static_assert(
    sizeof(BimMeshletResidencyEntry) == 16,
    "BimMeshletResidencyEntry must match shaders/bim_meshlet_residency.slang.");
static_assert(sizeof(BimMeshletResidencyPushConstants) == 32,
              "BimMeshletResidencyPushConstants must match "
              "shaders/bim_meshlet_residency.slang.");
static_assert(sizeof(BimVisibilityGpuObjectMetadata) == 48,
              "BimVisibilityGpuObjectMetadata must match "
              "shaders/bim_visibility_filter.slang.");
static_assert(sizeof(BimVisibilityFilterPushConstants) == 64,
              "BimVisibilityFilterPushConstants must match "
              "shaders/bim_visibility_filter.slang.");
static_assert(sizeof(BimDrawCompactionPushConstants) == 16,
              "BimDrawCompactionPushConstants must match "
              "shaders/bim_draw_compact.slang.");

struct BimOptimizedModelMetadata {
  bool cacheable{false};
  bool hasSourceMeshletClusters{false};
  bool cacheHit{false};
  bool cacheStale{false};
  bool cacheWriteAttempted{false};
  bool cacheWriteSucceeded{false};
  std::string cacheKey{};
  std::string cachePath{};
  std::string cacheStatus{};
  size_t meshObjectCount{0};
  size_t meshletClusterCount{0};
  size_t sourceMeshletClusterCount{0};
  size_t estimatedMeshletClusterCount{0};
  size_t objectClusterReferenceCount{0};
  uint32_t maxLodLevel{0};
};

struct BimDrawBudgetLodStats {
  bool enabled{false};
  uint32_t maxObjects{0};
  size_t visibleObjectCount{0};
  size_t visibleMeshObjectCount{0};
  size_t visibleMeshletClusterReferences{0};
  uint32_t visibleMaxLodLevel{0};
};

struct BimGeometryDrawLists {
  std::vector<DrawCommand> opaqueDrawCommands{};
  std::vector<DrawCommand> opaqueSingleSidedDrawCommands{};
  std::vector<DrawCommand> opaqueWindingFlippedDrawCommands{};
  std::vector<DrawCommand> opaqueDoubleSidedDrawCommands{};
  std::vector<DrawCommand> transparentDrawCommands{};
  std::vector<DrawCommand> transparentSingleSidedDrawCommands{};
  std::vector<DrawCommand> transparentWindingFlippedDrawCommands{};
  std::vector<DrawCommand> transparentDoubleSidedDrawCommands{};

  void clear() {
    opaqueDrawCommands.clear();
    opaqueSingleSidedDrawCommands.clear();
    opaqueWindingFlippedDrawCommands.clear();
    opaqueDoubleSidedDrawCommands.clear();
    transparentDrawCommands.clear();
    transparentSingleSidedDrawCommands.clear();
    transparentWindingFlippedDrawCommands.clear();
    transparentDoubleSidedDrawCommands.clear();
  }

  void reserve(size_t opaqueCount, size_t transparentCount) {
    opaqueDrawCommands.reserve(opaqueCount);
    opaqueSingleSidedDrawCommands.reserve(opaqueCount);
    opaqueWindingFlippedDrawCommands.reserve(opaqueCount);
    opaqueDoubleSidedDrawCommands.reserve(opaqueCount);
    transparentDrawCommands.reserve(transparentCount);
    transparentSingleSidedDrawCommands.reserve(transparentCount);
    transparentWindingFlippedDrawCommands.reserve(transparentCount);
    transparentDoubleSidedDrawCommands.reserve(transparentCount);
  }
};

struct BimDrawLists {
  std::vector<DrawCommand> opaqueDrawCommands{};
  std::vector<DrawCommand> opaqueSingleSidedDrawCommands{};
  std::vector<DrawCommand> opaqueWindingFlippedDrawCommands{};
  std::vector<DrawCommand> opaqueDoubleSidedDrawCommands{};
  std::vector<DrawCommand> transparentDrawCommands{};
  std::vector<DrawCommand> transparentSingleSidedDrawCommands{};
  std::vector<DrawCommand> transparentWindingFlippedDrawCommands{};
  std::vector<DrawCommand> transparentDoubleSidedDrawCommands{};
  BimGeometryDrawLists points{};
  BimGeometryDrawLists curves{};
  BimGeometryDrawLists nativePoints{};
  BimGeometryDrawLists nativeCurves{};

  void clear() {
    opaqueDrawCommands.clear();
    opaqueSingleSidedDrawCommands.clear();
    opaqueWindingFlippedDrawCommands.clear();
    opaqueDoubleSidedDrawCommands.clear();
    transparentDrawCommands.clear();
    transparentSingleSidedDrawCommands.clear();
    transparentWindingFlippedDrawCommands.clear();
    transparentDoubleSidedDrawCommands.clear();
    points.clear();
    curves.clear();
    nativePoints.clear();
    nativeCurves.clear();
  }
};

// Owns sidecar model draw data independently from the regular scene graph.
// Supports dotbim, tessellated IFC, IFCX, USD/USDC/USDZ meshes, and
// glTF fallback content routed through the same BIM render passes.
class BimManager {
public:
  BimManager(std::shared_ptr<container::gpu::VulkanDevice> device,
             container::gpu::AllocationManager &allocationManager,
             container::gpu::PipelineManager &pipelineManager);
  ~BimManager();

  BimManager(const BimManager &) = delete;
  BimManager &operator=(const BimManager &) = delete;

  void loadModel(const std::string &path, float importScale,
                 container::scene::SceneManager &sceneManager);
  void clear();
  void createMeshletResidencyResources(const std::filesystem::path &shaderDir);
  void
  updateMeshletResidencySettings(const BimMeshletResidencySettings &settings);
  void updateVisibilityFilterSettings(const BimDrawFilter &filter);
  void prepareDrawCompaction(BimDrawCompactionSlot slot,
                             const std::vector<DrawCommand> &commands);
  void prepareDrawCompaction(const std::vector<DrawCommand> &commands);
  void recordMeshletResidencyUpdate(VkCommandBuffer cmd, VkBuffer cameraBuffer,
                                    VkDeviceSize cameraBufferSize,
                                    VkBuffer objectBuffer,
                                    VkDeviceSize objectBufferSize);
  void recordVisibilityFilterUpdate(VkCommandBuffer cmd);
  void recordDrawCompactionUpdate(VkCommandBuffer cmd);
  void drawCompacted(BimDrawCompactionSlot slot, VkCommandBuffer cmd) const;
  void drawCompactedOpaqueSingleSided(VkCommandBuffer cmd) const;

  [[nodiscard]] bool hasScene() const;
  [[nodiscard]] const std::string &modelPath() const { return modelPath_; }

  [[nodiscard]] container::gpu::BufferSlice vertexSlice() const {
    return vertexSlice_;
  }
  [[nodiscard]] container::gpu::BufferSlice indexSlice() const {
    return indexSlice_;
  }
  [[nodiscard]] VkIndexType indexType() const { return VK_INDEX_TYPE_UINT32; }

  [[nodiscard]] const std::vector<container::gpu::ObjectData> &
  objectData() const {
    return objectData_;
  }
  [[nodiscard]] const std::vector<BimElementMetadata> &elementMetadata() const {
    return elementMetadata_;
  }
  [[nodiscard]] BimCoordinationOverlayResult buildCoordinationOverlay(
      const BimCoordinationOverlayBuildOptions &options,
      std::span<const BimCoordinationOverlayClashPair> clashPairs = {},
      std::span<const BimCoordinationOverlayIssuePin> issuePins = {}) const;
  bool rebuildCoordinationMarkerGeometry(
      const BimCoordinationOverlayResult &overlay, float markerRadius = 0.35f);
  void clearCoordinationMarkerGeometry();
  [[nodiscard]] const BimCoordinationMarkerDrawData &
  coordinationMarkerDrawData() const {
    return coordinationMarkerDrawData_;
  }
  bool rebuildSectionPlaneVisualGeometry(glm::vec4 plane, float size,
                                         glm::vec3 color);
  void clearSectionPlaneVisualGeometry();
  [[nodiscard]] const BimSectionPlaneVisualDrawData &
  sectionPlaneVisualDrawData() const {
    return sectionPlaneVisualDrawData_;
  }
  [[nodiscard]] const BimRelationshipGraph &relationshipGraph() const {
    return relationshipGraph_;
  }
  [[nodiscard]] const BimElementMetadata *
  metadataForObject(uint32_t objectIndex) const;
  [[nodiscard]] std::span<const uint32_t>
  objectIndicesForGuid(std::string_view guid) const;
  [[nodiscard]] std::span<const uint32_t>
  objectIndicesForSourceId(std::string_view sourceId) const;
  [[nodiscard]] BimElementBounds
  elementBoundsForObject(uint32_t objectIndex) const;
  [[nodiscard]] const std::vector<std::string> &elementTypes() const;
  [[nodiscard]] const std::vector<std::string> &elementStoreys() const;
  [[nodiscard]] const std::vector<std::string> &elementMaterials() const;
  [[nodiscard]] const std::vector<std::string> &elementDisciplines() const;
  [[nodiscard]] const std::vector<std::string> &elementPhases() const;
  [[nodiscard]] const std::vector<std::string> &elementFireRatings() const;
  [[nodiscard]] const std::vector<std::string> &
  elementLoadBearingValues() const;
  [[nodiscard]] const std::vector<std::string> &elementStatuses() const;
  [[nodiscard]] const std::vector<BimStoreyRange> &elementStoreyRanges() const;
  [[nodiscard]] const BimModelUnitMetadata &modelUnitMetadata() const;
  [[nodiscard]] const BimModelGeoreferenceMetadata &
  modelGeoreferenceMetadata() const;
  [[nodiscard]] BimSemanticColorMode semanticColorMode() const {
    return semanticColorMode_;
  }
  bool setSemanticColorMode(BimSemanticColorMode mode);
  [[nodiscard]] BimSceneStats sceneStats() const;
  [[nodiscard]] std::vector<container::scene::SceneProviderTriangleBatch>
  sceneProviderTriangleBatches() const;
  [[nodiscard]] const BimOptimizedModelMetadata &
  optimizedModelMetadata() const {
    return optimizedModelMetadata_;
  }
  [[nodiscard]] std::span<const BimMeshletClusterMetadata>
  meshletClusters() const {
    return meshletClusters_;
  }
  [[nodiscard]] std::span<const BimObjectLodStreamingMetadata>
  objectLodStreamingMetadata() const {
    return objectLodMetadata_;
  }
  [[nodiscard]] const BimMeshletResidencyStats &meshletResidencyStats() const {
    return meshletResidencyStats_;
  }
  [[nodiscard]] const container::gpu::AllocatedBuffer &
  meshletClusterBuffer() const {
    return meshletClusterBuffer_;
  }
  [[nodiscard]] const container::gpu::AllocatedBuffer &
  meshletObjectLodBuffer() const {
    return meshletObjectLodBuffer_;
  }
  [[nodiscard]] const container::gpu::AllocatedBuffer &
  meshletResidencyBuffer() const {
    return meshletResidencyBuffer_;
  }
  [[nodiscard]] const container::gpu::AllocatedBuffer &
  visibilityMaskBuffer() const {
    return visibilityMaskBuffer_;
  }
  [[nodiscard]] const BimVisibilityFilterStats &visibilityFilterStats() const {
    return visibilityFilterStats_;
  }
  [[nodiscard]] bool visibilityMaskReadyForDrawCompaction() const;
  [[nodiscard]] const BimDrawCompactionStats &drawCompactionStats() const {
    return drawCompactionSlots_[static_cast<size_t>(
                                    BimDrawCompactionSlot::OpaqueSingleSided)]
        .stats;
  }
  [[nodiscard]] const BimDrawCompactionStats &
  drawCompactionStats(BimDrawCompactionSlot slot) const;
  [[nodiscard]] bool drawCompactionReady() const {
    return drawCompactionReady(BimDrawCompactionSlot::OpaqueSingleSided);
  }
  [[nodiscard]] bool drawCompactionReady(BimDrawCompactionSlot slot) const;
  [[nodiscard]] BimDrawBudgetLodStats
  drawBudgetLodStats(const BimDrawFilter &filter) const;
  [[nodiscard]] bool objectMatchesFilter(uint32_t objectIndex,
                                         const BimDrawFilter &filter) const;
  [[nodiscard]] const BimDrawLists &
  filteredDrawLists(const BimDrawFilter &filter);
  [[nodiscard]] uint64_t objectDataRevision() const {
    return objectDataRevision_;
  }
  [[nodiscard]] VkBuffer objectBuffer() const { return objectBuffer_.buffer; }
  [[nodiscard]] VkDeviceSize objectBufferSize() const;

  [[nodiscard]] const container::gpu::AllocatedBuffer &
  objectAllocatedBuffer() const {
    return objectBuffer_;
  }

  [[nodiscard]] const std::vector<DrawCommand> &opaqueDrawCommands() const {
    return opaqueDrawCommands_;
  }
  [[nodiscard]] const std::vector<DrawCommand> &
  opaqueSingleSidedDrawCommands() const {
    return opaqueSingleSidedDrawCommands_;
  }
  [[nodiscard]] const std::vector<DrawCommand> &
  opaqueWindingFlippedDrawCommands() const {
    return opaqueWindingFlippedDrawCommands_;
  }
  [[nodiscard]] const std::vector<DrawCommand> &
  opaqueDoubleSidedDrawCommands() const {
    return opaqueDoubleSidedDrawCommands_;
  }
  [[nodiscard]] const std::vector<DrawCommand> &
  transparentDrawCommands() const {
    return transparentDrawCommands_;
  }
  [[nodiscard]] const std::vector<DrawCommand> &
  transparentSingleSidedDrawCommands() const {
    return transparentSingleSidedDrawCommands_;
  }
  [[nodiscard]] const std::vector<DrawCommand> &
  transparentWindingFlippedDrawCommands() const {
    return transparentWindingFlippedDrawCommands_;
  }
  [[nodiscard]] const std::vector<DrawCommand> &
  transparentDoubleSidedDrawCommands() const {
    return transparentDoubleSidedDrawCommands_;
  }
  [[nodiscard]] const BimGeometryDrawLists &pointDrawLists() const {
    return pointDrawLists_;
  }
  [[nodiscard]] const BimGeometryDrawLists &curveDrawLists() const {
    return curveDrawLists_;
  }
  [[nodiscard]] const BimGeometryDrawLists &nativePointDrawLists() const {
    return nativePointDrawLists_;
  }
  [[nodiscard]] const BimGeometryDrawLists &nativeCurveDrawLists() const {
    return nativeCurveDrawLists_;
  }
  [[nodiscard]] const std::vector<DrawCommand> &
  floorPlanGroundDrawCommands() const {
    return floorPlanGround_.drawCommands;
  }
  [[nodiscard]] const std::vector<DrawCommand> &
  floorPlanSourceElevationDrawCommands() const {
    return floorPlanSourceElevation_.drawCommands;
  }
  [[nodiscard]] const std::vector<DrawCommand> &floorPlanDrawCommands() const {
    return floorPlanGroundDrawCommands();
  }
  [[nodiscard]] std::vector<BimDrawingExportLine>
  floorPlanDrawingExportLines(bool sourceElevation, glm::vec3 color,
                              float lineWidthMm) const;
  [[nodiscard]] bool hasFloorPlanOverlay() const {
    return floorPlanGround_.valid() || floorPlanSourceElevation_.valid();
  }
  bool rebuildSectionClipCapGeometry(const BimSectionCapBuildOptions &options);
  void clearSectionClipCapGeometry();
  [[nodiscard]] const BimSectionClipCapDrawData &
  sectionClipCapDrawData() const {
    return sectionClipCapDrawData_;
  }
  [[nodiscard]] BimPickHit
  pickRenderableObject(const container::gpu::CameraData &cameraData,
                       VkExtent2D viewportExtent, double cursorX,
                       double cursorY, bool sectionPlaneEnabled = false,
                       glm::vec4 sectionPlane = {0.0f, 1.0f, 0.0f, 0.0f}) const;
  [[nodiscard]] BimPickHit pickTransparentRenderableObject(
      const container::gpu::CameraData &cameraData, VkExtent2D viewportExtent,
      double cursorX, double cursorY, bool sectionPlaneEnabled = false,
      glm::vec4 sectionPlane = {0.0f, 1.0f, 0.0f, 0.0f}) const;
  void
  collectDrawCommandsForObject(uint32_t objectIndex,
                               std::vector<DrawCommand> &outCommands) const;
  void collectNativePointDrawCommandsForObject(
      uint32_t objectIndex, std::vector<DrawCommand> &outCommands) const;
  void collectNativeCurveDrawCommandsForObject(
      uint32_t objectIndex, std::vector<DrawCommand> &outCommands) const;

private:
  struct MeshRange {
    uint32_t meshId{0};
    uint32_t firstIndex{0};
    uint32_t indexCount{0};
    glm::vec3 boundsCenter{0.0f};
    float boundsRadius{0.0f};
  };

  [[nodiscard]] std::filesystem::path
  resolveModelPath(const std::string &path) const;

  void loadDotBim(const std::filesystem::path &path, float importScale,
                  container::scene::SceneManager &sceneManager);
  void loadIfc(const std::filesystem::path &path, float importScale,
               container::scene::SceneManager &sceneManager);
  void loadGltfFallback(const std::filesystem::path &path, float importScale,
                        container::scene::SceneManager &sceneManager);
  void loadPreparedModel(const container::geometry::dotbim::Model &model,
                         const std::filesystem::path &path,
                         std::string_view format,
                         container::scene::SceneManager &sceneManager);
  void buildDrawDataFromModel(const container::geometry::dotbim::Model &model,
                              container::scene::SceneManager &sceneManager);
  [[nodiscard]] BimDrawFilterStateInputs drawFilterStateInputs() const;
  [[nodiscard]] BimPickHit pickRenderableObjectForDraws(
      const container::gpu::CameraData &cameraData, VkExtent2D viewportExtent,
      double cursorX, double cursorY, bool includeOpaque,
      bool includeTransparent, bool sectionPlaneEnabled,
      glm::vec4 sectionPlane) const;
  void uploadGeometry(std::span<const container::geometry::Vertex> vertices,
                      std::span<const uint32_t> indices);
  void uploadObjects();
  void uploadMeshletResidencyBuffers();
  void uploadVisibilityFilterBuffers();
  void destroyMeshletResidencyBuffers();
  void destroyVisibilityFilterBuffers();
  void
  writeMeshletResidencyDescriptorSet(VkBuffer cameraBuffer = VK_NULL_HANDLE,
                                     VkDeviceSize cameraBufferSize = 0,
                                     VkBuffer objectBuffer = VK_NULL_HANDLE,
                                     VkDeviceSize objectBufferSize = 0);
  void destroyMeshletResidencyComputeResources();
  void createVisibilityFilterResources(const std::filesystem::path &shaderDir);
  void destroyVisibilityFilterComputeResources();
  void writeVisibilityFilterDescriptorSet();
  void createDrawCompactionResources(const std::filesystem::path &shaderDir);
  void destroyDrawCompactionBuffers();
  void destroyDrawCompactionComputeResources();
  void destroyDrawCompactionBuffers(BimDrawCompactionSlotResources &slot);
  [[nodiscard]] bool drawCompactionSupported() const;
  void invalidateDrawCompactionOutputs(bool requestDispatch);
  void ensureDrawCompactionCapacity(BimDrawCompactionSlot slot,
                                    size_t inputDrawCount,
                                    size_t outputCapacity);
  void writeDrawCompactionDescriptorSet(BimDrawCompactionSlot slot);
  void collectNativeDrawCommandsForObject(
      uint32_t objectIndex, BimGeometryKind geometryKind,
      const BimGeometryDrawLists &sourceLists,
      std::vector<DrawCommand> &outCommands) const;

  std::shared_ptr<container::gpu::VulkanDevice> device_;
  container::gpu::AllocationManager &allocationManager_;
  container::gpu::PipelineManager &pipelineManager_;
  std::string modelPath_{};

  container::gpu::BufferSlice vertexSlice_{};
  container::gpu::BufferSlice indexSlice_{};
  container::gpu::AllocatedBuffer vertexBuffer_{};
  container::gpu::AllocatedBuffer indexBuffer_{};
  container::gpu::AllocatedBuffer objectBuffer_{};
  size_t objectBufferCapacity_{0};
  uint64_t objectDataRevision_{0};
  std::unique_ptr<BimMetadataCatalog> metadataCatalog_{};
  std::unique_ptr<BimMetadataIndex> metadataIndex_{};
  std::unique_ptr<BimDrawFilterState> drawFilterState_{};

  std::vector<container::geometry::Vertex> vertices_{};
  std::vector<uint32_t> indices_{};
  std::vector<container::gpu::ObjectData> objectData_{};
  std::vector<BimElementMetadata> elementMetadata_{};
  BimRelationshipGraph relationshipGraph_{};
  std::vector<DrawCommand> objectDrawCommands_{};
  std::vector<uint32_t> objectDrawCommandOffsets_{};
  std::vector<uint32_t> objectDrawCommandCounts_{};
  BimSemanticColorMode semanticColorMode_{BimSemanticColorMode::Off};
  bool semanticColorIdsDirty_{true};
  std::vector<DrawCommand> opaqueDrawCommands_{};
  std::vector<DrawCommand> opaqueSingleSidedDrawCommands_{};
  std::vector<DrawCommand> opaqueWindingFlippedDrawCommands_{};
  std::vector<DrawCommand> opaqueDoubleSidedDrawCommands_{};
  std::vector<DrawCommand> transparentDrawCommands_{};
  std::vector<DrawCommand> transparentSingleSidedDrawCommands_{};
  std::vector<DrawCommand> transparentWindingFlippedDrawCommands_{};
  std::vector<DrawCommand> transparentDoubleSidedDrawCommands_{};
  BimGeometryDrawLists pointDrawLists_{};
  BimGeometryDrawLists curveDrawLists_{};
  BimGeometryDrawLists nativePointDrawLists_{};
  BimGeometryDrawLists nativeCurveDrawLists_{};
  size_t meshletClusterCount_{0};
  std::vector<BimMeshletClusterMetadata> meshletClusters_{};
  std::vector<BimObjectLodStreamingMetadata> objectLodMetadata_{};
  BimOptimizedModelMetadata optimizedModelMetadata_{};
  container::gpu::AllocatedBuffer meshletClusterBuffer_{};
  container::gpu::AllocatedBuffer meshletObjectLodBuffer_{};
  container::gpu::AllocatedBuffer meshletResidencyBuffer_{};
  BimMeshletResidencyStats meshletResidencyStats_{};
  VkDescriptorSetLayout meshletResidencySetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool meshletResidencyDescriptorPool_{VK_NULL_HANDLE};
  VkDescriptorSet meshletResidencyDescriptorSet_{VK_NULL_HANDLE};
  VkPipelineLayout meshletResidencyPipelineLayout_{VK_NULL_HANDLE};
  VkPipeline meshletResidencyPipeline_{VK_NULL_HANDLE};
  BimMeshletResidencySettings meshletResidencySettings_{};
  VkBuffer meshletResidencyCameraBuffer_{VK_NULL_HANDLE};
  VkDeviceSize meshletResidencyCameraBufferSize_{0};
  VkBuffer meshletResidencyObjectBuffer_{VK_NULL_HANDLE};
  VkDeviceSize meshletResidencyObjectBufferSize_{0};
  bool meshletResidencyDescriptorsDirty_{false};
  bool meshletResidencyDispatchPending_{false};
  std::vector<BimVisibilityGpuObjectMetadata> visibilityFilterMetadata_{};
  container::gpu::AllocatedBuffer visibilityFilterMetadataBuffer_{};
  container::gpu::AllocatedBuffer visibilityMaskBuffer_{};
  BimVisibilityFilterPushConstants visibilityFilterSettings_{};
  BimVisibilityFilterStats visibilityFilterStats_{};
  VkDescriptorSetLayout visibilityFilterSetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool visibilityFilterDescriptorPool_{VK_NULL_HANDLE};
  VkDescriptorSet visibilityFilterDescriptorSet_{VK_NULL_HANDLE};
  VkPipelineLayout visibilityFilterPipelineLayout_{VK_NULL_HANDLE};
  VkPipeline visibilityFilterPipeline_{VK_NULL_HANDLE};
  bool visibilityFilterDescriptorsDirty_{false};
  bool visibilityFilterDispatchPending_{false};
  bool visibilityFilterMaskCurrent_{false};
  bool visibilityFilterMaskAllVisible_{false};
  std::array<BimDrawCompactionSlotResources, kBimDrawCompactionSlotCount>
      drawCompactionSlots_{};
  VkDescriptorSetLayout drawCompactionSetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool drawCompactionDescriptorPool_{VK_NULL_HANDLE};
  std::array<VkDescriptorSet, kBimDrawCompactionSlotCount>
      drawCompactionDescriptorSets_{};
  VkPipelineLayout drawCompactionPipelineLayout_{VK_NULL_HANDLE};
  VkPipeline drawCompactionPipeline_{VK_NULL_HANDLE};
  BimFloorPlanOverlayData floorPlanGround_{};
  BimFloorPlanOverlayData floorPlanSourceElevation_{};
  BimSectionClipCapDrawData sectionClipCapDrawData_{};
  BimCoordinationMarkerDrawData coordinationMarkerDrawData_{};
  BimSectionPlaneVisualDrawData sectionPlaneVisualDrawData_{};
  std::vector<container::geometry::Vertex> sectionClipCapVertices_{};
  std::vector<uint32_t> sectionClipCapIndices_{};
  container::gpu::AllocatedBuffer sectionClipCapVertexBuffer_{};
  container::gpu::AllocatedBuffer sectionClipCapIndexBuffer_{};
  BimSectionCapBuildOptions sectionClipCapBuildOptions_{};
  bool sectionClipCapBuildOptionsValid_{false};
  std::vector<container::geometry::Vertex> coordinationMarkerVertices_{};
  std::vector<uint32_t> coordinationMarkerIndices_{};
  container::gpu::AllocatedBuffer coordinationMarkerVertexBuffer_{};
  container::gpu::AllocatedBuffer coordinationMarkerIndexBuffer_{};
  size_t coordinationMarkerGeometrySignature_{0};
  std::vector<container::geometry::Vertex> sectionPlaneVisualVertices_{};
  std::vector<uint32_t> sectionPlaneVisualIndices_{};
  container::gpu::AllocatedBuffer sectionPlaneVisualVertexBuffer_{};
  container::gpu::AllocatedBuffer sectionPlaneVisualIndexBuffer_{};
  size_t sectionPlaneVisualGeometrySignature_{0};
};

} // namespace container::renderer
