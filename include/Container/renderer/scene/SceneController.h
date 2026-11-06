#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/common/CommonMath.h"
#include "Container/renderer/debug/DebugOverlayRenderer.h"
#include "Container/renderer/scene/GltfDeformationScene.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace container::app {
struct AppConfig;
}  // namespace container::app

namespace container::ecs {
class World;
}  // namespace container::ecs

namespace container::gpu {
class AllocationManager;
class PipelineManager;
class VulkanDevice;
}  // namespace container::gpu

namespace container::geometry {
struct GltfLoadResult;
}  // namespace container::geometry

namespace container::scene {
class SceneGraph;
class SceneManager;
struct MaterialRenderProperties;
class PreparedMaterialBindingChange;
}  // namespace container::scene

namespace container::ui {
class GuiManager;
}  // namespace container::ui

namespace container::renderer {

enum class ScenePrimitiveKind : uint32_t;

struct SceneNodePickHit {
  uint32_t nodeIndex{std::numeric_limits<uint32_t>::max()};
  float distance{std::numeric_limits<float>::max()};
  float depth{0.0f};
  glm::vec3 worldPosition{0.0f};
  bool hasWorldPosition{false};
  bool hit{false};
};

struct SceneNodeWorldBounds {
  bool valid{false};
  glm::vec3 min{0.0f};
  glm::vec3 max{0.0f};
  glm::vec3 center{0.0f};
  glm::vec3 size{0.0f};
  float radius{0.0f};
};

// Manages scene geometry upload, object buffer, scene-graph build/sync,
// and model reload. Lives for the lifetime of the application.
class SceneController {
 public:
  SceneController(
      std::shared_ptr<container::gpu::VulkanDevice>  device,
      container::gpu::AllocationManager&             allocationManager,
      container::gpu::PipelineManager&             pipelineManager,
      container::scene::SceneGraph&                     sceneGraph,
      container::scene::SceneManager&                   sceneManager,
      container::ui::GuiManager*                        guiManager,
      const container::app::AppConfig&                           config);

  ~SceneController();
  SceneController(const SceneController&) = delete;
  SceneController& operator=(const SceneController&) = delete;

  class PreparedGeneration {
   public:
    PreparedGeneration() noexcept;
    ~PreparedGeneration();
    PreparedGeneration(PreparedGeneration&&) noexcept;
    PreparedGeneration& operator=(PreparedGeneration&&) noexcept;
    PreparedGeneration(const PreparedGeneration&) = delete;
    PreparedGeneration& operator=(const PreparedGeneration&) = delete;
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] uint64_t expectedGraphRevision() const noexcept;
    [[nodiscard]] uint64_t expectedSceneEpoch() const noexcept;
    [[nodiscard]] container::gpu::BufferSlice vertexSlice() const noexcept;
    [[nodiscard]] container::gpu::BufferSlice indexSlice() const noexcept;
    [[nodiscard]] container::gpu::BufferSlice diagCubeVertexSlice() const noexcept;
    [[nodiscard]] container::gpu::BufferSlice diagCubeIndexSlice() const noexcept;
    [[nodiscard]] uint32_t diagCubeIndexCount() const noexcept;
    [[nodiscard]] std::span<const container::gpu::AllocatedBuffer> objectBuffers() const noexcept;
    [[nodiscard]] std::span<const size_t> objectBufferCapacities() const noexcept;
    [[nodiscard]] const GltfDeformationScene* deformationScene() const noexcept;
    [[nodiscard]] std::span<const container::gpu::ObjectData> objectData() const noexcept;
    [[nodiscard]] std::span<const uint32_t> objectNodeIndices() const noexcept;
    [[nodiscard]] std::span<const DrawCommand> opaqueDrawCommands() const noexcept;
    [[nodiscard]] std::span<const DrawCommand> opaqueSingleSidedDrawCommands() const noexcept;
    [[nodiscard]] std::span<const DrawCommand> opaqueWindingFlippedDrawCommands() const noexcept;
    [[nodiscard]] std::span<const DrawCommand> opaqueDoubleSidedDrawCommands() const noexcept;
    [[nodiscard]] std::span<const DrawCommand> transparentDrawCommands() const noexcept;
    [[nodiscard]] std::span<const container::geometry::Vertex> geometryVertices() const noexcept;
    [[nodiscard]] std::span<const uint32_t> geometryIndices() const noexcept;
    [[nodiscard]] std::span<const container::geometry::Vertex> gpuBindVertices() const noexcept;
    // Prospective live keys, valid while canExchangeGeneration remains true.
    [[nodiscard]] uint64_t geometryRevision() const noexcept;
    [[nodiscard]] uint64_t objectDataRevision() const noexcept;
    [[nodiscard]] uint64_t ownedBytes() const noexcept;
    [[nodiscard]] container::ecs::World& world();
    [[nodiscard]] const container::ecs::World& world() const;
   private:
    friend class SceneController;
    struct State;
    std::unique_ptr<State> state_;
  };

  // All allocations, draw extraction and uploads use detached scene state.
  // Buffer views are aliases owned by the returned generation. Its allocator
  // must outlive it. Retire graphics/GI readers before destroying a published
  // generation (including history eviction), and clear histories before
  // AllocationManager cleanup. Private upload work completes during prepare.
  [[nodiscard]] PreparedGeneration prepareGeneration(
      container::scene::SceneGraph& detachedGraph,
      container::scene::SceneManager& detachedScene, uint32_t imageCount,
      bool showDiagCube, uint64_t expectedGraphRevision,
      uint64_t expectedSceneEpoch) const;
  struct GraphPoseGenerationLimits {
    uint64_t maxNodes{262144},maxEdges{524288},maxObjects{1'000'000};
    uint64_t maxVertices{16'000'000},maxIndices{64'000'000};
    uint32_t maxImages{64};
    uint64_t maxCpuBytes{512ull*1024*1024},maxGpuBytes{512ull*1024*1024},maxWork{512ull*1024*1024};
  };
  // Transforms only: the candidate must preserve exact node lifetimes,
  // topology, source/mesh/material bindings, flags and active roots. The live
  // manager is borrowed read-only; no model or opaque World context is copied.
  // All graph/derived geometry/World allocation is bounded before staging.
  [[nodiscard]] PreparedGeneration prepareGraphPoseGeneration(
      const container::scene::SceneGraph& candidateGraph,uint32_t imageCount,
      bool showDiagCube,uint64_t expectedGraphRevision,uint64_t expectedSceneEpoch,
      const GraphPoseGenerationLimits&) const;
  [[nodiscard]] PreparedGeneration prepareGraphPoseGeneration(
      const container::scene::SceneGraph& candidateGraph,uint32_t imageCount,
      bool showDiagCube,uint64_t expectedGraphRevision,uint64_t expectedSceneEpoch) const;
  [[nodiscard]] bool canExchangeGeneration(const PreparedGeneration& prepared,
                                           uint64_t currentSceneEpoch) const noexcept;
  // Caller validates the complete transaction and retires GPU readers first.
  // Exchanges state without replacing the controller/World or allocating.
  // Old resources remain owned by prepared, permitting a later undo exchange.
  void exchangeGeneration(PreparedGeneration& prepared,
                          uint64_t publishedGraphRevision,
                          uint64_t publishedSceneGeometryRevision,
                          uint64_t publishedSceneEpoch) noexcept;
  [[nodiscard]] std::span<const container::gpu::AllocatedBuffer> objectBuffers() const noexcept;
  [[nodiscard]] std::span<const size_t> objectBufferCapacities() const noexcept;
  [[nodiscard]] bool ownsObjectBuffer(VkBuffer buffer) const noexcept;

  struct MaterialRoutingLimits {
    uint64_t maxMaterials{1'000'000}, maxObjects{1'000'000};
    uint32_t maxImages{64};
    uint64_t maxCpuBytes{128ull*1024*1024}, maxGpuBytes{256ull*1024*1024};
  };
  class PreparedMaterialRouting {
   public:
    PreparedMaterialRouting() noexcept;
    ~PreparedMaterialRouting();
    PreparedMaterialRouting(PreparedMaterialRouting&&) noexcept;
    PreparedMaterialRouting& operator=(PreparedMaterialRouting&&) noexcept;
    PreparedMaterialRouting(const PreparedMaterialRouting&)=delete;
    PreparedMaterialRouting& operator=(const PreparedMaterialRouting&)=delete;
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::span<const container::gpu::AllocatedBuffer> objectBuffers() const noexcept;
    [[nodiscard]] std::span<const size_t> objectBufferCapacities() const noexcept;
    [[nodiscard]] std::span<const container::gpu::ObjectData> objectData() const noexcept;
    [[nodiscard]] std::span<const uint32_t> objectNodeIndices() const noexcept;
    [[nodiscard]] std::span<const DrawCommand> opaqueDrawCommands() const noexcept;
    [[nodiscard]] std::span<const DrawCommand> transparentDrawCommands() const noexcept;
    [[nodiscard]] std::span<const DrawCommand> opaqueSingleSidedDrawCommands() const noexcept;
    [[nodiscard]] std::span<const DrawCommand> opaqueWindingFlippedDrawCommands() const noexcept;
    [[nodiscard]] std::span<const DrawCommand> opaqueDoubleSidedDrawCommands() const noexcept;
    [[nodiscard]] std::span<const DrawCommand> transparentSingleSidedDrawCommands() const noexcept;
    [[nodiscard]] std::span<const DrawCommand> transparentWindingFlippedDrawCommands() const noexcept;
    [[nodiscard]] std::span<const DrawCommand> transparentDoubleSidedDrawCommands() const noexcept;
    [[nodiscard]] uint64_t objectDataRevision() const noexcept;
    [[nodiscard]] uint64_t expectedGraphRevision() const noexcept;
    [[nodiscard]] uint64_t candidateGraphRevision() const noexcept;
   private:
    friend class SceneController;
    struct State;
    std::unique_ptr<State> state_;
  };
  // The live object cache must already match the graph/geometry/material keys.
  // Only object buffers/tables are owned here; factor-only routing does not
  // publish geometry, deformation or ECS state. Caller retires readers before
  // exchange/destruction, as for PreparedGeneration.
  // Read-only authority check: never updates deformation, World entities,
  // graph transforms, object tables, buffers or revisions.
  [[nodiscard]] bool materialRoutingCacheCurrent(bool showDiagCube) const noexcept;
  [[nodiscard]] PreparedMaterialRouting prepareMaterialRouting(
      std::span<const container::scene::MaterialRenderProperties> materials,
      uint32_t imageCount,bool showDiagCube,uint64_t expectedSceneEpoch,
      const MaterialRoutingLimits& limits) const;
  [[nodiscard]] PreparedMaterialRouting prepareMaterialRouting(
      std::span<const container::scene::MaterialRenderProperties> materials,
      uint32_t imageCount,bool showDiagCube,uint64_t expectedSceneEpoch) const;
  // Uses an owning immutable binding receipt, including an exact prospective
  // material append/removal context. Inactive changed bindings need not have an
  // object row. Preparation leaves graph, ECS and live buffers unchanged.
  // Final exchange follows graph + manager publication and is one-shot; it
  // updates existing ECS material components without recreating entities.
  [[nodiscard]] PreparedMaterialRouting prepareMaterialRouting(
      std::span<const container::scene::MaterialRenderProperties> materials,
      const container::scene::PreparedMaterialBindingChange& bindings,
      uint32_t imageCount,bool showDiagCube,uint64_t expectedSceneEpoch,
      const MaterialRoutingLimits& limits) const;
  [[nodiscard]] PreparedMaterialRouting prepareMaterialRouting(
      std::span<const container::scene::MaterialRenderProperties> materials,
      const container::scene::PreparedMaterialBindingChange& bindings,
      uint32_t imageCount,bool showDiagCube,uint64_t expectedSceneEpoch) const;
  [[nodiscard]] bool canExchangeMaterialRouting(const PreparedMaterialRouting&,uint64_t currentSceneEpoch) const noexcept;
  void exchangeMaterialRouting(PreparedMaterialRouting&,uint64_t publishedMaterialRevision,
                               uint64_t publishedSceneEpoch) noexcept;
  // Caller proves material routing/height facts unchanged. Only acknowledges
  // the actually published manager revision on an otherwise current cache.
  void acknowledgeMaterialRevision(uint64_t publishedMaterialRevision) noexcept;

  // ---- Geometry buffers ---------------------------------------------------

  // Uploads scene + diagnostic-cube vertices/indices into a shared GPU arena.
  void createGeometryBuffers();

  // CPU reconciliation only. Topology changes remain pending until
  // createGeometryBuffers publishes matching bind/index/diagnostic streams.
  // Caller retires GPU readers before uploading a changed layout.
  GltfDeformationSceneUpdate updateDeformation(bool resetHistory = false);
  [[nodiscard]] const GltfDeformationScene* deformationScene() const {
    return deformationScene_.get();
  }
  [[nodiscard]] std::span<const container::geometry::Vertex> geometryVertices() const;
  [[nodiscard]] std::span<const uint32_t> geometryIndices() const;
  [[nodiscard]] uint64_t geometryRevision() const { return geometryRevision_; }
  // Complete GPU bind stream, including the diagnostic tail. Geometry accessors
  // above expose the evaluated scene only, for CPU rays, bounds and picking.
  [[nodiscard]] std::span<const container::geometry::Vertex> gpuBindVertices() const {
    return gpuBindVertices_;
  }
  void commitDeformationFrame();

  // Creates cameraBuffer + objectBuffer if not already present, uploads data.
  void createSceneBuffers(
      const container::gpu::AllocatedBuffer& cameraBuffer,
      container::gpu::AllocatedBuffer&       objectBuffer,
      size_t&                                 objectBufferCapacity);

  // ---- Per-frame updates --------------------------------------------------

  // Rebuilds objectData, opaqueDrawCommands, transparentDrawCommands from
  // the scene graph. Appends the diagnostic cube entry when enabled.
  void syncObjectDataFromSceneGraph(bool showDiagCube);

  // Calls sync then uploads objectData to objectBuffer; recreates the buffer
  // if needed. Returns true when the buffer was recreated (caller must
  // re-update descriptor sets). Deferred GPU publication suppresses CPU writes
  // and invalidates the CPU upload receipt; the caller must publish a complete
  // compute record or restore the CPU table before rendering.
  bool updateObjectBuffer(
      container::gpu::AllocatedBuffer&       objectBuffer,
      size_t&                                 objectBufferCapacity,
      const container::gpu::AllocatedBuffer& cameraBuffer,
      bool deferUploadToGpu = false);
  [[nodiscard]] uint64_t lastObjectUploadBytes() const noexcept { return lastObjectUploadBytes_; }

  // ---- Scene graph --------------------------------------------------------

  void buildSceneGraph(uint32_t& outRootNode,
                       uint32_t& outSelectedMeshNode,
                       uint32_t& outCubeNode);

  void addSceneObject(const glm::mat4& transform,
                      uint32_t         rootNode,
                      container::gpu::AllocatedBuffer& objectBuffer,
                      size_t& objectBufferCapacity,
                      const container::gpu::AllocatedBuffer& cameraBuffer);
  [[nodiscard]] uint32_t addScenePrimitive(ScenePrimitiveKind kind,
                                           const glm::mat4& transform,
                                           uint32_t& rootNode);
  // Append retained runtime geometry without publishing a graph instance.
  // The editor stages final hierarchy/membership and calls createGeometryBuffers
  // after graph publication. Existing direct creation uses this same routine.
  [[nodiscard]] uint32_t appendScenePrimitiveGeometry(ScenePrimitiveKind kind);

  bool reloadSceneModel(
      const std::string&                      path,
      float                                   importScale,
      container::gpu::AllocatedBuffer&       objectBuffer,
      size_t&                                 objectBufferCapacity,
      const container::gpu::AllocatedBuffer& cameraBuffer,
      VkIndexType&                            outIndexType,
      uint32_t&                               outRootNode,
      uint32_t&                               outSelectedMeshNode,
      uint32_t&                               outCubeNode,
      container::geometry::GltfLoadResult*      prepared = nullptr);

  [[nodiscard]] uint32_t pickRenderableNode(
      const container::gpu::CameraData& cameraData,
      VkExtent2D viewportExtent,
      double cursorX,
      double cursorY,
      bool sectionPlaneEnabled = false,
      glm::vec4 sectionPlane = {0.0f, 1.0f, 0.0f, 0.0f}) const;
  [[nodiscard]] SceneNodePickHit pickRenderableNodeHit(
      const container::gpu::CameraData& cameraData,
      VkExtent2D viewportExtent,
      double cursorX,
      double cursorY,
      bool sectionPlaneEnabled = false,
      glm::vec4 sectionPlane = {0.0f, 1.0f, 0.0f, 0.0f}) const;
  [[nodiscard]] SceneNodePickHit pickTransparentRenderableNodeHit(
      const container::gpu::CameraData& cameraData,
      VkExtent2D viewportExtent,
      double cursorX,
      double cursorY,
      bool sectionPlaneEnabled = false,
      glm::vec4 sectionPlane = {0.0f, 1.0f, 0.0f, 0.0f}) const;
  [[nodiscard]] uint32_t nodeIndexForObject(uint32_t objectIndex) const;
  [[nodiscard]] SceneNodeWorldBounds nodeWorldBounds(uint32_t nodeIndex) const;
  void collectDrawCommandsForNode(uint32_t nodeIndex,
                                  std::vector<DrawCommand>& outCommands) const;

  // ---- Buffer helpers (also used by other subsystems) ---------------------

  static void writeToBuffer(
      container::gpu::AllocationManager&      allocationManager,
      const container::gpu::AllocatedBuffer&  buffer,
      const void*                              data,
      size_t                                   size);

  // Ensure objectBuffer capacity >= requiredObjectCount.  Returns true if the
  // buffer was (re)created and descriptor sets must be refreshed.
  static bool ensureObjectBufferCapacity(
      container::gpu::AllocationManager&  allocationManager,
      container::gpu::AllocatedBuffer&    objectBuffer,
      size_t&                              objectBufferCapacity,
      size_t                               requiredObjectCount,
      VkBufferUsageFlags                   additionalUsage = 0);

  // Update the guiManager pointer (may be set after construction).
  void setGuiManager(container::ui::GuiManager* guiManager) { guiManager_ = guiManager; }

  const std::vector<DrawCommand>& opaqueDrawCommands()      const { return opaqueDrawCommands_; }
  const std::vector<DrawCommand>& transparentDrawCommands() const { return transparentDrawCommands_; }
  const std::vector<DrawCommand>& opaqueSingleSidedDrawCommands() const {
    return opaqueSingleSidedDrawCommands_;
  }
  const std::vector<DrawCommand>& opaqueWindingFlippedDrawCommands() const {
    return opaqueWindingFlippedDrawCommands_;
  }
  const std::vector<DrawCommand>& opaqueDoubleSidedDrawCommands() const {
    return opaqueDoubleSidedDrawCommands_;
  }
  const std::vector<DrawCommand>& transparentSingleSidedDrawCommands() const {
    return transparentSingleSidedDrawCommands_;
  }
  const std::vector<DrawCommand>& transparentWindingFlippedDrawCommands() const {
    return transparentWindingFlippedDrawCommands_;
  }
  const std::vector<DrawCommand>& transparentDoubleSidedDrawCommands() const {
    return transparentDoubleSidedDrawCommands_;
  }
  const std::vector<container::gpu::ObjectData>&  objectData()              const { return objectData_; }
  [[nodiscard]] std::span<const uint32_t> objectNodeIndices() const noexcept { return objectNodeIndices_; }
  uint64_t objectDataRevision() const { return objectDataRevision_; }

  container::gpu::BufferSlice vertexSlice()         const { return vertexSlice_; }
  container::gpu::BufferSlice indexSlice()          const { return indexSlice_; }
  container::gpu::BufferSlice diagCubeVertexSlice() const { return diagCubeVertexSlice_; }
  container::gpu::BufferSlice diagCubeIndexSlice()  const { return diagCubeIndexSlice_; }
  uint32_t                     diagCubeIndexCount()  const { return diagCubeIndexCount_; }
  uint32_t                     diagCubeObjectIndex() const { return diagCubeObjectIndex_; }

  // ECS world used for draw-call extraction.
  container::ecs::World&       world();
  const container::ecs::World& world() const;

 private:
  [[nodiscard]] PreparedMaterialRouting prepareMaterialRoutingImpl(
      std::span<const container::scene::MaterialRenderProperties>,
      const container::scene::PreparedMaterialBindingChange*,uint32_t,bool,uint64_t,
      const MaterialRoutingLimits&) const;
  struct PrimitiveBounds {
    glm::vec3 center{0.0f};
    float radius{0.0f};
    bool valid{false};
  };

  void rebuildPrimitiveBoundsCache();
  struct OwnedGenerationBuffers;
  [[nodiscard]] PreparedGeneration prepareGenerationImpl(
      container::scene::SceneGraph&,container::scene::SceneManager&,uint32_t,bool,uint64_t,uint64_t,bool preserveContext) const;
  void swapGenerationState(PreparedGeneration::State& state) noexcept;
  void setOwnedGeometrySlices(size_t sceneIndexCount) noexcept;
  void rebuildDeformationBindStreams();
  void invalidateObjectDataCache();
  void refreshObjectDataCache(bool showDiagCube);
  [[nodiscard]] SceneNodePickHit pickRenderableNodeHitForDraws(
      const container::gpu::CameraData& cameraData,
      VkExtent2D viewportExtent,
      double cursorX,
      double cursorY,
      bool includeOpaque,
      bool includeTransparent,
      bool sectionPlaneEnabled,
      glm::vec4 sectionPlane) const;

  std::shared_ptr<container::gpu::VulkanDevice>  device_;
  container::gpu::AllocationManager&             allocationManager_;
  container::gpu::PipelineManager&             pipelineManager_;
  container::scene::SceneGraph&                     sceneGraph_;
  container::scene::SceneManager&                   sceneManager_;
  container::ui::GuiManager*                        guiManager_{nullptr};
  const container::app::AppConfig&                           config_;

  std::unique_ptr<container::ecs::World> world_;
  std::unique_ptr<OwnedGenerationBuffers> ownedGenerationBuffers_;
  // Objects-only override. Full geometry generations carry this owner too;
  // changing draw classification must never replace the geometry allocation.
  std::unique_ptr<OwnedGenerationBuffers> ownedMaterialObjectBuffers_;
  std::unique_ptr<GltfDeformationScene> deformationScene_;
  std::vector<container::geometry::Vertex> gpuBindVertices_;
  std::vector<uint32_t> gpuBindIndices_;
  size_t generationSceneVertexCount_{},generationSceneIndexCount_{};
  uint64_t deformationSourceGeometryRevision_{std::numeric_limits<uint64_t>::max()};
  uint64_t geometryRevision_{0};
  bool deformationTopologyPending_{true};

  std::vector<container::gpu::ObjectData>   objectData_;
  std::vector<DrawCommand>  opaqueDrawCommands_;
  std::vector<DrawCommand>  transparentDrawCommands_;
  std::vector<DrawCommand>  opaqueSingleSidedDrawCommands_;
  std::vector<DrawCommand>  opaqueWindingFlippedDrawCommands_;
  std::vector<DrawCommand>  opaqueDoubleSidedDrawCommands_;
  std::vector<DrawCommand>  transparentSingleSidedDrawCommands_;
  std::vector<DrawCommand>  transparentWindingFlippedDrawCommands_;
  std::vector<DrawCommand>  transparentDoubleSidedDrawCommands_;
  std::vector<PrimitiveBounds> primitiveBounds_;
  std::vector<uint32_t> objectNodeIndices_;
  std::unordered_map<VkBuffer, uint64_t> objectBufferUploadRevisions_;

  uint64_t cachedSceneGraphRevision_{std::numeric_limits<uint64_t>::max()};
  uint64_t cachedGeometryRevision_{std::numeric_limits<uint64_t>::max()};
  uint64_t cachedMaterialRevision_{std::numeric_limits<uint64_t>::max()};
  uint64_t objectDataRevision_{0};
  uint64_t objectBufferUploadRevision_{0};
  uint64_t lastObjectUploadBytes_{0};
  bool cachedShowDiagCube_{false};
  bool objectDataCacheValid_{false};

  container::gpu::BufferSlice vertexSlice_{};
  container::gpu::BufferSlice indexSlice_{};
  container::gpu::BufferSlice diagCubeVertexSlice_{};
  container::gpu::BufferSlice diagCubeIndexSlice_{};
  uint32_t diagCubeIndexCount_{0};
  uint32_t diagCubeObjectIndex_{std::numeric_limits<uint32_t>::max()};
};

}  // namespace container::renderer
