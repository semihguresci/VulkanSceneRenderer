#include "Container/renderer/deferred/DeferredRasterFrameState.h"

#include "Container/renderer/deferred/DeferredRasterPipelineBridge.h"
#include "Container/renderer/deferred/DeferredRasterResourceBridge.h"
#include "Container/utility/GuiManager.h"

#include <algorithm>
#include <cmath>

namespace container::renderer {

bool hasDrawCommands(const std::vector<DrawCommand> *commands) {
  return commands != nullptr && !commands->empty();
}

bool hasSplitOpaqueDrawCommands(const FrameDrawLists &draws) {
  return hasDrawCommands(draws.opaqueSingleSidedDrawCommands) ||
         hasDrawCommands(draws.opaqueWindingFlippedDrawCommands) ||
         hasDrawCommands(draws.opaqueDoubleSidedDrawCommands);
}

bool hasOpaqueDrawCommands(const FrameDrawLists &draws) {
  return hasSplitOpaqueDrawCommands(draws) ||
         hasDrawCommands(draws.opaqueDrawCommands);
}

std::array<const FrameDrawLists *, 3>
bimSurfaceDrawListSet(const FrameBimResources &bim) {
  // Surface passes consume mesh draws plus point/curve placeholder fallbacks.
  // Native point/curve ranges are submitted by the dedicated
  // point-list/line-list primitive passes instead of entering triangle G-buffer
  // or shadow paths.
  return {&bim.draws, &bim.pointDraws, &bim.curveDraws};
}

bool hasBimOpaqueDrawCommands(const FrameBimResources &bim) {
  for (const FrameDrawLists* draws : bimSurfaceDrawListSet(bim)) {
    if (hasOpaqueDrawCommands(*draws)) {
      return true;
    }
  }
  return false;
}

bool hasTransparentDrawCommands(const FrameDrawLists &draws) {
  return hasDrawCommands(draws.transparentSingleSidedDrawCommands) ||
         hasDrawCommands(draws.transparentWindingFlippedDrawCommands) ||
         hasDrawCommands(draws.transparentDoubleSidedDrawCommands) ||
         hasDrawCommands(draws.transparentDrawCommands);
}

bool hasTransparentDrawCommands(const FrameRecordParams &p) {
  if (hasTransparentDrawCommands(p.draws)) {
    return true;
  }
  for (const FrameDrawLists *draws : bimSurfaceDrawListSet(p.bim)) {
    if (hasTransparentDrawCommands(*draws)) {
      return true;
    }
  }
  return false;
}

bool hasBimTransparentGeometry(const FrameRecordParams &p) {
  const auto &bimScene = p.bim.scene;
  bool hasTransparent = false;
  for (const FrameDrawLists *draws : bimSurfaceDrawListSet(p.bim)) {
    hasTransparent = hasTransparent || hasTransparentDrawCommands(*draws);
  }
  return deferredRasterDescriptorSet(
             p, DeferredRasterDescriptorSetId::BimScene) != VK_NULL_HANDLE &&
         bimScene.vertexSlice.buffer != VK_NULL_HANDLE &&
         bimScene.indexSlice.buffer != VK_NULL_HANDLE && hasTransparent;
}

container::ui::GBufferViewMode
currentDisplayMode(const container::ui::GuiManager *guiManager) {
  return currentDisplayMode(guiManager,
                            container::ui::GBufferViewMode::Overview);
}

container::ui::GBufferViewMode
currentDisplayMode(const container::ui::GuiManager *guiManager,
                   container::ui::GBufferViewMode fallbackDisplayMode) {
  return guiManager ? guiManager->gBufferViewMode() : fallbackDisplayMode;
}

bool displayModeRecordsShadowAtlas(container::ui::GBufferViewMode mode) {
  return static_cast<uint32_t>(mode) >= 100u ||
         mode == container::ui::GBufferViewMode::Lit ||
         mode == container::ui::GBufferViewMode::Overview;
}

bool displayModeRecordsTileCull(container::ui::GBufferViewMode mode) {
  return static_cast<uint32_t>(mode) >= 100u ||
         mode == container::ui::GBufferViewMode::Lit ||
         mode == container::ui::GBufferViewMode::Overview ||
         mode == container::ui::GBufferViewMode::TileLightHeatMap;
}

bool displayModeRecordsGtao(container::ui::GBufferViewMode mode) {
  return static_cast<uint32_t>(mode) >= 100u ||
         mode == container::ui::GBufferViewMode::Lit ||
         mode == container::ui::GBufferViewMode::Overview;
}

bool displayModeRecordsExposureAdaptation(container::ui::GBufferViewMode mode) {
  return static_cast<uint32_t>(mode) >= 100u ||
         mode == container::ui::GBufferViewMode::Lit ||
         mode == container::ui::GBufferViewMode::Overview;
}

bool displayModeRecordsBloom(container::ui::GBufferViewMode mode) {
  return static_cast<uint32_t>(mode) >= 100u ||
         mode == container::ui::GBufferViewMode::Lit ||
         mode == container::ui::GBufferViewMode::Overview;
}

namespace {

float finiteOr(float value, float fallback) {
  return std::isfinite(value) ? value : fallback;
}

} // namespace

container::gpu::ExposureSettings
sanitizeExposureSettings(container::gpu::ExposureSettings settings) {
  settings.mode = settings.mode == container::gpu::kExposureModeAuto
                      ? container::gpu::kExposureModeAuto
                      : container::gpu::kExposureModeManual;
  settings.manualExposure =
      std::max(finiteOr(settings.manualExposure, 0.25f), 0.0f);
  settings.targetLuminance =
      std::max(finiteOr(settings.targetLuminance, 0.18f), 0.001f);
  settings.minExposure =
      std::max(finiteOr(settings.minExposure, 0.03125f), 0.0f);
  settings.maxExposure =
      std::max(finiteOr(settings.maxExposure, 8.0f), settings.minExposure);
  settings.adaptationRate =
      std::max(finiteOr(settings.adaptationRate, 1.5f), 0.0f);
  settings.meteringLowPercentile =
      std::clamp(finiteOr(settings.meteringLowPercentile, 0.50f), 0.0f, 0.99f);
  settings.meteringHighPercentile =
      std::clamp(finiteOr(settings.meteringHighPercentile, 0.95f),
                 settings.meteringLowPercentile + 0.01f, 1.0f);
  return settings;
}

bool shouldRecordTransparentOit(const FrameRecordParams &p,
                                const container::ui::GuiManager *guiManager) {
  return shouldRecordTransparentOit(p, guiManager,
                                    container::ui::GBufferViewMode::Overview);
}

bool shouldRecordTransparentOit(
    const FrameRecordParams &p, const container::ui::GuiManager *guiManager,
    container::ui::GBufferViewMode fallbackDisplayMode) {
  if (!hasTransparentDrawCommands(p)) {
    return false;
  }

  const auto displayMode = currentDisplayMode(guiManager, fallbackDisplayMode);
  if (static_cast<uint32_t>(displayMode) < 100u &&
      displayMode != container::ui::GBufferViewMode::Lit &&
      displayMode != container::ui::GBufferViewMode::Overview &&
      displayMode != container::ui::GBufferViewMode::Transparency &&
      displayMode != container::ui::GBufferViewMode::Revealage) {
    return false;
  }

  const auto wireframeSettings = guiManager
                                     ? guiManager->wireframeSettings()
                                     : container::ui::WireframeSettings{};
  const bool editorOverlaysEnabled =
      guiManager == nullptr || guiManager->editorOverlaysEnabled();
  const bool wireframeFullMode =
      guiManager && editorOverlaysEnabled && guiManager->wireframeSupported() &&
      wireframeSettings.enabled &&
      wireframeSettings.mode == container::ui::WireframeMode::Full &&
      deferredRasterPipelineReady(p,
                                  DeferredRasterPipelineId::WireframeDepth) &&
      deferredRasterPipelineReady(p,
                                  DeferredRasterPipelineId::WireframeNoDepth);
  return !wireframeFullMode;
}

RenderPassReadiness renderPassReady() { return {}; }

RenderPassReadiness renderPassNotNeeded() {
  RenderPassReadiness readiness{};
  readiness.ready = false;
  readiness.skipReason = RenderPassSkipReason::NotNeeded;
  return readiness;
}

RenderPassReadiness renderPassMissingResource(RenderResourceId resource) {
  RenderPassReadiness readiness{};
  readiness.ready = false;
  readiness.skipReason = RenderPassSkipReason::MissingResource;
  readiness.blockingResource = resource;
  return readiness;
}

} // namespace container::renderer
