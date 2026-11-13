#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/bim/BimLightingOverlayPlanner.h"
#include "Container/renderer/debug/DebugOverlayRenderer.h"
#include "Container/utility/VulkanMemoryManager.h"

namespace container::renderer {

struct FrameRecordParams;

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
  VkPipeline capFill{VK_NULL_HANDLE};
  VkPipeline capHatch{VK_NULL_HANDLE};
  const WireframePushConstants *wireframePushConstants{nullptr};
  const DebugOverlayRenderer *debugOverlay{nullptr};
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

void recordBimRasterOverlays(VkCommandBuffer cmd, const FrameRecordParams &p,
                             const BimRasterOverlayRecordInputs &inputs);

} // namespace container::renderer
