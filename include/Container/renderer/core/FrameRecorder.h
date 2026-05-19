#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/core/RenderExtraction.h"
#include "Container/renderer/core/RenderGraph.h"
#include "Container/renderer/core/RenderTechnique.h"
#include "Container/renderer/debug/DebugOverlayPushConstants.h"
#include "Container/renderer/scene/DrawCommand.h"
#include "Container/renderer/scene/TransformGizmoState.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string_view>
#include <vector>

// Forward declarations — full headers only needed in FrameRecorder.cpp.
namespace container::renderer {
class BloomManager;
class BimManager;
class GpuCullManager;
class FrameResourceRegistry;
class PipelineRegistry;
class RenderPassGpuProfiler;
class RendererTelemetry;
class ShadowCullManager;
class ShadowManager;
struct FrameBufferBinding;
struct FrameDescriptorBinding;
struct FrameFramebufferBinding;
struct FrameImageBinding;
struct BimSectionCapDrawStyle;
struct BimSectionMarkerLine;
struct FrameResourceBinding;
struct FrameSamplerBinding;
struct LightPushConstants;
} // namespace container::renderer

namespace container::renderer {

struct FrameRecordParams;

struct FrameRecordLifecycleHooks {
  std::function<void(const FrameRecordParams &)> beforePrepareFrame{};
  std::function<void(const FrameRecordParams &, const RenderGraph &)>
      afterPrepareFrame{};
  std::function<void(VkCommandBuffer, const FrameRecordParams &)>
      afterCommandBufferBegin{};
  std::function<void(VkCommandBuffer, const FrameRecordParams &)>
      afterGraphExecution{};
};

struct FrameRuntimeResources {
  uint32_t frameSlot{0};
  uint32_t imageIndex{0};
};

struct FrameSceneGeometry {
  container::gpu::BufferSlice vertexSlice{};
  container::gpu::BufferSlice indexSlice{};
  VkIndexType indexType{VK_INDEX_TYPE_UINT32};
  const std::vector<container::gpu::ObjectData> *objectData{nullptr};
  uint64_t objectDataRevision{0};
  VkBuffer objectBuffer{VK_NULL_HANDLE};
  VkDeviceSize objectBufferSize{0};
  uint32_t diagCubeObjectIndex{std::numeric_limits<uint32_t>::max()};
};

struct FrameDrawLists {
  const std::vector<DrawCommand> *opaqueDrawCommands{nullptr};
  const std::vector<DrawCommand> *transparentDrawCommands{nullptr};
  // Split draw lists let the recorder route fast GPU-driven single-sided draws
  // separately from mirrored-winding and no-cull/double-sided material paths.
  const std::vector<DrawCommand> *opaqueSingleSidedDrawCommands{nullptr};
  const std::vector<DrawCommand> *opaqueWindingFlippedDrawCommands{nullptr};
  const std::vector<DrawCommand> *opaqueDoubleSidedDrawCommands{nullptr};
  const std::vector<DrawCommand> *transparentSingleSidedDrawCommands{nullptr};
  const std::vector<DrawCommand> *transparentWindingFlippedDrawCommands{
      nullptr};
  const std::vector<DrawCommand> *transparentDoubleSidedDrawCommands{nullptr};
  const std::vector<DrawCommand> *hoveredDrawCommands{nullptr};
  const std::vector<DrawCommand> *selectedDrawCommands{nullptr};
};

struct FrameBimFloorPlanOverlayState {
  bool enabled{false};
  bool depthTest{false};
  glm::vec3 color{0.02f, 0.02f, 0.02f};
  float opacity{0.85f};
  float lineWidth{1.0f};
};

struct FrameBimPointStyleState {
  bool enabled{true};
  bool depthTest{true};
  glm::vec3 color{0.0f, 0.72f, 1.0f};
  float opacity{0.7f};
  float pointSize{3.0f};
};

struct FrameBimCurveStyleState {
  bool enabled{true};
  bool depthTest{true};
  glm::vec3 color{1.0f, 0.72f, 0.0f};
  float opacity{0.75f};
  float lineWidth{2.0f};
};

struct FrameBimPointCurveStyleState {
  FrameBimPointStyleState points{};
  FrameBimCurveStyleState curves{};
};

struct FrameBimPointCloudPrimitivePassState {
  bool enabled{false};
  bool depthTest{true};
  // Placeholder preview is kept as an explicit fallback for old loader paths
  // that do not provide native point-list ranges.
  bool placeholderRangePreviewEnabled{false};
  glm::vec3 color{0.0f, 0.72f, 1.0f};
  float opacity{0.85f};
  float pointSize{3.0f};
};

struct FrameBimCurvePrimitivePassState {
  bool enabled{false};
  bool depthTest{true};
  // Placeholder preview is kept as an explicit fallback for old loader paths
  // that do not provide native line-list ranges.
  bool placeholderRangePreviewEnabled{false};
  glm::vec3 color{1.0f, 0.72f, 0.0f};
  float opacity{0.85f};
  float lineWidth{2.0f};
};

struct FrameBimPrimitivePassState {
  FrameBimPointCloudPrimitivePassState pointCloud{};
  FrameBimCurvePrimitivePassState curves{};
};

enum class FrameSectionClipCapHatchMode : uint32_t {
  None = 0,
  Diagonal = 1,
  Cross = 2,
};

struct FrameSectionBoxClipState {
  bool enabled{false};
  bool invert{false};
  uint32_t planeCount{0};
  std::array<glm::vec4, 6> planes{};
};

struct FrameSectionClipCapStyleState {
  bool enabled{false};
  bool fillEnabled{true};
  bool hatchEnabled{true};
  FrameSectionClipCapHatchMode hatchMode{
      FrameSectionClipCapHatchMode::Diagonal};
  glm::vec4 fillColor{0.06f, 0.08f, 0.10f, 0.82f};
  glm::vec4 hatchColor{0.85f, 0.72f, 0.32f, 0.95f};
  float hatchSpacing{0.25f};
  float hatchLineWidth{1.0f};
  float hatchAngleRadians{0.7853982f};
  float capOffset{0.0f};
  FrameSectionBoxClipState boxClip{};
};

struct FrameSectionClipCapGeometry {
  FrameSceneGeometry scene{};
  const std::vector<DrawCommand> *fillDrawCommands{nullptr};
  const std::vector<DrawCommand> *hatchDrawCommands{nullptr};
  const std::vector<BimSectionCapDrawStyle> *fillDrawStyles{nullptr};
  const std::vector<BimSectionCapDrawStyle> *hatchDrawStyles{nullptr};
  const std::vector<BimSectionMarkerLine> *sectionMarkerLines{nullptr};

  [[nodiscard]] bool valid() const {
    return scene.vertexSlice.buffer != VK_NULL_HANDLE &&
           scene.indexSlice.buffer != VK_NULL_HANDLE &&
           ((fillDrawCommands != nullptr && !fillDrawCommands->empty()) ||
            (hatchDrawCommands != nullptr && !hatchDrawCommands->empty()));
  }
};

struct FrameBimTechnicalElevationState {
  bool enabled{false};
  // Hidden-line elevation is a depth-tested wire overlay on the normal shaded
  // pass, so occluded BIM edges stay hidden by the filled depth buffer.
  bool hiddenLineOverlay{true};
  bool depthTestLines{true};
  bool sectionCapsEnabled{true};
  bool capFillEnabled{true};
  bool capHatchingEnabled{true};
  glm::vec3 lineColor{0.02f, 0.025f, 0.03f};
  float lineWidth{1.25f};
  float overlayIntensity{0.95f};
};

struct FrameBimCoordinationMarkerState {
  bool issueMarkersEnabled{false};
  bool clashMarkersEnabled{false};
  bool depthTest{true};
  glm::vec3 issueColor{0.12f, 0.48f, 1.0f};
  glm::vec3 clashColor{1.0f, 0.16f, 0.05f};
  float issueOpacity{0.9f};
  float clashOpacity{1.0f};
  float issueLineWidth{3.0f};
  float clashLineWidth{4.0f};
};

struct FrameBimSectionPlaneVisualState {
  bool enabled{false};
  bool depthTest{true};
  glm::vec3 color{0.12f, 0.62f, 1.0f};
  float opacity{0.55f};
  float lineWidth{2.0f};
};

struct FrameBimResources {
  // BIM models use a separate render path so semantic overlays, selection, and
  // future IFC metadata can evolve without perturbing regular glTF draws.
  FrameSceneGeometry scene{};
  FrameDrawLists draws{};
  FrameDrawLists pointDraws{};
  FrameDrawLists curveDraws{};
  FrameDrawLists nativePointDraws{};
  FrameDrawLists nativeCurveDraws{};
  FrameSceneGeometry coordinationMarkerScene{};
  FrameSceneGeometry sectionPlaneVisualScene{};
  const std::vector<DrawCommand> *coordinationIssueMarkerDrawCommands{nullptr};
  const std::vector<DrawCommand> *coordinationClashMarkerDrawCommands{nullptr};
  const std::vector<DrawCommand> *sectionPlaneVisualDrawCommands{nullptr};
  const std::vector<DrawCommand> *floorPlanDrawCommands{nullptr};
  FrameBimFloorPlanOverlayState floorPlan{};
  FrameBimPointCurveStyleState pointCurveStyle{};
  FrameBimCoordinationMarkerState coordinationMarkers{};
  FrameBimSectionPlaneVisualState sectionPlaneVisual{};
  FrameBimPrimitivePassState primitivePasses{};
  FrameSectionClipCapStyleState sectionClipCaps{};
  FrameSectionClipCapGeometry sectionClipCapGeometry{};
  FrameBimTechnicalElevationState technicalElevation{};
  uint32_t semanticColorMode{0};
  bool opaqueMeshDrawsUseGpuVisibility{false};
  bool transparentMeshDrawsUseGpuVisibility{false};
  bool nativePrimitiveDrawsUseGpuVisibility{false};
  bool nativePointDrawsUseGpuVisibility{false};
  bool nativeCurveDrawsUseGpuVisibility{false};
};

struct FrameCameraResources {
  // Camera plane data shared by passes that derive depth-space values.
  float nearPlane{0.1f};
  float farPlane{100.0f};
  bool orthographic{false};
};

struct FrameRegistryState {
  // Technique contracts describe what a technique can request. Runtime bindings
  // expose the actual per-frame Vulkan resources and pipeline handles.
  const FrameResourceRegistry *resourceContracts{nullptr};
  const FrameResourceRegistry *resourceBindings{nullptr};
  const PipelineRegistry *pipelineRecipes{nullptr};
  const PipelineRegistry *pipelineHandles{nullptr};
  const PipelineRegistry *pipelineLayouts{nullptr};
};

struct FrameDebugState {
  bool debugDirectionalOnly{false};
  bool debugVisualizePointLightStencil{false};
  bool debugFreezeCulling{false};
  bool wireframeRasterModeSupported{false};
  bool wireframeWideLinesSupported{false};
};

struct FramePushConstantState {
  container::gpu::BindlessPushConstants *bindless{nullptr};
  LightPushConstants *light{nullptr};
  WireframePushConstants *wireframe{nullptr};
  NormalValidationPushConstants *normalValidation{nullptr};
  SurfaceNormalPushConstants *surfaceNormal{nullptr};
  TransformGizmoPushConstants *transformGizmo{nullptr};
};

struct FrameShadowResources {
  // Shadow cascade framebuffers. GPU-cull buffers are owned by
  // ShadowCullManager, which keeps the frame parameter contract to high-level
  // pass services.
  VkRenderPass renderPass{VK_NULL_HANDLE};
  const VkFramebuffer *shadowFramebuffers{nullptr};
  const VkFramebuffer *localShadowFramebuffers{nullptr};
  const container::gpu::ShadowData *shadowData{nullptr};
  const container::gpu::LocalShadowData *localShadowData{nullptr};
  container::gpu::ShadowSettings shadowSettings{};
  uint32_t localShadowLayerCount{0};
  bool useGpuShadowCull{false};
  ShadowCullManager *shadowCullManager{nullptr};
  const ShadowManager *shadowManager{nullptr};
  // One secondary command buffer per shadow cascade, allocated from separate
  // worker command pools. When unavailable, shadows fall back to inline primary
  // command recording.
  bool useShadowSecondaryCommandBuffers{false};
  std::array<VkCommandBuffer, container::gpu::kShadowCascadeCount>
      shadowSecondaryCommandBuffers{};
};

struct FrameSwapchainResources {
  const std::vector<VkFramebuffer> *swapChainFramebuffers{nullptr};
};

struct FrameScreenshotCapture {
  bool enabled{false};
  VkImage swapChainImage{VK_NULL_HANDLE};
  VkBuffer readbackBuffer{VK_NULL_HANDLE};
  VkExtent2D extent{};
};

struct FramePassServices {
  GpuCullManager *gpuCullManager{nullptr};
  BimManager *bimManager{nullptr};
  BloomManager *bloomManager{nullptr};
  RendererTelemetry *telemetry{nullptr};
  RenderPassGpuProfiler *gpuProfiler{nullptr};
};

struct FramePostProcessState {
  VkRenderPass renderPass{VK_NULL_HANDLE};
  container::gpu::ExposureSettings exposureSettings{};
};

// All per-frame render state needed by FrameRecorder::record().
struct FrameRecordParams {
  FrameRuntimeResources runtime{};
  FrameSceneGeometry scene{};
  FrameBimResources bim{};
  FrameDrawLists draws{};
  FrameCameraResources camera{};
  FrameRegistryState registries{};
  FrameDebugState debug{};
  FramePushConstantState pushConstants{};
  FrameTransformGizmoState transformGizmo{};
  FrameShadowResources shadows{};
  FrameSwapchainResources swapchain{};
  FrameScreenshotCapture screenshot{};
  FramePassServices services{};
  FramePostProcessState postProcess{};
  ProviderSceneExtraction sceneExtraction{};
  FrameRecordLifecycleHooks lifecycle{};

  [[nodiscard]] const FrameResourceBinding *resourceBinding(
      RenderTechniqueId technique, std::string_view name) const;
  [[nodiscard]] const FrameImageBinding *imageBinding(
      RenderTechniqueId technique, std::string_view name) const;
  [[nodiscard]] const FrameBufferBinding *bufferBinding(
      RenderTechniqueId technique, std::string_view name) const;
  [[nodiscard]] const FrameFramebufferBinding *framebufferBinding(
      RenderTechniqueId technique, std::string_view name) const;
  [[nodiscard]] const FrameDescriptorBinding *descriptorBinding(
      RenderTechniqueId technique, std::string_view name) const;
  [[nodiscard]] const FrameSamplerBinding *samplerBinding(
      RenderTechniqueId technique, std::string_view name) const;
  [[nodiscard]] VkFramebuffer framebuffer(RenderTechniqueId technique,
                                          std::string_view name) const;
  [[nodiscard]] VkDescriptorSet descriptorSet(RenderTechniqueId technique,
                                              std::string_view name) const;
  [[nodiscard]] VkSampler sampler(RenderTechniqueId technique,
                                  std::string_view name) const;
  [[nodiscard]] VkPipeline pipelineHandle(RenderTechniqueId technique,
                                          std::string_view name) const;
  [[nodiscard]] VkPipelineLayout pipelineLayout(RenderTechniqueId technique,
                                                std::string_view name) const;
};

// Executes the active technique's prepared render graph into a command buffer.
// Technique-specific setup and teardown arrive through FrameRecordParams
// lifecycle hooks so FrameRecorder stays focused on shared graph execution.
class FrameRecorder {
public:
  FrameRecorder() = default;
  ~FrameRecorder() = default;

  // Records a complete frame into commandBuffer.
  void record(VkCommandBuffer commandBuffer,
              const FrameRecordParams &params) const;

  // Access the graph for external pass toggling or inspection.
  RenderGraph &graph() { return graph_; }
  const RenderGraph &graph() const { return graph_; }
  RenderGraphBuilder graphBuilder() { return RenderGraphBuilder(graph_); }

private:
  RenderGraph graph_;
};

} // namespace container::renderer
