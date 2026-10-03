#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/bim/BimSurfacePassRecorder.h"
#include "Container/utility/SceneData.h"

#include <span>

namespace container::renderer {

struct BimSurfaceRasterPassPipelines {
  VkPipeline singleSided{VK_NULL_HANDLE};
  VkPipeline windingFlipped{VK_NULL_HANDLE};
  VkPipeline doubleSided{VK_NULL_HANDLE};
};

struct BimSurfaceRasterPassRecordInputs {
  RenderingPassHandle renderPass{VK_NULL_HANDLE};
  RenderingTargetHandle framebuffer{VK_NULL_HANDLE};
  VkExtent2D extent{};
  const BimSurfacePassPlan *plan{nullptr};
  BimSurfacePassGeometryBinding geometry{};
  BimSurfaceRasterPassPipelines pipelines{};
  VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
  container::gpu::BindlessPushConstants pushConstants{};
  const DebugOverlayRenderer *debugOverlay{nullptr};
  BimManager *bimManager{nullptr};
};

struct BimSurfaceFramePassDrawSources {
  BimSurfaceDrawLists mesh{};
  BimSurfaceDrawLists pointPlaceholders{};
  BimSurfaceDrawLists curvePlaceholders{};
  bool opaqueMeshDrawsUseGpuVisibility{false};
  bool transparentMeshDrawsUseGpuVisibility{false};
};

struct BimSurfaceFrameBinding {
  BimSurfaceFramePassDrawSources draws{};
  BimSurfacePassGeometryBinding geometry{};
  uint32_t semanticColorMode{0};
};

struct BimSurfaceFrameBindingInputs {
  BimSurfaceFramePassDrawSources draws{};
  container::gpu::BufferSlice vertexSlice{};
  container::gpu::BufferSlice indexSlice{};
  VkIndexType indexType{VK_INDEX_TYPE_UINT32};
  std::span<const VkDescriptorSet> descriptorSets{};
  uint32_t semanticColorMode{0};
};

struct BimSurfaceFramePassRecordInputs {
  BimSurfacePassKind kind{BimSurfacePassKind::DepthPrepass};
  bool passReady{false};
  BimSurfaceFramePassDrawSources draws{};
  RenderingPassHandle renderPass{VK_NULL_HANDLE};
  RenderingTargetHandle framebuffer{VK_NULL_HANDLE};
  VkExtent2D extent{};
  BimSurfacePassGeometryBinding geometry{};
  BimSurfaceRasterPassPipelines pipelines{};
  VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
  const container::gpu::BindlessPushConstants *pushConstants{nullptr};
  uint32_t semanticColorMode{0};
  const DebugOverlayRenderer *debugOverlay{nullptr};
  BimManager *bimManager{nullptr};
};

[[nodiscard]] container::gpu::BindlessPushConstants
bimSurfaceRasterPassPushConstants(container::gpu::BindlessPushConstants base,
                                  const BimSurfacePassPlan &plan);

[[nodiscard]] BimSurfacePassInputs
buildBimSurfaceFramePassInputs(const BimSurfaceFramePassRecordInputs &inputs);

[[nodiscard]] BimSurfacePassPlan
buildBimSurfaceFramePassPlan(const BimSurfaceFramePassRecordInputs &inputs);

[[nodiscard]] BimSurfaceFrameBinding buildBimSurfaceFrameBinding(
    const BimSurfaceFrameBindingInputs &inputs);

[[nodiscard]] bool recordBimSurfaceRasterPassCommands(
    VkCommandBuffer cmd, const BimSurfaceRasterPassRecordInputs &inputs);

[[nodiscard]] bool recordBimSurfaceFramePassCommands(
    VkCommandBuffer cmd, const BimSurfaceFramePassRecordInputs &inputs);

} // namespace container::renderer
