#pragma once

#include <Container/geometry/GltfAnimationRuntime.h>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/vec3.hpp>

#include "Container/app/SceneLightingDefaults.h"
#include "Container/app/GfxCapture.h"
#include "Container/renderer/bim/BimInspectionCache.h"
#include "Container/renderer/core/PushConstantBlock.h"
#include "Container/renderer/core/RendererDeviceCapabilities.h"
#include "Container/renderer/debug/DebugRenderState.h"
#include "Container/renderer/lighting/EditableLight.h"
#include "Container/utility/EditHistory.h"
#include "Container/renderer/lighting/DdgiProbeVolume.h"
#include "Container/renderer/lighting/DdgiSceneChanges.h"
#include "Container/renderer/picking/RenderSurfaceInteractionController.h"
#include "Container/renderer/resources/RenderResources.h"
#include "Container/renderer/scene/DrawCommand.h"
#include "Container/renderer/scene/SceneState.h"
#include "Container/scene/MeshSceneProviderAssetAdapter.h"
#include "Container/utility/VulkanMemoryManager.h"

struct GLFWwindow;

namespace container::temporal {
struct CaptureSample;
struct CaptureEvent;
struct CaptureLightTarget;
struct CaptureNodeEdit;
}

// Forward declarations — full headers are only needed in RendererFrontend.cpp.
namespace container::app {
struct AppConfig;
} // namespace container::app

namespace container::renderer {
class BloomManager;
class TemporalManager;
class RayShadowManager;
class ReferencePathTracePass;
class DdgiManager;
class BimManager;
enum class BimDisciplinePreset : uint32_t;
struct BimDrawFilter;
class CameraController;
class CommandBufferManager;
class EnvironmentManager;
class ExposureManager;
class DeferredRasterFrameGraphContext;
class FrameResourceRegistry;
class FrameRecorder;
struct FrameRecordParams;
class FrameResourceManager;
class GraphicsPipelineBuilder;
class GpuCullManager;
class LightingManager;
class OitManager;
class PipelineRegistry;
class RenderPassGpuProfiler;
class RenderTechnique;
enum class RenderTechniqueId;
class RenderTechniqueRegistry;
struct RenderSystemContext;
class RendererTelemetry;
class SceneController;
class GltfDeformationPass;
class SceneProviderSynchronizer;
class ShadowCullManager;
class ShadowManager;
struct VulkanContextResult;
} // namespace container::renderer

namespace container::gpu {
class AllocationManager;
class FrameSyncManager;
class PipelineManager;
class SwapChainManager;
} // namespace container::gpu

namespace container::scene {
class SceneGraph;
class SceneManager;
class SceneProviderRegistry;
} // namespace container::scene

namespace container::ui {
class GuiManager;
struct ViewpointSnapshotState;
} // namespace container::ui

namespace container::window {
class InputManager;
} // namespace container::window

namespace container::renderer {

// Groups all construction parameters for RendererFrontend.
struct RendererFrontendCreateInfo {
  VulkanContextResult *ctx{nullptr};
  container::gpu::PipelineManager *pipelineManager{nullptr};
  container::gpu::AllocationManager *allocationManager{nullptr};
  container::gpu::SwapChainManager *swapChainManager{nullptr};
  CommandBufferManager *commandBufferManager{nullptr};
  const container::app::AppConfig *config{nullptr};
  GLFWwindow *nativeWindow{nullptr};
  container::window::InputManager *inputManager{nullptr};
};

// RendererFrontend owns the renderer-facing lifetime graph. The application
// handles window/input setup, while this class creates render passes, frame
// resources, scene systems, pipelines, and per-frame submission state.
class RendererFrontend {
public:
  explicit RendererFrontend(RendererFrontendCreateInfo info);

  ~RendererFrontend();
  RendererFrontend(const RendererFrontend &) = delete;
  RendererFrontend &operator=(const RendererFrontend &) = delete;

  // Full initialization: render passes → pipelines → scene → sync primitives.
  void initialize();

  // Submit one frame. Returns false if swap chain needs recreation (caller
  // should set framebufferResized = false and call handleResize()).
  bool drawFrame(bool &framebufferResized);

  // Handle window resize / suboptimal swapchain.
  void handleResize();

  // Process keyboard / camera input for this tick.
  void processInput(float deltaTime);

  // Capture the next submitted swapchain image to an sRGB PNG.
  void requestScreenshot(std::filesystem::path outputPath, bool viewportOnly = false);
  void applyTemporalCapture(const container::temporal::CaptureSample &sample);
  void writeCaptureTelemetry(const std::filesystem::path &path,
                             std::optional<glm::uvec2> depthProbe = std::nullopt);
  void startGfxCapture();
  void gfxCaptureTick(uint64_t tick, bool skipped);
  [[nodiscard]] bool gfxCaptureComplete() const;

  // Scene operations forwarded from the application.
  bool reloadSceneModel(const std::string &path, float importScale = 1.0f);
  // Deterministic captures settle requests while continuing event/UI processing.
  void settlePendingModelLoads();

  // Shutdown: wait idle and release all Vulkan resources in dependency order.
  void shutdown();

  // Access debug state (allows the application to expose toggles if needed).
  DebugRenderState &debugState() { return debugState_; }
  const DebugRenderState &debugState() const { return debugState_; }

  const SceneState &sceneState() const { return sceneState_; }

private:
  struct ModelLoadState;
  std::unique_ptr<ModelLoadState> modelLoad_;
  void pumpModelLoadProgress(std::string_view stage, bool render = true);
  void drawModelLoadProgressFrame();
  container::capture::Journal gfxJournal_;
  [[nodiscard]] nlohmann::json captureTelemetry() const;
  std::optional<float> captureExposure_{};
  std::optional<glm::vec4> captureSectionPlane_{};
  std::optional<uint32_t> captureBimHiddenObject_{};
  std::optional<int> captureBimLodBias_{};
  std::optional<uint32_t> pendingDdgiSceneFit_{};
  std::optional<DdgiProbeVolumeDesc> fittedDdgiVolume_{};
  glm::vec3 ddgiFitBoundsMin_{}, ddgiFitBoundsMax_{};
  uint32_t ddgiFitProbeBudget_{0}, ddgiFitProbeCount_{0};
  uint64_t ddgiFitPayloadBytes_{0};
  std::string ddgiFitStatus_{"No scene fit requested"};
  DdgiSceneChangeTracker ddgiSceneChanges_{};
  uint64_t ddgiTrackedSceneRevision_{0};
  void fitDdgiToActiveScene(uint32_t maxProbes);
  void processPendingDdgiSceneFit();
  void fitDdgiDetailToSelection(uint32_t maxProbes, std::string_view nodeName = {});
  void processPendingDdgiDetailFit();
  [[nodiscard]] std::string ddgiActiveSceneIdentity() const;
  void processPendingDdgiPreset();
  std::optional<uint64_t> temporalClipRevision_{};
  bool captureAcquireOutOfDate_{false}, capturePresentSuboptimal_{false};
  std::optional<glm::mat4> captureObjectBase_{};
  uint32_t captureObjectNode_{std::numeric_limits<uint32_t>::max()};
  // Owned subsystems are listed roughly in construction/use order. shutdown()
  // releases them in dependency-aware order because many destructors touch
  // Vulkan objects owned by earlier services.
  struct OwnedSubsystems {
    std::unique_ptr<RenderPassManager> renderPassManager;
    std::unique_ptr<OitManager> oitManager;
    std::unique_ptr<FrameResourceManager> frameResourceManager;
    std::unique_ptr<container::scene::SceneManager> sceneManager;
    std::unique_ptr<BimManager> bimManager;
    std::unique_ptr<SceneController> sceneController;
    std::unique_ptr<GltfDeformationPass> gltfDeformationPass;
    std::unique_ptr<CameraController> cameraController;
    std::unique_ptr<LightingManager> lightingManager;
    std::unique_ptr<ShadowCullManager> shadowCullManager;
    std::unique_ptr<ShadowManager> shadowManager;
    std::unique_ptr<EnvironmentManager> environmentManager;
    std::unique_ptr<GpuCullManager> gpuCullManager;
    std::unique_ptr<BloomManager> bloomManager;
    std::unique_ptr<TemporalManager> temporalManager;
    std::unique_ptr<RayShadowManager> rayShadowManager;
    std::unique_ptr<ReferencePathTracePass> referencePathTracePass;
    std::unique_ptr<DdgiManager> ddgiManager;
    std::unique_ptr<ExposureManager> exposureManager;
    std::unique_ptr<GraphicsPipelineBuilder> pipelineBuilder;
    std::unique_ptr<FrameRecorder> frameRecorder;
    std::unique_ptr<DeferredRasterFrameGraphContext>
        deferredRasterFrameGraphContext;
    std::unique_ptr<FrameResourceRegistry> frameResourceRegistry;
    std::unique_ptr<FrameResourceRegistry> frameRuntimeResourceRegistry;
    std::unique_ptr<PipelineRegistry> pipelineRegistry;
    std::unique_ptr<SceneProviderSynchronizer> sceneProviderSynchronizer;
    std::unique_ptr<container::scene::SceneProviderRegistry>
        sceneProviderRegistry;
    RendererDeviceCapabilities deviceCapabilities{
        RendererDeviceCapabilities::rasterOnly()};
    std::unique_ptr<RenderTechniqueRegistry> techniqueRegistry;
    RenderTechnique *activeTechnique{nullptr};
    std::unique_ptr<RenderPassGpuProfiler> renderPassGpuProfiler;
    std::unique_ptr<RendererTelemetry> rendererTelemetry;
    std::unique_ptr<container::ui::GuiManager> guiManager;
    std::unique_ptr<container::gpu::FrameSyncManager> frameSyncManager;
  };

  // External services passed in at construction. These must outlive the
  // frontend; they wrap the device, swapchain, allocator, command pool, and
  // app configuration supplied by the application layer.
  struct BorrowedServices {
    VulkanContextResult &ctx;
    container::gpu::PipelineManager &pipelineManager;
    container::gpu::AllocationManager &allocationManager;
    container::gpu::SwapChainManager &swapChainManager;
    CommandBufferManager &commandBufferManager;
    const container::app::AppConfig &config;
    GLFWwindow *nativeWindow{nullptr};
    container::window::InputManager &inputManager;
  };

  OwnedSubsystems subs_;
  BorrowedServices svc_;

  // ---- grouped state
  // ----------------------------------------------------------
  RenderResources resources_{};
  PushConstantBlock pushConstants_{};
  VkSampleCountFlagBits msaaSampleCount_{VK_SAMPLE_COUNT_1_BIT};
  std::vector<uint32_t> supportedMsaaSamples_{1u};
  std::optional<VkSampleCountFlagBits> pendingMsaaSampleCount_{};
  std::optional<RenderTechniqueId> pendingRenderTechniqueChange_{};

  // GPU buffers backing the camera UBO and per-object SSBO.
  struct SceneBufferState {
    std::vector<container::gpu::AllocatedBuffer> cameras;
    std::vector<container::gpu::AllocatedBuffer> objects;
    std::vector<size_t> objectCapacities;
    container::gpu::CameraData cameraData{};
    std::vector<bool> shadowObjectDescriptorReady;
  };
  SceneBufferState buffers_{};

  container::scene::SceneGraph sceneGraph_{};
  std::unique_ptr<container::geometry::gltf::AnimationPlayer> gltfAnimation_;
  std::optional<uint64_t> gltfAnimationSourceRevision_;
  std::optional<size_t> gltfAnimationCameraNode_;
  std::optional<container::geometry::gltf::AnimatedCameraState> gltfAnimationCamera_;
  double pendingGltfAnimationDeltaSeconds_{};
  void resetGltfAnimation(bool applyStartupOptions);
  void updateGltfAnimation();
  void updateGltfDeformation();
  void prepareGltfDeformationFrame(uint32_t imageIndex);
  void commitGltfDeformationFrame();
  void appendGltfDeformationTelemetry(nlohmann::json& json) const;
  uint64_t gltfDeformationSourceIdentity_{};
  uint64_t gltfDeformationSourceRevision_{};
  uint64_t gltfDeformationTopologyRevision_{};
  size_t gltfDeformationVertexCount_{};
  uint32_t gltfDeformationImageCount_{};
  std::optional<uint32_t> gltfDeformationSubmittedEpoch_;
  float gltfDeformationMaxPositionDelta_{};
  bool gltfDeformationHistoryReset_{true};
  bool gltfDeformationCurrentBoundToGpu_{};
  bool gltfDeformationPreviousBoundToGpu_{};
  void applyGltfAnimationCapture(const container::temporal::CaptureEvent& event);
  void publishGltfAnimation(const container::geometry::gltf::AnimationDirtySet& dirty,
                            bool discontinuity);
  void refreshGltfCameraChoices();
  void selectGltfCamera(std::optional<size_t> node);
  void appendGltfAnimationTelemetry(nlohmann::json& json) const;
  SceneState sceneState_{};
  DebugRenderState debugState_{};
  RenderSurfaceInteractionController interactionController_{};
  uint32_t hoveredMeshNode_{container::scene::SceneGraph::kInvalidNode};
  uint32_t hoveredBimObjectIndex_{std::numeric_limits<uint32_t>::max()};
  uint32_t selectedBimObjectIndex_{std::numeric_limits<uint32_t>::max()};
  BimInspectionCache bimInspectionCache_{};
  container::scene::MeshSceneProviderAssetCache meshProviderAssetCache_{};
  struct SelectionNavigationAnchor {
    bool valid{false};
    glm::vec3 point{0.0f};
    float radius{1.0f};
    uint32_t sceneNode{container::scene::SceneGraph::kInvalidNode};
    uint32_t bimObject{std::numeric_limits<uint32_t>::max()};
  };
  SelectionNavigationAnchor selectionNavigationAnchor_{};
  struct HoverPickCache {
    bool valid{false};
    double cursorX{0.0};
    double cursorY{0.0};
    uint32_t selectedMeshNode{container::scene::SceneGraph::kInvalidNode};
    uint32_t selectedBimObjectIndex{std::numeric_limits<uint32_t>::max()};
    uint64_t objectDataRevision{0};
    uint64_t bimObjectDataRevision{0};
    bool bimTypeFilterEnabled{false};
    std::string bimFilterType{};
    bool bimStoreyFilterEnabled{false};
    std::string bimFilterStorey{};
    bool bimMaterialFilterEnabled{false};
    std::string bimFilterMaterial{};
    bool bimDisciplineFilterEnabled{false};
    std::string bimFilterDiscipline{};
    BimDisciplinePreset bimDisciplinePreset{
        static_cast<BimDisciplinePreset>(0u)};
    bool bimPhaseFilterEnabled{false};
    std::string bimFilterPhase{};
    bool bimPhaseTimelineEnabled{false};
    uint32_t bimPhaseTimelineActiveIndex{0};
    bool bimPhaseTimelineShowExisting{true};
    bool bimPhaseTimelineShowNew{true};
    bool bimPhaseTimelineShowDemolished{false};
    bool bimPhaseTimelineGhostFuture{false};
    bool bimFireRatingFilterEnabled{false};
    std::string bimFilterFireRating{};
    bool bimLoadBearingFilterEnabled{false};
    std::string bimFilterLoadBearing{};
    bool bimStatusFilterEnabled{false};
    std::string bimFilterStatus{};
    bool bimDrawBudgetEnabled{false};
    uint32_t bimDrawBudgetMaxObjects{0};
    bool bimIsolateSelection{false};
    bool bimHideSelection{false};
    bool bimPointCloudVisible{true};
    bool bimCurvesVisible{true};
    bool sectionPlaneEnabled{false};
    glm::vec4 sectionPlane{0.0f, 1.0f, 0.0f, 0.0f};
    container::gpu::CameraData cameraData{};
  };
  HoverPickCache hoverPickCache_{};
  std::vector<DrawCommand> hoveredBimDrawCommands_{};
  std::vector<DrawCommand> selectedBimDrawCommands_{};
  std::vector<DrawCommand> hoveredBimNativePointDrawCommands_{};
  std::vector<DrawCommand> selectedBimNativePointDrawCommands_{};
  std::vector<DrawCommand> hoveredBimNativeCurveDrawCommands_{};
  std::vector<DrawCommand> selectedBimNativeCurveDrawCommands_{};
  std::vector<DrawCommand> hoveredDrawCommands_{};
  std::vector<DrawCommand> selectedDrawCommands_{};
  std::string activePrimaryModelPath_{};
  container::app::SceneLightingDefaults sceneLightingDefaults_{};
  float activePrimaryImportScale_{1.0f};
  std::string activeAuxiliaryModelPath_{};
  float activeAuxiliaryImportScale_{1.0f};

  // Per-frame synchronisation / bookkeeping.
  struct FrameState {
    std::vector<VkFence> imagesInFlight;
    uint32_t currentFrame{0};
    uint64_t submittedFrameCount{0};
  };
  FrameState frame_{};

  struct HostReadbackSlot {
    container::gpu::AllocatedBuffer readbackBuffer{};
    VkDeviceSize readbackSize{0};
    VkExtent2D extent{};
    VkFormat format{VK_FORMAT_UNDEFINED};
  };

  struct ScreenshotState {
    std::filesystem::path outputPath{};
    bool sceneLinear{false};
    bool viewportOnly{false};
    bool interactive{false};
    bool pending{false};
    std::vector<HostReadbackSlot> readbacks{};
  };
  ScreenshotState screenshot_{};
  std::vector<bool> referenceColorBound_;
  std::vector<uint64_t> referenceDiagnosticSeen_;
  uint64_t referenceDiagnosticEpoch_{}, referenceUnsupported_{}, referenceRejected_{};
  bool referenceRecorded_{};
  std::string referenceStatus_;

  struct DepthVisibilityFrameSlot {
    HostReadbackSlot readback{};
    uint32_t frameSlot{0};
    uint32_t imageIndex{std::numeric_limits<uint32_t>::max()};
    VkExtent2D extent{};
    VkFormat format{VK_FORMAT_UNDEFINED};
    container::gpu::CameraData cameraData{};
    uint64_t objectDataRevision{0};
    uint64_t bimObjectDataRevision{0};
    bool sectionPlaneEnabled{false};
    glm::vec4 sectionPlane{0.0f, 1.0f, 0.0f, 0.0f};
    bool bimTypeFilterEnabled{false};
    std::string bimFilterType{};
    bool bimStoreyFilterEnabled{false};
    std::string bimFilterStorey{};
    bool bimMaterialFilterEnabled{false};
    std::string bimFilterMaterial{};
    bool bimDisciplineFilterEnabled{false};
    std::string bimFilterDiscipline{};
    BimDisciplinePreset bimDisciplinePreset{
        static_cast<BimDisciplinePreset>(0u)};
    bool bimPhaseFilterEnabled{false};
    std::string bimFilterPhase{};
    bool bimPhaseTimelineEnabled{false};
    uint32_t bimPhaseTimelineActiveIndex{0};
    bool bimPhaseTimelineShowExisting{true};
    bool bimPhaseTimelineShowNew{true};
    bool bimPhaseTimelineShowDemolished{false};
    bool bimPhaseTimelineGhostFuture{false};
    bool bimFireRatingFilterEnabled{false};
    std::string bimFilterFireRating{};
    bool bimLoadBearingFilterEnabled{false};
    std::string bimFilterLoadBearing{};
    bool bimStatusFilterEnabled{false};
    std::string bimFilterStatus{};
    bool bimDrawBudgetEnabled{false};
    uint32_t bimDrawBudgetMaxObjects{0};
    bool bimIsolateSelection{false};
    bool bimHideSelection{false};
    bool bimPointCloudVisible{true};
    bool bimCurvesVisible{true};
    bool transparentPickDepthValid{false};
    uint32_t selectedBimObjectIndex{std::numeric_limits<uint32_t>::max()};
    VkFence renderFence{VK_NULL_HANDLE};
    bool valid{false};
  };

  struct DepthVisibilityState {
    std::vector<DepthVisibilityFrameSlot> slots{};
    uint32_t latestFrameSlot{0};
  };
  DepthVisibilityState depthVisibility_{};

  container::util::EditHistory editHistory_{};
  uint64_t lightHistoryScene_{}, lightHistoryAsset_{};
  std::optional<bool> captureHistoryRequest_;
  void ensureSceneEditHistory();
  bool applyEditHistory(bool redo);
  struct LightEditHistory;
  struct LightEditHistoryBudget;
  struct PendingLightGesture;
  struct LightEditPublication;
  struct PendingCaptureLight;
  std::shared_ptr<LightEditHistoryBudget> lightEditHistoryBudget_;
  std::shared_ptr<PendingLightGesture> lightEditGesture_;
  std::shared_ptr<PendingCaptureLight> pendingCaptureLight_;
  std::optional<EditableLightId> pendingLightSelection_;
  std::string lightEditError_;
  uint64_t lightEditPublications_{},lightEditHistoryCommits_{},lightEditHistoryReplays_{};
  uint64_t nextLightGizmoGesture_{uint64_t{1}<<62};
  uint64_t lastFinishedLightGesture_{};
  [[nodiscard]] bool lightEditGestureActive() const noexcept;
  void finishLightEditInput();
  void refreshLightEditInspector();
  void reportLightEditError(std::string_view);
  void selectLightForEditing(EditableLightId);
  void processLightEditRequests();
  bool authorLightEditRequest(const LightEditRequest&);
  bool authorManualLightAdd(EditableLightType);
  void finishLightEditGesture(bool cancel=false);
  bool replayLightEdit(LightEditHistory&,bool applied);
  bool publishLightEdit(LightEditPublication&);
  void beginLightGizmoGesture(container::ui::ViewportTool,
      container::ui::TransformSpace,container::ui::TransformAxis,bool snapEnabled);
  void endLightGizmoGesture();
  void cancelLightGizmoGesture(RenderSurfaceInteractionController::TransformGestureCancelReason);
  bool applyLightGizmoControls(container::ui::ViewportTool,container::ui::TransformAxis,
      const container::ui::TransformControls&);
  void appendLightEditTelemetry(nlohmann::json&) const;
  void queueCaptureLightEvent(const container::temporal::CaptureEvent&);
  void processCaptureLightEvent();
  EditableLightId resolveCaptureLightTarget(const container::temporal::CaptureLightTarget&) const;

  enum class NodeEditKind { Transform, Visibility, Reparent };
  struct NodePlacement {
    glm::mat4 local{1};
    uint32_t parent{};
    size_t sibling{};
    bool visible{};
    bool operator==(const NodePlacement&) const = default;
  };
  struct NodeEditRequest {
    NodeEditKind kind;
    uint32_t node;
    container::ui::TransformControls controls{};
    std::optional<uint64_t> widget{};
    std::optional<uint32_t> parent{};
    bool visible{};
  };
  struct NodeEditSelection {
    uint32_t mesh, bim;
    EditableLightId light;
    bool sectionEditable;
  };
  struct NodeGesture;
  std::shared_ptr<NodeGesture> nodeGesture_;
  std::vector<NodeEditRequest> pendingNodeEdits_;
  std::shared_ptr<container::temporal::CaptureNodeEdit> captureNodeEdit_;
  std::optional<uint32_t> captureNodeTelemetry_;
  std::optional<NodePlacement> nodePlacement(uint32_t node) const;
  bool nodeHasActivePoseTrack(uint32_t node) const;
  NodeEditSelection nodeEditSelection() const;
  void restoreNodeEditSelection(const NodeEditSelection&);
  bool publishNodePlacement(uint32_t node, uint64_t graph, uint64_t incarnation,
                            NodeEditKind kind, const NodePlacement& expected, const NodePlacement& desired,
                            const NodeEditSelection* selection = nullptr);
  bool recordNodeEdit(uint32_t node, NodeEditKind kind, const NodePlacement& before,
                      const NodePlacement& after, std::string label);
  bool editNodeVisibility(uint32_t node, bool visible);
  bool reparentNode(uint32_t node, std::optional<uint32_t> parent);
  void beginNodeGesture(uint32_t node, uint64_t widget, bool gizmo);
  bool previewNodePose(const glm::mat4& local);
  void finishNodeGesture(bool cancel = false);
  bool nodeGizmoActive() const noexcept;
  void applyNodeGizmoControls(container::ui::ViewportTool, container::ui::TransformAxis,
                              const container::ui::TransformControls&);
  void processNodeEditRequests();
  void processCaptureNodeEdit();
  void appendNodeEditTelemetry(nlohmann::json&) const;

  struct TransformDragSession {
    bool active{false};
    uint32_t nodeIndex{std::numeric_limits<uint32_t>::max()};
    container::ui::ViewportTool tool{container::ui::ViewportTool::Select};
    container::ui::TransformSpace space{container::ui::TransformSpace::World};
    container::ui::TransformAxis axis{container::ui::TransformAxis::Free};
    bool snapEnabled{false};
    container::ui::TransformControls startControls{};
    container::ui::SectionPlaneState startSectionPlane{};
    glm::vec3 origin{0.0f};
    float gizmoScale{1.0f};
    glm::vec3 axisX{1.0f, 0.0f, 0.0f};
    glm::vec3 axisY{0.0f, 1.0f, 0.0f};
    glm::vec3 axisZ{0.0f, 0.0f, 1.0f};
    double accumulatedDeltaX{0.0};
    double accumulatedDeltaY{0.0};
  };
  TransformDragSession transformDragSession_{};

  // ---- internal init helpers
  // --------------------------------------------------
  void createRenderPasses();
  void createGraphicsPipelines();
  void destroyGraphicsPipelines();
  void recreateMsaaResources(VkSampleCountFlagBits sampleCount);
  void createCamera();
  void resetCameraForActiveScene();
  void applySceneLightingDefaults();
  void syncCameraSelectionPivotOverride();
  void initializeScene();
  void buildSceneGraph();
  void createSceneBuffers();
  void createGeometryBuffers();
  void createFrameResources();
  void ensureCameraBuffers();
  void ensureObjectBuffers();
  void syncSceneProviders();

  // ---- per-frame helpers
  // ------------------------------------------------------
  void updateCameraBuffer(uint32_t imageIndex);
  void refreshSceneObjectData();
  void updateObjectBuffer(uint32_t imageIndex);
  void updateAllObjectBuffers();
  [[nodiscard]] container::gpu::AllocatedBuffer sceneObjectBuffer(
      uint32_t imageIndex) const;
  [[nodiscard]] size_t sceneObjectCapacity(uint32_t imageIndex) const;
  [[nodiscard]] size_t maxSceneObjectCapacity() const;
  void applyBimSemanticColorMode();
  void
  updateFrameDescriptorSets(uint32_t imageIndex = UINT32_MAX,
                            const FrameRecordParams *preparedParams = nullptr);
  void destroyGBufferResources();
  bool growExactOitNodePoolIfNeeded(uint32_t imageIndex);
  HostReadbackSlot& ensureReadbackSlot(
      std::vector<HostReadbackSlot>& readbacks, uint32_t frameSlot);
  void destroyReadbackSlots(std::vector<HostReadbackSlot>& readbacks);
  DepthVisibilityFrameSlot& ensureDepthVisibilityFrameSlot(uint32_t frameSlot);
  DepthVisibilityFrameSlot* depthVisibilityFrameSlot(uint32_t frameSlot);
  [[nodiscard]] const DepthVisibilityFrameSlot* depthVisibilityFrameSlot(
      uint32_t frameSlot) const;
  void invalidateDepthVisibilityFrames();
  void destroyDepthVisibilityFrameSlots();
  void ensureScreenshotReadbackBuffer(uint32_t frameSlot, VkExtent2D extent,
                                      VkFormat format);
  void writePendingScreenshot(uint32_t frameSlot, const FrameRecordParams& params);
  bool referencePathActive() const;
  void ensureReferencePathResources();
  bool recordReferencePathTrace(VkCommandBuffer, const FrameRecordParams&);
  void finishReferencePathTrace(VkCommandBuffer);
  void collectReferenceDiagnostics(uint32_t imageIndex);
  void updateReferenceColorSource(uint32_t imageIndex, bool sceneLinear);
  void publishReferenceColorBinding(uint32_t imageIndex);
  void ensureDepthVisibilityReadbackBuffer(uint32_t frameSlot);
  void markDepthVisibilityFrameComplete(uint32_t imageIndex,
                                        uint32_t frameSlot);
  [[nodiscard]] bool sampleDepthAtCursor(double cursorX, double cursorY,
                                         float &outDepth);
  [[nodiscard]] bool samplePickDepthAtCursor(double cursorX, double cursorY,
                                             float &outDepth);
  [[nodiscard]] bool sampleDepthAtCursor(double cursorX, double cursorY,
                                         float &outDepth, bool pickDepth);
  [[nodiscard]] bool samplePickIdAtCursor(double cursorX, double cursorY,
                                          uint32_t &outPickId);
  [[nodiscard]] bool depthVisibilityFrameMatchesCurrentState() const;
  void processPendingGuiModelLoadRequest();
  [[nodiscard]] BimDrawFilter currentBimDrawFilter() const;
  [[nodiscard]] bool bimObjectVisibleByLayer(uint32_t objectIndex) const;
  [[nodiscard]] container::ui::ViewpointSnapshotState
  currentViewpointSnapshot() const;
  bool restoreViewpointSnapshot(
      const container::ui::ViewpointSnapshotState &snapshot);
  void presentSceneControls();
  void selectMeshNodeAtCursor(double cursorX, double cursorY);
  void selectBimObject(uint32_t objectIndex,
                       std::optional<glm::vec3> anchorPoint = std::nullopt);
  void hoverMeshNodeAtCursor(double cursorX, double cursorY);
  void clearHoveredMeshNode();
  void clearSelectedMeshNode();
  void transformSelectedNodeByDrag(container::ui::ViewportTool tool,
                                   container::ui::TransformSpace space,
                                   container::ui::TransformAxis axis,
                                   bool snapEnabled, double deltaX,
                                   double deltaY);
  [[nodiscard]] std::optional<container::ui::TransformAxis>
  pickTransformGizmoAxisAtCursor(double cursorX, double cursorY) const;
  void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex,
                           const FrameRecordParams *preparedParams = nullptr);
  [[nodiscard]] FrameRecordParams buildFrameRecordParams(uint32_t imageIndex);
  void attachActiveTechniqueLifecycle(FrameRecordParams &params);
  void publishFrameRuntimeResourceBindings(uint32_t imageIndex);
  [[nodiscard]] FrameTransformGizmoState buildTransformGizmoState() const;
  [[nodiscard]] RenderSystemContext renderSystemContext();
  void initializeRenderTechnique(RenderTechniqueId requested,
                                 std::string_view requestLabel);
  void requestRenderTechnique(RenderTechniqueId requested);
  void applyPendingRenderTechniqueChange();
  void syncGuiRenderEngineOptions();

  // ---- scene helpers
  // ----------------------------------------------------------
  void syncSceneStateFromController();
};

} // namespace container::renderer
