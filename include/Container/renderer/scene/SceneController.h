#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/common/CommonMath.h"
#include "Container/renderer/debug/DebugOverlayRenderer.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
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

namespace container::scene {
class SceneGraph;
class SceneManager;
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

  // ---- Geometry buffers ---------------------------------------------------

  // Uploads scene + diagnostic-cube vertices/indices into a shared GPU arena.
  void createGeometryBuffers();

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
  // re-update descriptor sets).
  bool updateObjectBuffer(
      container::gpu::AllocatedBuffer&       objectBuffer,
      size_t&                                 objectBufferCapacity,
      const container::gpu::AllocatedBuffer& cameraBuffer);

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

  bool reloadSceneModel(
      const std::string&                      path,
      float                                   importScale,
      container::gpu::AllocatedBuffer&       objectBuffer,
      size_t&                                 objectBufferCapacity,
      const container::gpu::AllocatedBuffer& cameraBuffer,
      VkIndexType&                            outIndexType,
      uint32_t&                               outRootNode,
      uint32_t&                               outSelectedMeshNode,
      uint32_t&                               outCubeNode);

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
      size_t                               requiredObjectCount);

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
  struct PrimitiveBounds {
    glm::vec3 center{0.0f};
    float radius{0.0f};
    bool valid{false};
  };

  void rebuildPrimitiveBoundsCache();
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
  uint64_t objectDataRevision_{0};
  uint64_t objectBufferUploadRevision_{0};
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
