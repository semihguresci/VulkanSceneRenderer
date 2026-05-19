#include "Container/renderer/deferred/DeferredRasterFrameGraphContext.h"

#include "Container/renderer/bim/BimFrameGpuVisibilityRecorder.h"
#include "Container/renderer/core/ScreenshotCaptureRecorder.h"
#include "Container/renderer/culling/GpuCullManager.h"
#include "Container/renderer/deferred/DeferredRasterFrameState.h"
#include "Container/renderer/deferred/DeferredRasterGuiPassRecorder.h"
#include "Container/renderer/deferred/DeferredRasterResourceBridge.h"
#include "Container/renderer/lighting/LightingManager.h"
#include "Container/utility/GuiManager.h"
#include "Container/utility/SwapChainManager.h"

#include <stdexcept>

namespace container::renderer {

DeferredRasterFrameGraphContext::DeferredRasterFrameGraphContext(
    DeferredRasterFrameGraphServices services)
    : services_(services) {}

RenderGraphBuilder DeferredRasterFrameGraphContext::graphBuilder() const {
  if (services_.graph == nullptr) {
    throw std::runtime_error(
        "DeferredRasterFrameGraphContext requires a render graph");
  }
  return RenderGraphBuilder(*services_.graph);
}

VkExtent2D DeferredRasterFrameGraphContext::swapchainExtent() const {
  return services_.swapChainManager != nullptr
             ? services_.swapChainManager->extent()
             : VkExtent2D{};
}

container::ui::GBufferViewMode
DeferredRasterFrameGraphContext::displayMode() const {
  const auto fallbackDisplayMode = services_.fallbackDisplayMode.value_or(
      container::ui::GBufferViewMode::Overview);
  return currentDisplayMode(services_.guiManager, fallbackDisplayMode);
}

bool DeferredRasterFrameGraphContext::isPassActive(RenderPassId id) const {
  if (services_.graph == nullptr) {
    return false;
  }
  for (const auto &status : services_.graph->lastFrameExecutionStatuses()) {
    if (status.id == id) {
      return status.active;
    }
  }
  return services_.graph->isPassActive(id);
}

FrameRecordLifecycleHooks
DeferredRasterFrameGraphContext::lifecycleHooks() const {
  FrameRecordLifecycleHooks hooks{};
  hooks.beforePrepareFrame = [this](const FrameRecordParams &p) {
    beforePrepareFrame(p);
  };
  hooks.afterPrepareFrame = [this](const FrameRecordParams &p,
                                   const RenderGraph &graph) {
    afterPrepareFrame(p, graph);
  };
  hooks.afterCommandBufferBegin = [this](VkCommandBuffer cmd,
                                         const FrameRecordParams &p) {
    afterCommandBufferBegin(cmd, p);
  };
  hooks.afterGraphExecution = [this](VkCommandBuffer cmd,
                                     const FrameRecordParams &p) {
    afterGraphExecution(cmd, p);
  };
  return hooks;
}

GpuCullManager *DeferredRasterFrameGraphContext::gpuCullManager() const {
  return services_.gpuCullManager;
}

DeferredTransparentOitFramePassRecorder
DeferredRasterFrameGraphContext::transparentOitFramePassRecorder() const {
  return DeferredTransparentOitFramePassRecorder(
      {.oitManager = services_.oitManager,
       .guiManager = services_.guiManager,
       .fallbackDisplayMode = displayMode()});
}

BloomManager *DeferredRasterFrameGraphContext::bloomManager() const {
  return services_.bloomManager;
}

ExposureManager *DeferredRasterFrameGraphContext::exposureManager() const {
  return services_.exposureManager;
}

const EnvironmentManager *
DeferredRasterFrameGraphContext::environmentManager() const {
  return services_.environmentManager;
}

const LightingManager *
DeferredRasterFrameGraphContext::lightingManager() const {
  return services_.lightingManager;
}

container::ui::GuiManager *DeferredRasterFrameGraphContext::guiManager() const {
  return services_.guiManager;
}

const container::scene::BaseCamera *
DeferredRasterFrameGraphContext::camera() const {
  return services_.camera;
}

const SceneController *
DeferredRasterFrameGraphContext::sceneController() const {
  return services_.sceneController;
}

const DebugOverlayRenderer *
DeferredRasterFrameGraphContext::debugOverlay() const {
  return &debugOverlay_;
}

DeferredRasterLightingPassRecorder
DeferredRasterFrameGraphContext::lightingPassRecorder() const {
  return DeferredRasterLightingPassRecorder(
      {.framebufferExtent = swapchainExtent(),
       .lightingManager = services_.lightingManager,
       .sceneController = services_.sceneController,
       .camera = services_.camera,
       .guiManager = services_.guiManager,
       .fallbackDisplayMode = displayMode(),
       .debugOverlay = &debugOverlay_,
       .tileCullPassActive = isPassActive(RenderPassId::TileCull)});
}

void DeferredRasterFrameGraphContext::recordShadowPass(
    VkCommandBuffer cmd, const FrameRecordParams &p,
    uint32_t cascadeIndex) const {
  shadowCascadeFramePassRecorder_.recordCascadePass(
      cmd, p, shadowCascadeFramePassContext(), cascadeIndex);
}

void DeferredRasterFrameGraphContext::renderGui(VkCommandBuffer cmd) const {
  static_cast<void>(recordDeferredRasterGuiPass(
      {.commandBuffer = cmd, .guiManager = services_.guiManager}));
}

bool DeferredRasterFrameGraphContext::canRecordShadowPass(
    const FrameRecordParams &p, uint32_t cascadeIndex) const {
  return shadowCascadeFramePassRecorder_.canRecordCascade(p, cascadeIndex);
}

ShadowCascadeFramePassContext
DeferredRasterFrameGraphContext::shadowCascadeFramePassContext() const {
  return {.shadowAtlasVisible = displayModeRecordsShadowAtlas(displayMode()),
          .isPassActive = [this](RenderPassId id) { return isPassActive(id); }};
}

void DeferredRasterFrameGraphContext::beforePrepareFrame(
    const FrameRecordParams &p) const {
  if (services_.gpuCullManager != nullptr) {
    services_.gpuCullManager->beginFrameCulling(p.runtime.imageIndex);
  }
}

void DeferredRasterFrameGraphContext::afterPrepareFrame(
    const FrameRecordParams &p, const RenderGraph &) const {
  shadowCascadeFramePassRecorder_.prepareFrame(p,
                                               shadowCascadeFramePassContext());
  prepareBimFrameGpuVisibility(p.services.bimManager, p.bim);
}

void DeferredRasterFrameGraphContext::afterCommandBufferBegin(
    VkCommandBuffer cmd, const FrameRecordParams &p) const {
  if (services_.lightingManager != nullptr) {
    services_.lightingManager->resetGpuTimers(cmd, p.runtime.imageIndex);
  }
  static_cast<void>(recordBimFrameGpuVisibilityCommands(
      {.manager = p.services.bimManager,
       .commandBuffer = cmd,
       .cameraBuffer = deferredRasterBuffer(p, DeferredRasterBufferId::Camera),
       .cameraBufferSize =
           deferredRasterBufferSize(p, DeferredRasterBufferId::Camera),
       .objectBuffer = p.bim.scene.objectBuffer,
       .objectBufferSize = p.bim.scene.objectBufferSize}));
}

void DeferredRasterFrameGraphContext::afterGraphExecution(
    VkCommandBuffer cmd, const FrameRecordParams &p) const {
  if (!p.screenshot.enabled) {
    return;
  }
  recordScreenshotCaptureCopy({.commandBuffer = cmd,
                               .swapChainImage = p.screenshot.swapChainImage,
                               .readbackBuffer = p.screenshot.readbackBuffer,
                               .extent = p.screenshot.extent});
}

} // namespace container::renderer
