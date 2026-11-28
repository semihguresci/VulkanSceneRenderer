#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/debug/DebugOverlayRenderer.h"
#include "Container/renderer/deferred/DeferredRasterDebugOverlayPlanner.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/VulkanMemoryManager.h"

namespace container::renderer {

struct DeferredDebugOverlayGeometryBinding {
  VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
  container::gpu::BufferSlice vertexSlice{};
  container::gpu::BufferSlice indexSlice{};
  VkIndexType indexType{VK_INDEX_TYPE_UINT32};
};

struct DeferredDebugOverlayDiagnosticGeometry {
  container::gpu::BufferSlice vertexSlice{};
  container::gpu::BufferSlice indexSlice{};
  uint32_t indexCount{0};
};

struct DeferredDebugOverlayPipelineHandles {
  VkPipeline wireframeDepth{VK_NULL_HANDLE};
  VkPipeline wireframeNoDepth{VK_NULL_HANDLE};
  VkPipeline wireframeDepthFrontCull{VK_NULL_HANDLE};
  VkPipeline wireframeNoDepthFrontCull{VK_NULL_HANDLE};
  VkPipeline objectNormalDebug{VK_NULL_HANDLE};
  VkPipeline objectNormalDebugFrontCull{VK_NULL_HANDLE};
  VkPipeline objectNormalDebugNoCull{VK_NULL_HANDLE};
  VkPipeline geometryDebug{VK_NULL_HANDLE};
  VkPipeline normalValidation{VK_NULL_HANDLE};
  VkPipeline normalValidationFrontCull{VK_NULL_HANDLE};
  VkPipeline normalValidationNoCull{VK_NULL_HANDLE};
  VkPipeline surfaceNormalLine{VK_NULL_HANDLE};
};

struct DeferredDebugOverlayRecordInputs {
  const DeferredDebugOverlayPlan *plan{nullptr};
  DeferredDebugOverlayPipelineHandles pipelines{};
  VkPipelineLayout sceneLayout{VK_NULL_HANDLE};
  VkPipelineLayout wireframeLayout{VK_NULL_HANDLE};
  VkPipelineLayout normalValidationLayout{VK_NULL_HANDLE};
  VkPipelineLayout surfaceNormalLayout{VK_NULL_HANDLE};
  DeferredDebugOverlayGeometryBinding scene{};
  DeferredDebugOverlayGeometryBinding bim{};
  DeferredDebugOverlayDiagnosticGeometry diagnostic{};
  const container::gpu::BindlessPushConstants *bindlessPushConstants{nullptr};
  const WireframePushConstants *wireframePushConstants{nullptr};
  const NormalValidationPushConstants *normalValidationPushConstants{nullptr};
  const SurfaceNormalPushConstants *surfaceNormalPushConstants{nullptr};
  container::gpu::NormalValidationSettings normalValidationSettings{};
  glm::vec3 wireframeColor{0.0f, 1.0f, 0.0f};
  float wireframeIntensity{1.0f};
  float wireframeLineWidth{1.0f};
  bool wireframeRasterModeSupported{false};
  bool wireframeWideLinesSupported{false};
  const DebugOverlayRenderer *debugOverlay{nullptr};
};

[[nodiscard]] bool recordDeferredDebugOverlayWireframeFullCommands(
    VkCommandBuffer cmd, const DeferredDebugOverlayRecordInputs &inputs);

[[nodiscard]] bool recordDeferredDebugOverlayObjectNormalCommands(
    VkCommandBuffer cmd, const DeferredDebugOverlayRecordInputs &inputs);

[[nodiscard]] bool recordDeferredDebugOverlayGeometryCommands(
    VkCommandBuffer cmd, const DeferredDebugOverlayRecordInputs &inputs);

[[nodiscard]] bool recordDeferredDebugOverlayNormalValidationCommands(
    VkCommandBuffer cmd, const DeferredDebugOverlayRecordInputs &inputs);

[[nodiscard]] bool recordDeferredDebugOverlaySurfaceNormalCommands(
    VkCommandBuffer cmd, const DeferredDebugOverlayRecordInputs &inputs);

[[nodiscard]] bool recordDeferredDebugOverlayWireframeOverlayCommands(
    VkCommandBuffer cmd, const DeferredDebugOverlayRecordInputs &inputs,
    VkDescriptorSet oitSet = VK_NULL_HANDLE);

} // namespace container::renderer
