#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/scene/SceneRasterPassPlanner.h"
#include "Container/renderer/scene/SceneRasterPassRecorder.h"

namespace container::renderer {

class DebugOverlayRenderer;
class GpuCullManager;

struct DeferredRasterScenePassRecordInputs {
  SceneRasterPassKind kind{SceneRasterPassKind::DepthPrepass};
  VkRenderPass renderPass{VK_NULL_HANDLE};
  VkFramebuffer framebuffer{VK_NULL_HANDLE};
  VkExtent2D extent{};
  SceneOpaqueDrawLists draws{};
  SceneOpaqueDrawGeometryBinding geometry{};
  SceneRasterPassPipelineInputs pipelines{};
  VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
  const container::gpu::BindlessPushConstants *pushConstants{nullptr};
  uint32_t imageIndex{0};
  SceneDiagnosticCubeRecordInputs diagnosticCube{};
  const GpuCullManager *gpuCullManager{nullptr};
  bool frustumCullActive{false};
  bool occlusionCullActive{false};
  const DebugOverlayRenderer *debugOverlay{nullptr};
};

[[nodiscard]] bool recordDeferredRasterScenePassCommands(
    VkCommandBuffer cmd, const DeferredRasterScenePassRecordInputs &inputs);

} // namespace container::renderer
