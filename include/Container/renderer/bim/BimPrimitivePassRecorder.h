#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/bim/BimPrimitivePassPlanner.h"
#include "Container/renderer/debug/DebugOverlayRenderer.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <glm/vec3.hpp>

namespace container::renderer {

class BimManager;

struct BimPrimitivePassRecordInputs {
  const BimPrimitivePassPlan *plan{nullptr};
  bool geometryReady{false};
  VkPipeline depthPipeline{VK_NULL_HANDLE};
  VkPipeline noDepthPipeline{VK_NULL_HANDLE};
  VkPipelineLayout wireframeLayout{VK_NULL_HANDLE};
  VkDescriptorSet sceneDescriptorSet{VK_NULL_HANDLE};
  container::gpu::BufferSlice vertexSlice{};
  container::gpu::BufferSlice indexSlice{};
  VkIndexType indexType{VK_INDEX_TYPE_UINT32};
  const WireframePushConstants *pushConstants{nullptr};
  const DebugOverlayRenderer *debugOverlay{nullptr};
  BimManager *bimManager{nullptr};
  glm::vec3 color{1.0f};
  bool recordLineWidth{false};
  bool wideLinesSupported{false};
  VkDescriptorSet oitDescriptorSet{VK_NULL_HANDLE};
};

struct BimPrimitiveFramePassStyle {
  BimPrimitivePassKind kind{BimPrimitivePassKind::Points};
  bool enabled{false};
  bool depthTest{true};
  bool placeholderRangePreviewEnabled{false};
  bool nativeDrawsUseGpuVisibility{false};
  float opacity{1.0f};
  float primitiveSize{1.0f};
  glm::vec3 color{1.0f};
  bool recordLineWidth{false};
  bool wideLinesSupported{false};
};

struct BimPrimitivePassGeometryBinding {
  VkDescriptorSet sceneDescriptorSet{VK_NULL_HANDLE};
  container::gpu::BufferSlice vertexSlice{};
  container::gpu::BufferSlice indexSlice{};
  VkIndexType indexType{VK_INDEX_TYPE_UINT32};
};

struct BimPrimitiveFramePassPipelines {
  VkPipeline depth{VK_NULL_HANDLE};
  VkPipeline noDepth{VK_NULL_HANDLE};
};

struct BimPrimitiveFramePassRecordInputs {
  BimPrimitiveFramePassStyle style{};
  BimPrimitivePassDrawLists placeholderDraws{};
  BimPrimitivePassDrawLists nativeDraws{};
  BimPrimitivePassGeometryBinding geometry{};
  BimPrimitiveFramePassPipelines pipelines{};
  VkPipelineLayout wireframeLayout{VK_NULL_HANDLE};
  const WireframePushConstants *pushConstants{nullptr};
  const DebugOverlayRenderer *debugOverlay{nullptr};
  BimManager *bimManager{nullptr};
  VkDescriptorSet oitDescriptorSet{VK_NULL_HANDLE};
};

[[nodiscard]] bool hasBimPrimitiveFramePassGeometry(
    const BimPrimitivePassGeometryBinding &geometry);

[[nodiscard]] BimPrimitivePassPlanInputs buildBimPrimitiveFramePassPlanInputs(
    const BimPrimitiveFramePassRecordInputs &inputs);

[[nodiscard]] bool
recordBimPrimitivePassCommands(VkCommandBuffer cmd,
                               const BimPrimitivePassRecordInputs &inputs);

[[nodiscard]] bool recordBimPrimitiveFramePassCommands(
    VkCommandBuffer cmd, const BimPrimitiveFramePassRecordInputs &inputs);

} // namespace container::renderer
