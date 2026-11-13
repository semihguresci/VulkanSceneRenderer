#include "Container/renderer/deferred/DeferredRasterLighting.h"

#include <algorithm>
#include <cmath>

namespace container::renderer {

namespace {

[[nodiscard]] float finiteOr(float value, float fallback) {
  return std::isfinite(value) ? value : fallback;
}

[[nodiscard]] float clampUnit(float value, float fallback) {
  return std::clamp(finiteOr(value, fallback), 0.0f, 1.0f);
}

[[nodiscard]] float sanitizeLineWidth(float value) {
  return std::max(finiteOr(value, 1.0f), 1.0f);
}

[[nodiscard]] bool supportsTransparentOit(
    DeferredLightingDisplayMode displayMode) {
  return displayMode == DeferredLightingDisplayMode::Lit ||
         displayMode == DeferredLightingDisplayMode::Transparency ||
         displayMode == DeferredLightingDisplayMode::Revealage;
}

[[nodiscard]] bool pointLightPipelineReady(
    const DeferredLightingFrameInputs &inputs) {
  return inputs.debugVisualizePointLightStencil
             ? inputs.pipelines.pointLightStencilDebug
             : inputs.pipelines.pointLight;
}

[[nodiscard]] bool bimTechnicalElevationReady(
    const DeferredLightingFrameInputs &inputs) {
  const auto &style = inputs.bimTechnicalElevation;
  const bool pipelineReady = style.depthTestLines
                                 ? inputs.pipelines.wireframeDepth
                                 : inputs.pipelines.wireframeNoDepth;
  return style.enabled && inputs.wireframeSupported && pipelineReady &&
         (!style.hiddenLineOverlay || style.depthTestLines);
}

void applyBimTechnicalElevationStyle(
    DeferredLightingFrameState &state,
    const DeferredLightingBimTechnicalElevationSettings &style) {
  state.wireframeEnabled = true;
  state.wireframeFullMode = false;
  state.wireframeOverlayMode = true;
  state.wireframeSettings.enabled = true;
  state.wireframeSettings.mode = DeferredLightingWireframeMode::Overlay;
  state.wireframeSettings.depthTest = style.depthTestLines;
  state.wireframeSettings.color = style.lineColor;
  state.wireframeSettings.lineWidth = sanitizeLineWidth(style.lineWidth);
  state.wireframeSettings.overlayIntensity =
      clampUnit(style.overlayIntensity, 0.95f);
  state.wireframeIntensity = state.wireframeSettings.overlayIntensity;
}

} // namespace

DeferredLightingFrameState buildDeferredLightingFrameState(
    const DeferredLightingFrameInputs &inputs) {
  DeferredLightingFrameState state{};
  state.displayMode = inputs.displayMode;
  state.wireframeSettings = inputs.wireframeSettings;
  state.wireframeSettings.lineWidth =
      sanitizeLineWidth(state.wireframeSettings.lineWidth);
  state.wireframeSettings.overlayIntensity =
      clampUnit(state.wireframeSettings.overlayIntensity, 0.85f);
  state.normalValidationSettings = inputs.normalValidationSettings;
  state.normalValidationSettings.lineWidth =
      sanitizeLineWidth(state.normalValidationSettings.lineWidth);
  state.bimTechnicalElevationEnabled =
      bimTechnicalElevationReady(inputs);

  const bool guiWireframeEnabled =
      inputs.guiAvailable && inputs.wireframeSupported &&
      state.wireframeSettings.enabled && inputs.pipelines.wireframeDepth &&
      inputs.pipelines.wireframeNoDepth;
  state.wireframeEnabled =
      guiWireframeEnabled || state.bimTechnicalElevationEnabled;
  if (state.bimTechnicalElevationEnabled) {
    applyBimTechnicalElevationStyle(state, inputs.bimTechnicalElevation);
  } else {
    state.wireframeFullMode =
        state.wireframeEnabled &&
        state.wireframeSettings.mode == DeferredLightingWireframeMode::Full;
    state.wireframeOverlayMode =
        state.wireframeEnabled &&
        state.wireframeSettings.mode == DeferredLightingWireframeMode::Overlay;
    state.wireframeIntensity =
        state.wireframeFullMode ? 1.0f
                                : state.wireframeSettings.overlayIntensity;
  }

  state.objectSpaceNormalsEnabled =
      inputs.displayMode == DeferredLightingDisplayMode::ObjectSpaceNormals &&
      inputs.pipelines.objectNormalDebug;
  state.directionalLightingEnabled =
      !state.wireframeFullMode && !state.objectSpaceNormalsEnabled &&
      inputs.pipelines.directionalLight;

  const bool pointLightingAllowed =
      !state.wireframeFullMode && !state.objectSpaceNormalsEnabled &&
      !inputs.debugDirectionalOnly && inputs.pointLightCount > 0u;
  const bool tiledPointLightingReady =
      pointLightingAllowed && inputs.tileCullPassActive &&
      inputs.tiledLightingReady && inputs.depthSamplingReady &&
      inputs.tiledDescriptorSetReady && inputs.pipelines.tiledPointLight;
  if (tiledPointLightingReady) {
    state.pointLighting.path = DeferredPointLightingPath::Tiled;
  } else if (pointLightingAllowed && inputs.clusteredCapacityLimited &&
             inputs.depthSamplingReady && inputs.pipelines.pointLight) {
    state.pointLighting.path = DeferredPointLightingPath::FullscreenFallback;
    state.pointLighting.stencilLightCount =
        std::min(inputs.pointLightCount,
                 container::gpu::kMaxDeferredPointLights);
  } else if (pointLightingAllowed && inputs.pipelines.stencilVolume &&
             pointLightPipelineReady(inputs)) {
    state.pointLighting.path = DeferredPointLightingPath::Stencil;
    state.pointLighting.stencilLightCount =
        std::min(inputs.pointLightCount,
                 container::gpu::kMaxDeferredPointLights);
  }

  state.transparentOitEnabled =
      inputs.transparentDrawCommandsAvailable &&
      supportsTransparentOit(inputs.displayMode) && !state.wireframeFullMode;
  state.geometryOverlayEnabled =
      inputs.guiAvailable && inputs.geometryOverlayRequested &&
      inputs.pipelines.geometryDebug;
  state.normalValidationEnabled =
      inputs.guiAvailable && inputs.normalValidationSettings.enabled &&
      inputs.pipelines.normalValidation;
  state.surfaceNormalLinesEnabled =
      inputs.pipelines.surfaceNormalLine &&
      (state.normalValidationEnabled ||
       (inputs.guiAvailable &&
        inputs.displayMode == DeferredLightingDisplayMode::SurfaceNormals));
  state.surfaceNormalLineWidth =
      inputs.wireframeWideLinesSupported
          ? state.normalValidationSettings.lineWidth
          : 1.0f;
  return state;
}

} // namespace container::renderer
