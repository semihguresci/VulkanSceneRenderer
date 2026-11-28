#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/bim/BimSectionClipCapPassPlanner.h"
#include "Container/renderer/debug/DebugOverlayRenderer.h"
#include "Container/utility/VulkanMemoryManager.h"

namespace container::renderer {

struct BimSectionClipCapPassRecordInputs {
  const BimSectionClipCapPassPlan *plan{nullptr};
  VkPipeline fillPipeline{VK_NULL_HANDLE};
  VkPipeline hatchPipeline{VK_NULL_HANDLE};
  VkPipelineLayout wireframeLayout{VK_NULL_HANDLE};
  VkDescriptorSet sceneDescriptorSet{VK_NULL_HANDLE};
  container::gpu::BufferSlice vertexSlice{};
  container::gpu::BufferSlice indexSlice{};
  VkIndexType indexType{VK_INDEX_TYPE_UINT32};
  const WireframePushConstants *pushConstants{nullptr};
  const DebugOverlayRenderer *debugOverlay{nullptr};
  VkDescriptorSet oitDescriptorSet{VK_NULL_HANDLE};
};

struct BimSectionClipCapFramePassStyle {
  bool enabled{false};
  bool fillEnabled{false};
  bool hatchEnabled{false};
  bool wideLinesSupported{false};
  glm::vec4 fillColor{0.06f, 0.08f, 0.10f, 0.82f};
  glm::vec4 hatchColor{0.85f, 0.72f, 0.32f, 0.95f};
  float hatchLineWidth{1.0f};
  const std::vector<DrawCommand> *fillDrawCommands{nullptr};
  const std::vector<DrawCommand> *hatchDrawCommands{nullptr};
  const std::vector<BimSectionCapDrawStyle> *fillDrawStyles{nullptr};
  const std::vector<BimSectionCapDrawStyle> *hatchDrawStyles{nullptr};
  const std::vector<BimSectionMarkerLine> *sectionMarkerLines{nullptr};
};

struct BimSectionClipCapPassGeometryBinding {
  VkDescriptorSet sceneDescriptorSet{VK_NULL_HANDLE};
  container::gpu::BufferSlice vertexSlice{};
  container::gpu::BufferSlice indexSlice{};
  VkIndexType indexType{VK_INDEX_TYPE_UINT32};
};

struct BimSectionClipCapFramePassRecordInputs {
  BimSectionClipCapFramePassStyle style{};
  BimSectionClipCapPassGeometryBinding geometry{};
  VkPipeline fillPipeline{VK_NULL_HANDLE};
  VkPipeline hatchPipeline{VK_NULL_HANDLE};
  VkPipelineLayout wireframeLayout{VK_NULL_HANDLE};
  const WireframePushConstants *pushConstants{nullptr};
  const DebugOverlayRenderer *debugOverlay{nullptr};
  VkDescriptorSet oitDescriptorSet{VK_NULL_HANDLE};
};

[[nodiscard]] bool hasBimSectionClipCapFramePassGeometry(
    const BimSectionClipCapPassGeometryBinding &geometry);

[[nodiscard]] BimSectionClipCapPassInputs
buildBimSectionClipCapFramePassPlanInputs(
    const BimSectionClipCapFramePassRecordInputs &inputs);

[[nodiscard]] bool recordBimSectionClipCapPassCommands(
    VkCommandBuffer cmd, const BimSectionClipCapPassRecordInputs &inputs);

[[nodiscard]] bool recordBimSectionClipCapFramePassCommands(
    VkCommandBuffer cmd, const BimSectionClipCapFramePassRecordInputs &inputs);

} // namespace container::renderer
