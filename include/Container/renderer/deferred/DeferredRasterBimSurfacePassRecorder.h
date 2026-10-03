#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/bim/BimSurfaceRasterPassRecorder.h"

namespace container::renderer {

class BimManager;
class DebugOverlayRenderer;

struct DeferredRasterBimSurfacePassRecordInputs {
  BimSurfacePassKind kind{BimSurfacePassKind::DepthPrepass};
  bool passReady{false};
  RenderingPassHandle renderPass{VK_NULL_HANDLE};
  RenderingTargetHandle framebuffer{VK_NULL_HANDLE};
  VkExtent2D extent{};
  BimSurfaceFrameBinding binding{};
  BimSurfaceRasterPassPipelines pipelines{};
  VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
  const container::gpu::BindlessPushConstants *pushConstants{nullptr};
  const DebugOverlayRenderer *debugOverlay{nullptr};
  BimManager *bimManager{nullptr};
};

[[nodiscard]] bool recordDeferredRasterBimSurfacePassCommands(
    VkCommandBuffer cmd,
    const DeferredRasterBimSurfacePassRecordInputs &inputs);

} // namespace container::renderer
