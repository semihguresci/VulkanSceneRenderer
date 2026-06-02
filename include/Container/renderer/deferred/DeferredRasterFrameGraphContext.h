#pragma once

#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/debug/DebugOverlayRenderer.h"
#include "Container/renderer/deferred/DeferredRasterLightingPassRecorder.h"
#include "Container/renderer/deferred/DeferredTransparentOitFramePassRecorder.h"
#include "Container/renderer/shadow/ShadowCascadeFramePassRecorder.h"

#include <optional>

namespace container::gpu {
class SwapChainManager;
} // namespace container::gpu

namespace container::scene {
class BaseCamera;
} // namespace container::scene

namespace container::ui {
class GuiManager;
enum class GBufferViewMode : uint32_t;
} // namespace container::ui

namespace container::renderer {

class BloomManager;
class EnvironmentManager;
class ExposureManager;
class GpuCullManager;
class LightingManager;
class OitManager;
class SceneController;

struct DeferredRasterFrameGraphServices {
  RenderGraph *graph{nullptr};
  container::gpu::SwapChainManager *swapChainManager{nullptr};
  const OitManager *oitManager{nullptr};
  const LightingManager *lightingManager{nullptr};
  const EnvironmentManager *environmentManager{nullptr};
  const SceneController *sceneController{nullptr};
  GpuCullManager *gpuCullManager{nullptr};
  BloomManager *bloomManager{nullptr};
  ExposureManager *exposureManager{nullptr};
  const container::scene::BaseCamera *camera{nullptr};
  container::ui::GuiManager *guiManager{nullptr};
  std::optional<container::ui::GBufferViewMode> fallbackDisplayMode{};
};

// Deferred-raster compatibility facade. The generic FrameRecorder owns only
// graph execution; this facade owns deferred-specific services and pre/post
// frame hooks while the larger frame contract is being split.
class DeferredRasterFrameGraphContext {
public:
  explicit DeferredRasterFrameGraphContext(
      DeferredRasterFrameGraphServices services);

  [[nodiscard]] RenderGraphBuilder graphBuilder() const;
  [[nodiscard]] VkExtent2D swapchainExtent() const;
  [[nodiscard]] container::ui::GBufferViewMode displayMode() const;
  [[nodiscard]] const OitManager *oitManager() const;
  [[nodiscard]] DeferredTransparentOitFramePassRecorder
  transparentOitFramePassRecorder() const;
  [[nodiscard]] bool isPassActive(RenderPassId id) const;
  [[nodiscard]] FrameRecordLifecycleHooks lifecycleHooks() const;

  [[nodiscard]] GpuCullManager *gpuCullManager() const;
  [[nodiscard]] BloomManager *bloomManager() const;
  [[nodiscard]] ExposureManager *exposureManager() const;
  [[nodiscard]] const EnvironmentManager *environmentManager() const;
  [[nodiscard]] const LightingManager *lightingManager() const;
  [[nodiscard]] container::ui::GuiManager *guiManager() const;
  [[nodiscard]] const container::scene::BaseCamera *camera() const;
  [[nodiscard]] const SceneController *sceneController() const;
  [[nodiscard]] const DebugOverlayRenderer *debugOverlay() const;
  [[nodiscard]] DeferredRasterLightingPassRecorder lightingPassRecorder() const;

  void recordShadowPass(VkCommandBuffer cmd, const FrameRecordParams &p,
                        uint32_t cascadeIndex) const;
  void recordForwardShadowPass(VkCommandBuffer cmd, const FrameRecordParams &p,
                               uint32_t cascadeIndex) const;
  void renderGui(VkCommandBuffer cmd) const;

  [[nodiscard]] bool canRecordShadowPass(const FrameRecordParams &p,
                                         uint32_t cascadeIndex) const;
  [[nodiscard]] bool canRecordForwardShadowPass(
      const FrameRecordParams &p, uint32_t cascadeIndex) const;

private:
  [[nodiscard]] ShadowCascadeFramePassContext
  shadowCascadeFramePassContext() const;
  [[nodiscard]] ShadowCascadeFramePassContext
  forwardShadowCascadeFramePassContext() const;
  [[nodiscard]] ShadowCascadeFramePassContext
  shadowCascadeFramePassContext(const FrameRecordParams &p) const;
  void beforePrepareFrame(const FrameRecordParams &p) const;
  void afterPrepareFrame(const FrameRecordParams &p,
                         const RenderGraph &graph) const;
  void afterCommandBufferBegin(VkCommandBuffer cmd,
                               const FrameRecordParams &p) const;
  void afterGraphExecution(VkCommandBuffer cmd,
                           const FrameRecordParams &p) const;

  DeferredRasterFrameGraphServices services_{};
  DebugOverlayRenderer debugOverlay_{};
  ShadowCascadeFramePassRecorder shadowCascadeFramePassRecorder_{};
};

} // namespace container::renderer
