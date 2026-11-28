#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/bim/BimLightingOverlayPlanner.h"
#include "Container/renderer/debug/DebugOverlayRenderer.h"
#include "Container/utility/VulkanMemoryManager.h"

namespace container::renderer {

struct FrameRecordParams;

inline constexpr char kBimDisplayWireframePipeline[] = "bim-display-wireframe";
inline constexpr char kBimDisplayWireframeFrontCullPipeline[] = "bim-display-wireframe-front-cull";
inline constexpr char kBimNativeOitLayout[] = "bim-native-oit";
inline constexpr char kBimNativePointOitPipeline[] = "bim-native-point-oit";
inline constexpr char kBimNativeCurveOitPipeline[] = "bim-native-curve-oit";
inline constexpr char kBimNativeCurveStrokeOitPipeline[] = "bim-native-curve-stroke-oit";
inline constexpr char kBimFloorPlanOitPipeline[] = "bim-floor-plan-oit";
inline constexpr char kBimSelectionClearSilhouettePipeline[] = "bim-selection-clear-silhouette";
inline constexpr char kBimSelectionExpandSilhouettePipeline[] = "bim-selection-expand-silhouette";
inline constexpr char kBimSelectionDisplayPipeline[] = "bim-selection-display";
inline constexpr char kBimSelectionStrokeImage[] = "bim-selection-stroke";
inline constexpr VkFormat kBimSelectionStrokeFormat = VK_FORMAT_R8_UNORM;
inline constexpr uint32_t kBimSelectionStrokeBinding = 17;

struct BimLightingOverlayGeometryBinding {
  VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
  container::gpu::BufferSlice vertexSlice{};
  container::gpu::BufferSlice indexSlice{};
  VkIndexType indexType{VK_INDEX_TYPE_UINT32};
};

struct BimLightingOverlayPipelineHandles {
  VkPipeline wireframeDepth{VK_NULL_HANDLE};
  VkPipeline wireframeNoDepth{VK_NULL_HANDLE};
  VkPipeline wireframeDepthFrontCull{VK_NULL_HANDLE};
  VkPipeline wireframeNoDepthFrontCull{VK_NULL_HANDLE};
  VkPipeline bimFloorPlanDepth{VK_NULL_HANDLE};
  VkPipeline bimFloorPlanNoDepth{VK_NULL_HANDLE};
  VkPipeline bimPointCloudDepth{VK_NULL_HANDLE};
  VkPipeline bimCurveDepth{VK_NULL_HANDLE};
  VkPipeline bimCurveStrokeDepth{VK_NULL_HANDLE};
  VkPipeline selectionMask{VK_NULL_HANDLE};
  VkPipeline selectionOutline{VK_NULL_HANDLE};
};

struct BimLightingOverlayRecordInputs {
  const BimLightingOverlayPlan *plan{nullptr};
  BimLightingOverlayPipelineHandles pipelines{};
  VkPipelineLayout wireframeLayout{VK_NULL_HANDLE};
  BimLightingOverlayGeometryBinding scene{};
  BimLightingOverlayGeometryBinding bim{};
  BimLightingOverlayGeometryBinding coordinationMarkers{};
  BimLightingOverlayGeometryBinding sectionPlaneVisual{};
  VkClearAttachment selectionStencilClearAttachment{};
  VkClearRect selectionStencilClearRect{};
  const WireframePushConstants *wireframePushConstants{nullptr};
  const DebugOverlayRenderer *debugOverlay{nullptr};
  bool wireframeRasterModeSupported{false};
  bool wireframeWideLinesSupported{false};
  bool lineListWideLinesSupported{false};
};

struct BimLightingOverlayFrameStyleState {
  bool enabled{false};
  bool depthTest{true};
  glm::vec3 color{1.0f};
  float opacity{1.0f};
  float lineWidth{1.0f};
};

struct BimLightingOverlayFrameDrawSources {
  BimLightingOverlayDrawLists points{};
  BimLightingOverlayDrawLists curves{};
  const std::vector<DrawCommand> *floorPlan{nullptr};
  const std::vector<DrawCommand> *sceneHover{nullptr};
  const std::vector<DrawCommand> *bimHover{nullptr};
  const std::vector<DrawCommand> *sceneSelection{nullptr};
  const std::vector<DrawCommand> *bimSelection{nullptr};
  const std::vector<DrawCommand> *nativePointHover{nullptr};
  const std::vector<DrawCommand> *nativeCurveHover{nullptr};
  const std::vector<DrawCommand> *nativePointSelection{nullptr};
  const std::vector<DrawCommand> *nativeCurveSelection{nullptr};
  const std::vector<DrawCommand> *nativeCurveStrokeHover{nullptr};
  const std::vector<DrawCommand> *nativeCurveStrokeSelection{nullptr};
  const std::vector<DrawCommand> *coordinationIssueMarkers{nullptr};
  const std::vector<DrawCommand> *coordinationClashMarkers{nullptr};
  const std::vector<DrawCommand> *sectionPlaneVisual{nullptr};
};

struct BimLightingOverlayFrameRecordInputs {
  bool bimGeometryReady{false};
  VkExtent2D framebufferExtent{};
  BimLightingOverlayFrameStyleState points{};
  BimLightingOverlayFrameStyleState curves{};
  BimLightingOverlayFrameStyleState floorPlan{};
  BimLightingOverlayFrameStyleState coordinationIssueMarkers{};
  BimLightingOverlayFrameStyleState coordinationClashMarkers{};
  BimLightingOverlayFrameStyleState sectionPlaneVisual{};
  BimLightingOverlayFrameDrawSources draws{};
  float nativePointSize{1.0f};
  float nativeCurveLineWidth{1.0f};
  bool coordinationMarkerGeometryReady{false};
  bool sectionPlaneVisualGeometryReady{false};
  BimLightingOverlayPipelineHandles pipelines{};
  VkPipelineLayout wireframeLayout{VK_NULL_HANDLE};
  BimLightingOverlayGeometryBinding scene{};
  BimLightingOverlayGeometryBinding bim{};
  BimLightingOverlayGeometryBinding coordinationMarkers{};
  BimLightingOverlayGeometryBinding sectionPlaneVisualGeometry{};
  VkClearAttachment selectionStencilClearAttachment{};
  VkClearRect selectionStencilClearRect{};
  const WireframePushConstants *wireframePushConstants{nullptr};
  const DebugOverlayRenderer *debugOverlay{nullptr};
  bool wireframeRasterModeSupported{false};
  bool wireframeWideLinesSupported{false};
  bool lineListWideLinesSupported{false};
};

struct BimRasterOverlayRecordInputs {
  VkDescriptorSet sceneDescriptorSet{VK_NULL_HANDLE};
  VkDescriptorSet bimDescriptorSet{VK_NULL_HANDLE};
  VkExtent2D extent{};
  VkPipelineLayout wireframeLayout{VK_NULL_HANDLE};
  BimLightingOverlayPipelineHandles pipelines{};
  VkPipeline pointCloudNoDepth{VK_NULL_HANDLE};
  VkPipeline curveNoDepth{VK_NULL_HANDLE};
  VkPipeline curveStrokeNoDepth{VK_NULL_HANDLE};
  VkPipeline capFill{VK_NULL_HANDLE};
  VkPipeline capHatch{VK_NULL_HANDLE};
  const WireframePushConstants *wireframePushConstants{nullptr};
  const DebugOverlayRenderer *debugOverlay{nullptr};
  VkDescriptorSet nativeOitSet{VK_NULL_HANDLE};
};

[[nodiscard]] BimLightingOverlayInputs buildBimLightingOverlayFramePlanInputs(
    const BimLightingOverlayFrameRecordInputs &inputs);

[[nodiscard]] BimLightingOverlayPlan buildBimLightingOverlayFramePlan(
    const BimLightingOverlayFrameRecordInputs &inputs);

[[nodiscard]] bool
recordBimLightingOverlayCommands(VkCommandBuffer cmd,
                                 const BimLightingOverlayRecordInputs &inputs);

[[nodiscard]] bool recordBimLightingOverlayFrameCommands(
    VkCommandBuffer cmd, const BimLightingOverlayFrameRecordInputs &inputs);

enum class BimRasterOverlayPhase { All, Native, Display };

[[nodiscard]] bool hasBimRasterDisplayOverlays(const FrameRecordParams &p);
[[nodiscard]] bool hasBimNativeOitDrawCommands(const FrameRecordParams &p);

void recordBimRasterOverlays(VkCommandBuffer cmd, const FrameRecordParams &p,
                             const BimRasterOverlayRecordInputs &inputs,
                             BimRasterOverlayPhase phase = BimRasterOverlayPhase::All);

void recordBimPresentationFloorPlan(VkCommandBuffer cmd, const FrameRecordParams &p,
                                    VkExtent2D extent, const DebugOverlayRenderer *debugOverlay,
                                    bool resolvedDepthAvailable = false);

void recordBimXrayClashOverlay(VkCommandBuffer cmd, const FrameRecordParams &p,
                               VkExtent2D extent, const DebugOverlayRenderer *debugOverlay);

[[nodiscard]] bool presentsBimTechnicalLines(const FrameRecordParams &p);
void recordBimTechnicalPresentationLines(VkCommandBuffer cmd, const FrameRecordParams &p,
                                        VkExtent2D extent, const DebugOverlayRenderer *debugOverlay);

[[nodiscard]] bool recordBimSelectionStrokeMask(VkCommandBuffer cmd, const FrameRecordParams &p,
                                                VkExtent2D extent, const DebugOverlayRenderer *debugOverlay);
void recordBimSelectionDisplay(VkCommandBuffer cmd, const FrameRecordParams &p,
                               VkDescriptorSet postProcessSet);

} // namespace container::renderer
