#pragma once

#include <cstdint>
#include <filesystem>
#include <future>
#include <stop_token>
#include <limits>
#include <memory>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "Container/app/SceneLightingDefaults.h"
#include "Container/app/GfxCapture.h"
#include "Container/renderer/bim/BimInspectionCache.h"
#include "Container/renderer/core/PushConstantBlock.h"
#include "Container/renderer/core/RendererDeviceCapabilities.h"
#include "Container/renderer/debug/DebugRenderState.h"
#include "Container/renderer/lighting/EditableLight.h"
#include "Container/renderer/lighting/LightEditTypes.h"
#include "Container/renderer/lighting/DdgiProbeVolume.h"
#include "Container/renderer/lighting/DdgiSceneChanges.h"
#include "Container/renderer/pathtracing/ReferenceOidnSettings.h"
#include "Container/renderer/pathtracing/ReferencePathDisplayMode.h"
#include "Container/renderer/temporal/UpscaleMode.h"
#include "Container/renderer/picking/RenderSurfaceInteractionController.h"
#include "Container/renderer/resources/RenderResources.h"
#include "Container/renderer/scene/DrawCommand.h"
#include "Container/renderer/scene/PhysicsDemoNodes.h"
#include "Container/renderer/scene/SceneState.h"
#include "Container/scene/MeshSceneProviderAssetAdapter.h"
#include "Container/utility/VulkanMemoryManager.h"
#include "Container/utility/SceneEditHistory.h"
#include "Container/geometry/GltfAnimationRuntime.h"
#include "Container/geometry/GltfInteractivity.h"
#include "Container/geometry/GltfCameraAuthoring.h"
#include "Container/utility/GltfMaterialEditRequest.h"
#include "Container/utility/GltfMaterialBindingRequest.h"
#include "Container/utility/ResourceCatalogControls.h"
#include "Container/utility/CameraProjectionTarget.h"
#include "Container/utility/CameraViewPoseTarget.h"

struct GLFWwindow;

namespace container::temporal {
struct CaptureViewportPointer;
struct CaptureSample;
struct CaptureComfyUi;
struct CapturePhysicsJointCreate;
struct CaptureEvent;
struct CaptureLightTarget;
}
namespace container::ui { struct GltfAnimationControls; struct SceneNodeAuthoringRequest; }
namespace container::asl { class PoseReplayScene; }
namespace container::geometry {
struct GltfLoadResult;
namespace gltf { struct GltfMergeOptions; }
class OpenVdbVolumeSequence;
class OpenVdbAsyncVolumeSequence;
class OpenVdbSequencePlayback;
}
namespace container::physics {
class PhysicsScene;
class PhysicsElementBindings;
struct PhysicsBodySettings;
}

// Forward declarations — full headers are only needed in RendererFrontend.cpp.
namespace container::app {
struct AppConfig;
} // namespace container::app

namespace container::renderer {
class BloomManager;
class OpenVdbVolumeRenderer;
class TemporalManager;
class RayShadowManager;
class ReferencePathTracePass;
class DdgiManager;
class BimManager;
enum class BimDisciplinePreset : uint32_t;
struct BimDrawFilter;
class CameraController;
enum class ScenePrimitiveKind : uint32_t;
class CommandBufferManager;
class EnvironmentManager;
class ExposureManager;
class DeferredRasterFrameGraphContext;
class FrameResourceRegistry;
class FrameRecorder;
struct FrameRecordParams;
class FrameResourceManager;
class GraphicsPipelineBuilder;
class ShaderReloadCandidate;
class GpuCullManager;
class LightingManager;
struct SceneLightEdits;
struct ImportedSceneLightEdits;
class OitManager;
class PipelineRegistry;
class RenderPassGpuProfiler;
class RendererMcpServer;
struct ComfyUiFrontendState;
class RenderTechnique;
enum class RenderTechniqueId;
class RenderTechniqueRegistry;
struct RenderSystemContext;
class RendererTelemetry;
class GpuHardwareMonitor;
class SceneController;
class SceneProviderSynchronizer;
struct SceneProviderSyncInput;
struct MeshSceneProviderSyncInput;
class ShadowCullManager;
class ShadowManager;
class GltfDeformationPass;
class GpuHierarchyPass;
class RayInstanceTransformPass;
class PbrAuxiliaryPass;
class ScreenReflectionPass;
class VolumetricFogPass;
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
struct AuthoredLightBinding;
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
  void processInput(float deltaTime, bool useViewportInput = true);

  // Capture the next submitted frame as an sRGB PNG or scene-linear EXR.
  void requestScreenshot(std::filesystem::path outputPath, bool viewportOnly = false,
                         std::optional<uint32_t> referenceSampleTarget = std::nullopt);
  void saveSceneModel(const std::filesystem::path& outputPath, bool keepLinks = false);
  bool selectMaterialVariant(std::optional<size_t> variant,bool recordHistory=true);
  void applyTemporalCapture(const container::temporal::CaptureSample &sample);
  void applyCaptureViewportPointer(const container::temporal::CaptureViewportPointer& pointer);
  [[nodiscard]] bool comfyUiCapturePending() const;
  void writeCaptureTelemetry(const std::filesystem::path &path,
                             std::optional<glm::uvec2> depthProbe = std::nullopt,
                             std::optional<uint32_t> referenceSampleTarget = std::nullopt);
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
  std::unique_ptr<container::asl::PoseReplayScene> aslPoseReplay_;
  std::unique_ptr<container::geometry::OpenVdbVolumeSequence> vdbSequence_;
  std::unique_ptr<container::geometry::OpenVdbAsyncVolumeSequence> vdbAsyncSequence_;
  std::unique_ptr<container::geometry::OpenVdbSequencePlayback> vdbPlayback_;
  std::optional<size_t> vdbLoadedFrame_{};
  std::optional<size_t> vdbUploadingFrame_{};
  uint64_t vdbUploadSubmitted_{0}, vdbUploadPublished_{0}, vdbUploadDiscarded_{0};
  void updateOpenVdbSequence();
  struct ModelLoadState;
  std::unique_ptr<ModelLoadState> modelLoad_;
  struct SceneMergeHistory;
  struct SceneMergeBudget;
  std::shared_ptr<SceneMergeBudget> mergeBudget_;
  enum class MergeRequestOutcome { Idle, Pending, Committed, Failed };
  struct MergeRequestState {
    uint64_t requestId{}, sourceReadCount{}, publications{};
    MergeRequestOutcome outcome{MergeRequestOutcome::Idle};
    std::string error, parentName;
  } mergeRequest_;
  std::vector<std::weak_ptr<SceneMergeHistory>> mergeReceipts_;
  std::weak_ptr<SceneMergeHistory> lastCommittedMerge_;
  void appendGltfMergeTelemetry(nlohmann::json& json) const;
  bool mergeSceneModel(const std::string& path,
      const container::geometry::gltf::GltfMergeOptions& options,
      const container::geometry::GltfLoadResult* prepared = nullptr);
  bool duplicateSceneNode(uint32_t node, std::optional<std::string> name = std::nullopt);
  uint32_t addScenePrimitive(ScenePrimitiveKind kind, std::string name = {},
      std::optional<uint32_t> parent = std::nullopt);
  bool reparentSceneNode(uint32_t node, std::optional<uint32_t> parent);
  container::util::SceneEditHistory::ReparentActions prepareSceneReparent(
      uint32_t node, const container::util::SceneEditHistory::Placement& before,
      const container::util::SceneEditHistory::Placement& after);
  bool commitSceneAddition(std::shared_ptr<SceneMergeHistory> state,
      std::string label, uint64_t beforeBytes, uint64_t usedBytes,
      std::optional<uint32_t> selection, bool imported);
  bool replaySceneMerge(SceneMergeHistory& state, bool redo);
  bool publishSceneMerge(SceneMergeHistory& state,
      container::scene::SceneGraph& candidate,
      container::scene::SceneGraph::PreparedAppend* append,
      container::scene::SceneGraph::PreparedTailChange* tail, bool redo);
  [[nodiscard]] SceneProviderSyncInput buildSceneProviderSyncInput(const MeshSceneProviderSyncInput* preparedMesh = nullptr);
  struct MaterialEditHistory;
  struct PendingMaterialGesture;
  struct MaterialPublication;
  struct MaterialBindingHistory;
  struct MaterialBindingBudget;
  struct MaterialTextureHistory;
  struct MaterialRetirementHistory;
  struct ResourceRetirementHistory;
  struct ResourceCatalogPreview;
  std::shared_ptr<ResourceCatalogPreview> resourceCatalogPreview_;
  std::shared_ptr<PendingMaterialGesture> materialGesture_;
  std::shared_ptr<MaterialBindingBudget> materialBindingBudget_;
  uint64_t materialPublications_{};
  int32_t lastEditedSourceMaterial_{-1};
  uint32_t materialInspectorBinding_{UINT32_MAX};
  uint32_t materialCatalogRuntime_{UINT32_MAX};
  uint64_t materialCatalogAsset_{};
  std::string materialCatalogError_;
  std::optional<container::ui::ResourceCatalogTarget> resourceCatalogSelection_;
  std::string resourceCatalogError_;
  std::string materialTextureError_;
  int32_t materialTextureErrorSource_{-1};
  uint64_t materialTextureErrorAsset_{};
  bool authorMaterial(const container::material::GltfMaterialEditRequest& request);
  bool authorMaterialTexture(const container::material::GltfMaterialTextureEditRequest& request);
  bool replayMaterialTexture(const MaterialTextureHistory&,bool forward);
  bool authorMaterialRetirement(uint32_t runtimeMaterial,int32_t sourceMaterial,
      std::optional<uint64_t> expectedAuthoredRevision=std::nullopt);
  bool replayMaterialRetirement(const MaterialRetirementHistory&,bool retired);
  bool authorResourceRetirement(const container::ui::ResourceRetirementRequest&);
  bool replayResourceRetirement(const ResourceRetirementHistory&,bool retired);
  void refreshResourceCatalog();
  bool publishMaterialEdit(const container::geometry::gltf::GltfMaterialAuthoringPlan&);
  bool publishMaterialTransaction(MaterialPublication&);
  bool authorMaterialBinding(const container::material::GltfMaterialBindingRequest&);
  bool replayMaterialBinding(const MaterialBindingHistory&, bool redo);
  bool replayMaterialEdit(const MaterialEditHistory&,container::geometry::gltf::GltfMaterialEditSide);
  void finishMaterialGesture();
  void refreshMaterialInspector();
  void processMaterialEditRequests();
  void appendMaterialAuthoringTelemetry(nlohmann::json&) const;
  void appendMaterialTextureTelemetry(nlohmann::json&) const;
  void pumpModelLoadProgress(std::string_view stage, bool render = true);
  void drawModelLoadProgressFrame();
  container::capture::Journal gfxJournal_;
  [[nodiscard]] nlohmann::json captureTelemetry() const;
  std::optional<float> captureExposure_{};
  std::optional<UpscaleSelection> captureUpscaling_{};
  struct UpscaleResourceRequest {
    UpscaleSelection selection{};
    bool eligible{};
    bool infiniteFar{};
    bool operator==(const UpscaleResourceRequest&) const = default;
  };
  UpscaleResourceRequest upscaleResourceRequest_{};
  std::string upscaleViewportStatus_{};
  float temporalFrameMilliseconds_{1000.0f / 60.0f};
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
  std::optional<std::pair<uint64_t, uint32_t>> captureNodeGizmoTarget_;
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
    std::unique_ptr<GpuHierarchyPass> gpuHierarchyPass;
    std::unique_ptr<RayInstanceTransformPass> gpuRayInstancePass;
    std::unique_ptr<PbrAuxiliaryPass> pbrAuxiliaryPass;
    std::unique_ptr<ScreenReflectionPass> screenReflectionPass;
    std::unique_ptr<VolumetricFogPass> volumetricFogPass;
    std::unique_ptr<CameraController> cameraController;
    std::unique_ptr<LightingManager> lightingManager;
    std::unique_ptr<ShadowCullManager> shadowCullManager;
    std::unique_ptr<ShadowManager> shadowManager;
    std::unique_ptr<EnvironmentManager> environmentManager;
    std::unique_ptr<GpuCullManager> gpuCullManager;
    std::unique_ptr<BloomManager> bloomManager;
    std::unique_ptr<OpenVdbVolumeRenderer> openVdbVolumeRenderer;
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
#ifdef CONTAINER_ENABLE_RENDERER_MCP
    std::unique_ptr<RendererMcpServer> mcpServer;
#endif
    std::unique_ptr<RendererTelemetry> rendererTelemetry;
    std::unique_ptr<GpuHardwareMonitor> gpuHardwareMonitor;
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

  bool referencePathActive() const;
  [[nodiscard]] ReferencePathDisplayMode referencePathDisplay() const;
  void ensureReferencePathResources();
  bool recordReferencePathTrace(VkCommandBuffer, const FrameRecordParams&);
  void finishReferencePathTrace(VkCommandBuffer);
  void collectReferenceDiagnostics(uint32_t imageIndex);
  void updateReferenceSampleBudget(float lightingMs);
  void updateReferenceColorSource(uint32_t imageIndex, bool sceneLinear);
  void publishReferenceColorBinding(uint32_t imageIndex);
  std::vector<bool> referenceColorBound_;
  std::vector<uint64_t> referenceDiagnosticSeen_;
  uint64_t referenceDiagnosticEpoch_{}, referenceUnsupported_{}, referenceRejected_{};
  bool referenceRecorded_{};
  uint64_t referenceTimedSubmission_{};
  std::optional<uint32_t> referenceAdaptiveBatch_;
  std::optional<float> referenceTimedLightingMs_;
  std::optional<float> referenceFocusDistanceOverride_;
  std::optional<ReferenceOidnQuality> referenceOidnQualityOverride_;
  std::optional<bool> referenceOptixOverride_;
  std::optional<ReferencePathDisplayMode> referencePathDisplayOverride_;
  std::optional<UpscaleMode> referenceDlssOverride_;
  bool referenceAutofocusRequested_{};
  std::string referenceAutofocusStatus_;
  std::string referenceStatus_;

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
  std::unique_ptr<container::util::SceneEditHistory> sceneEdits_{};
  std::unique_ptr<container::geometry::gltf::AnimationPlayer> gltfAnimation_{};
  std::unique_ptr<container::geometry::gltf::GltfInteractivity> gltfInteractivity_;
  std::string gltfInteractivityError_;
  std::vector<size_t> gltfInteractivityHoverChain_;
  struct GltfInteractivityRequest {
    std::optional<bool> playing;
    bool reset{};
    std::optional<std::string> event;
  } gltfInteractivityRequest_;
  double pendingGltfAnimationDeltaSeconds_{};
  uint64_t gltfAnimationGraphIdentity_{};
  std::optional<size_t> gltfAnimationCameraNode_{};
  std::optional<container::geometry::gltf::AnimatedCameraState> gltfAnimationCamera_{};
  bool gltfCameraChoicesDirty_{};
  bool sceneEditPublicationPending_{false};
  uint64_t materialVariantGraphIdentity_{};
  std::optional<int> captureMaterialVariant_{};
  std::string captureSceneHistory_{};
  uint64_t gltfSceneGraphIdentity_{};
  struct CaptureGltfSceneRequest {
    size_t index;
    bool expectRejection;
  };
  std::optional<CaptureGltfSceneRequest> captureGltfScene_;
  std::string gltfSceneSelectionError_;
  std::map<size_t,std::shared_ptr<SceneLightEdits>> gltfSceneLightEdits_{};
  std::shared_ptr<ImportedSceneLightEdits> gltfImportedLightEdits_{};
  void updateGltfSceneState();
  void processGltfSceneRequest();
  void selectGltfScene(size_t index, bool expectRejection = false);
  void captureImportedLightEdits();
  void refreshGltfCameraChoices();
  static void setGltfCameraChoices(const container::scene::SceneGraph&, const tinygltf::Model&,
      container::ui::GltfAnimationControls&);
  void updateMaterialVariantState();
  void processMaterialVariantRequest();
  uint64_t nextViewportEditGesture_{uint64_t{1} << 63};
  void ensureSceneEditHistory();
  void processSceneNodeAuthoringRequest();
  bool authorSceneNode(const container::ui::SceneNodeAuthoringRequest& request);
  bool retireSceneNode(uint32_t selected);
  struct SceneRetirementHistory;
  struct SceneRetirementBudget;
  std::shared_ptr<SceneRetirementBudget> sceneRetirementBudget_;
  bool replaySceneRetirement(SceneRetirementHistory& state, bool retired);
  void publishSceneRetirementRefresh();
  std::shared_ptr<SceneRetirementHistory> pendingSceneRetirement_;
  void resetGltfAnimation(bool applyStartupOptions = false);
  void updateGltfAnimation();
  void publishGltfAnimationChanges(const container::geometry::gltf::AnimationDirtySet&);
  void resetGltfInteractivity();
  void pauseGltfInteractivity();
  container::geometry::gltf::AnimationDirtySet updateGltfInteractivity(double delta, bool& discontinuity);
  std::vector<size_t> gltfInteractivityAncestors(uint32_t node) const;
  void queueGltfInteractivitySelection(uint32_t node, std::optional<glm::vec3> point);
  void updateGltfInteractivityHover(uint32_t node);
  void appendGltfInteractivityTelemetry(nlohmann::json&) const;
  struct CameraProjectionHistory;
  struct CameraProjectionHistoryBudget;
  struct CameraProjectionInspectorCache;
  std::shared_ptr<CameraProjectionHistoryBudget> cameraProjectionHistoryBudget_;
  std::shared_ptr<CameraProjectionInspectorCache> cameraProjectionInspectorCache_;
  uint64_t cameraProjectionPublications_{};
  [[nodiscard]] uint32_t cameraGraphNode(size_t sourceNode) const;
  [[nodiscard]] size_t cameraNodeByName(std::string_view) const;
  bool setGltfCameraNode(std::optional<size_t>);
  bool authorCameraProjection(const container::scene::CameraProjectionTarget&,
      const container::geometry::gltf::GltfCameraProjectionPatch&);
  bool authorCameraProjectionSource(int32_t,const container::geometry::gltf::GltfCameraProjectionPatch&,
      std::optional<uint64_t> expectedRevision);
  bool replayCameraProjection(const CameraProjectionHistory&,container::geometry::gltf::GltfCameraEditSide);
  void refreshCameraProjectionInspector();
  void processCameraProjectionRequests();
  void appendCameraProjectionTelemetry(nlohmann::json&) const;
  struct CameraViewPoseHistory;
  struct CameraViewPoseHistoryBudget;
  struct CameraViewPoseViewState;
  struct CameraViewPosePublication;
  std::shared_ptr<CameraViewPoseHistoryBudget> cameraViewPoseHistoryBudget_;
  std::shared_ptr<CameraViewPoseViewState> cameraViewPoseViewState_;
  std::optional<container::scene::CameraViewPoseTarget> cameraViewPoseDestination_;
  uint64_t cameraViewPosePublications_{},cameraViewPoseViewRevision_{};
  [[nodiscard]] glm::mat4 cameraViewportUnjitteredView() const;
  [[nodiscard]] uint64_t refreshCameraViewPoseViewRevision();
  void updateCameraViewPoseDestination();
  void refreshCameraViewPoseInspector();
  void processCameraViewPoseRequests();
  bool authorCameraViewPose(const container::scene::CameraViewPoseTarget&,uint64_t expectedViewRevision);
  bool authorCameraViewPoseSource(int32_t,std::optional<uint64_t> expectedCameraRevision);
  bool replayCameraViewPose(const CameraViewPoseHistory&,container::geometry::gltf::GltfCameraEditSide);
  bool publishCameraViewPose(CameraViewPosePublication&,const glm::mat4* currentView);
  void appendCameraViewPoseTelemetry(nlohmann::json&) const;
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
  void finishEditGestures();
  void finishSceneTransformGesture(bool cancel = false);
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
  [[nodiscard]] EditableLightId resolveCaptureLightTarget(const container::temporal::CaptureLightTarget&) const;
  bool applyGltfAnimationCamera(uint32_t imageIndex);
  void updateGltfDeformation();
  void prepareGltfDeformationFrame(uint32_t imageIndex);
  void commitGltfDeformationFrame();
  uint64_t gltfDeformationGraphIdentity_{};
  uint64_t gltfDeformationSourceRevision_{};
  uint64_t gltfDeformationTopologyRevision_{};
  size_t gltfDeformationVertexCount_{};
  uint32_t gltfDeformationImageCount_{};
  std::optional<uint32_t> gltfDeformationSubmittedEpoch_{};
  void publishSceneEdit();
  void refreshSceneEditView();
  void applyEditHistoryRequest();
  void validateSceneNodeTransform(uint32_t node, const glm::mat4 &proposed);
  void requireEditableSceneNode(uint32_t node) const;
  void validateSceneNodeReparent(uint32_t node, std::optional<uint32_t> parent,
                                 const glm::mat4 &local);
  struct SceneEditSelection {
    uint32_t root, mesh, bim;
    EditableLightId light;
    bool sectionEditable;
    std::shared_ptr<const container::scene::AuthoredLightBinding> lightBinding;
  };
  [[nodiscard]] SceneEditSelection sceneEditSelection() const;
  void restoreSceneEditSelection(const SceneEditSelection &selection);
  struct VisibilityLightSelection {
    EditableLightId light;
    std::shared_ptr<const container::scene::AuthoredLightBinding> binding;
    uint32_t node;
    uint64_t nodeIncarnation, graphIdentity, assetIdentity;
    size_t scene;
  };
  [[nodiscard]] std::optional<VisibilityLightSelection> selectedLightUnder(uint32_t ancestor) const;
  [[nodiscard]] bool sceneEditSelectionVacant() const;
  std::optional<VisibilityLightSelection> pendingVisibilityLightSelection_;
#if CONTAINER_HAS_JOLT
  std::unique_ptr<container::physics::PhysicsScene> physicsScene_;
  std::unique_ptr<container::physics::PhysicsElementBindings> physicsElements_;
#endif
  struct PhysicsCaptureAction {
    std::optional<uint32_t> primaryNode;
    uint32_t bimObject{std::numeric_limits<uint32_t>::max()};
    std::string attach, action;
    std::shared_ptr<const container::temporal::CapturePhysicsJointCreate> jointCreate;
    std::string jointAction;
    std::optional<float> jointMotorVelocity;
    std::shared_ptr<const container::physics::PhysicsBodySettings> bodySettings;
  };
  std::vector<PhysicsCaptureAction> pendingPhysicsActions_;
  uint64_t physicsBimPublication_{0};
  glm::vec3 physicsBimRootTranslation_{0.0f};
  double pendingPhysicsDeltaSeconds_{0.0};
  PhysicsDemoNodes physicsDemoNodes_{};
  std::string physicsError_;
  std::optional<uint32_t> physicsJointFirstNode_;
  uint64_t physicsJointFirstBody_{0};
  uint64_t physicsJointFirstGraph_{0};
  bool physicsJointFirstIsWorld_{false};
  uint64_t physicsSelectedJoint_{0};
  void presentPhysicsControls();
  void advancePhysics();
  void pausePhysicsForTransformEdit(uint32_t node);
  [[nodiscard]] bool applySceneNodeTransform(
      uint32_t node, const container::ui::TransformControls &controls);
  void clearPhysicsBindings();
  void createPhysicsDemo();
  void attachPhysicsBody(uint32_t node, const container::physics::PhysicsBodySettings &settings);
  void attachBimPhysicsBody(uint32_t object, const container::physics::PhysicsBodySettings &settings);
  void synchronizeBimPhysicsBindings();
  void publishBimPhysicsTransforms();
  void performPhysicsAction(std::string_view action, std::optional<uint32_t> primaryNode,
                            uint32_t bimObject);
  void appendPhysicsTelemetry(nlohmann::json &json) const;
  void appendGltfAnimationTelemetry(nlohmann::json &json) const;
  void appendGltfDeformationTelemetry(nlohmann::json &json) const;
  float gltfDeformationMaxPositionDelta_{};
  bool gltfDeformationHistoryReset_{true};
  bool gltfDeformationCurrentBoundToGpu_{};
  bool gltfDeformationPreviousBoundToGpu_{};
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
    uint64_t sceneGraphRevision{0};
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
  container::app::SceneLightingOverrides sceneLightingOverrides_{};
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
    std::optional<uint32_t> referenceSampleTarget{};
    bool sceneLinear{false};
    bool viewportOnly{false};
    bool interactive{false};
    bool remote{false};
    bool pending{false};
    std::vector<HostReadbackSlot> readbacks{};
  };
  ScreenshotState screenshot_{};
  std::unique_ptr<ComfyUiFrontendState> comfyUi_;
  std::string mcpCaptureStatus_;
  std::string mcpModelStatus_;
  uint32_t referenceSeriesSamples_{};

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
    bool pickIdValid{false};
    uint32_t selectedBimObjectIndex{std::numeric_limits<uint32_t>::max()};
    VkFence renderFence{VK_NULL_HANDLE};
    bool valid{false};
  };

  struct DepthVisibilityState {
    std::vector<DepthVisibilityFrameSlot> slots{};
    uint32_t latestFrameSlot{0};
  };
  DepthVisibilityState depthVisibility_{};

  struct TransformDragSession {
    bool active{false};
    uint64_t historyGesture{0};
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
  PipelineBuildResult buildGraphicsPipelines(const std::filesystem::path& shaderDirectory);
  void destroyGraphicsPipelines();
  void processShaderReload(bool request, bool wait = false);
  std::future<std::shared_ptr<ShaderReloadCandidate>> shaderReloadJob_;
  std::stop_source shaderReloadStop_;
  std::shared_ptr<ShaderReloadCandidate> activeShaderReload_;
  uint64_t shaderReloadGeneration_{};
  bool gpuHierarchyEnabled_{false};
  bool gpuHierarchyPrepared_{false};
  uint32_t gpuHierarchyImage_{};
  std::string gpuHierarchyStatus_{"CPU"};
  std::vector<VkBuffer> gpuHierarchyObjectBuffers_;
  bool gpuObjectUploadDeferred_{false};
  uint64_t cpuObjectUploadBytes_{};
  std::string shaderReloadStatus_;
  void recreateMsaaResources(VkSampleCountFlagBits sampleCount);
  void createCamera();
  void resetCameraForActiveScene();
  void applySceneLightingDefaults();
  void applyLightingOverrides();
  void syncCameraSelectionPivotOverride();
  void initializeScene();
  void buildSceneGraph();
  void createSceneBuffers();
  void createGeometryBuffers();
  void createFrameResources();
  [[nodiscard]] VkExtent2D sceneRenderExtent() const;
  [[nodiscard]] UpscaleResourceRequest currentUpscaleResourceRequest() const;
  void reconcileUpscaleResources();
  void ensureCameraBuffers();
  void ensureObjectBuffers();
  void syncSceneProviders();

  // ---- per-frame helpers
  // ------------------------------------------------------
  void updateCameraBuffer(uint32_t imageIndex);
  void refreshSceneObjectData();
  void updateObjectBuffer(uint32_t imageIndex, bool deferUploadToGpu = false);
  void prepareGpuHierarchyFrame(uint32_t imageIndex);
  void uploadPreparedObjectData(uint32_t imageIndex,std::span<const container::gpu::ObjectData>);
  void updateAllObjectBuffers();
  [[nodiscard]] container::gpu::AllocatedBuffer sceneObjectBuffer(
      uint32_t imageIndex) const;
  [[nodiscard]] size_t sceneObjectCapacity(uint32_t imageIndex) const;
  [[nodiscard]] size_t maxSceneObjectCapacity() const;
  void applyBimSemanticColorMode();
  void processEnvironmentLoad(bool wait = false);
#ifdef CONTAINER_ENABLE_RENDERER_MCP
  void processMcpRequests();
#endif
  void initializeComfyUi(bool scripted = false);
  void processComfyUi();
  void drawComfyUi();
  void retireComfyUiPreview();
  void shutdownComfyUi();
  bool finishComfyUiCapture(std::string error = {});
  bool applyComfyUiEnvironment();
  void applyComfyUiCaptureAction(const container::temporal::CaptureComfyUi& action);
  [[nodiscard]] nlohmann::json comfyUiCaptureTelemetry() const;
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
  void ensureDepthVisibilityReadbackBuffer(uint32_t frameSlot);
  void markDepthVisibilityFrameComplete(uint32_t imageIndex,
                                        uint32_t frameSlot,
                                        bool pickIdWritten);
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
  void ensureExtendedPbrResources();
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
