#include "Container/renderer/deferred/DeferredRasterLightingPassRecorder.h"

#include "Container/renderer/bim/BimLightingOverlayRecorder.h"
#include "Container/renderer/bim/BimPrimitivePassRecorder.h"
#include "Container/renderer/bim/BimSectionClipCapPassRecorder.h"
#include "Container/renderer/bim/BimSurfaceRasterPassRecorder.h"
#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/core/RenderPassScopeRecorder.h"
#include "Container/renderer/deferred/DeferredDirectionalLightingRecorder.h"
#include "Container/renderer/deferred/DeferredLightingDescriptorPlanner.h"
#include "Container/renderer/deferred/DeferredLightingPassPlanner.h"
#include "Container/renderer/deferred/DeferredPointLightingDrawPlanner.h"
#include "Container/renderer/deferred/DeferredPointLightingRecorder.h"
#include "Container/renderer/deferred/DeferredRasterDebugOverlayPlanner.h"
#include "Container/renderer/deferred/DeferredRasterDebugOverlayRecorder.h"
#include "Container/renderer/deferred/DeferredRasterFrameState.h"
#include "Container/renderer/deferred/DeferredRasterLighting.h"
#include "Container/renderer/deferred/DeferredRasterPipelineBridge.h"
#include "Container/renderer/deferred/DeferredRasterResourceBridge.h"
#include "Container/renderer/deferred/DeferredTransparentOitRecorder.h"
#include "Container/renderer/lighting/LightingManager.h"
#include "Container/renderer/scene/SceneController.h"
#include "Container/renderer/scene/SceneTransparentDrawPlanner.h"
#include "Container/renderer/scene/SceneViewport.h"
#include "Container/utility/GuiManager.h"

#include <array>
#include <span>
#include <vector>

namespace container::renderer {

namespace {

bool hasRenderableDrawCommands(const FrameDrawLists &draws) {
  return hasOpaqueDrawCommands(draws) || hasTransparentDrawCommands(draws);
}

DeferredLightingDisplayMode
deferredLightingDisplayMode(container::ui::GBufferViewMode mode) {
  if (static_cast<uint32_t>(mode) >= 100u)
    return DeferredLightingDisplayMode::Lit;
  switch (mode) {
  case container::ui::GBufferViewMode::Lit:
    return DeferredLightingDisplayMode::Lit;
  case container::ui::GBufferViewMode::Transparency:
    return DeferredLightingDisplayMode::Transparency;
  case container::ui::GBufferViewMode::Revealage:
    return DeferredLightingDisplayMode::Revealage;
  case container::ui::GBufferViewMode::SurfaceNormals:
    return DeferredLightingDisplayMode::SurfaceNormals;
  case container::ui::GBufferViewMode::ObjectSpaceNormals:
    return DeferredLightingDisplayMode::ObjectSpaceNormals;
  default:
    return DeferredLightingDisplayMode::Other;
  }
}

VkDescriptorSet deferredRasterSceneDescriptorSet(const FrameRecordParams &p) {
  return deferredRasterDescriptorSet(p, DeferredRasterDescriptorSetId::Scene);
}

VkDescriptorSet
deferredRasterBimSceneDescriptorSet(const FrameRecordParams &p) {
  return deferredRasterDescriptorSet(p,
                                     DeferredRasterDescriptorSetId::BimScene);
}

VkDescriptorSet
deferredRasterTiledLightingDescriptorSet(const FrameRecordParams &p) {
  return deferredRasterDescriptorSet(
      p, DeferredRasterDescriptorSetId::TiledLighting);
}

DeferredLightingWireframeSettings deferredLightingWireframeSettings(
    const container::ui::WireframeSettings &settings) {
  return {.enabled = settings.enabled,
          .mode = settings.mode == container::ui::WireframeMode::Full
                      ? DeferredLightingWireframeMode::Full
                      : DeferredLightingWireframeMode::Overlay,
          .depthTest = settings.depthTest,
          .color = settings.color,
          .lineWidth = settings.lineWidth,
          .overlayIntensity = settings.overlayIntensity};
}

DeferredLightingFrameInputs deferredLightingFrameInputs(
    const FrameRecordParams &p, const container::ui::GuiManager *guiManager,
    container::ui::GBufferViewMode fallbackDisplayMode, bool tileCullPassActive,
    bool tiledLightingReady, uint32_t pointLightCount) {
  DeferredLightingFrameInputs inputs{};
  inputs.displayMode = deferredLightingDisplayMode(
      currentDisplayMode(guiManager, fallbackDisplayMode));
  inputs.guiAvailable = guiManager != nullptr;
  inputs.wireframeSupported = guiManager != nullptr &&
                              guiManager->editorOverlaysEnabled() &&
                              guiManager->wireframeSupported();
  inputs.wireframeWideLinesSupported = p.debug.wireframeWideLinesSupported;
  if (guiManager != nullptr) {
    inputs.wireframeSettings =
        deferredLightingWireframeSettings(guiManager->wireframeSettings());
    inputs.normalValidationSettings = guiManager->normalValidationSettings();
    inputs.geometryOverlayRequested = guiManager->showGeometryOverlay();
    if (!guiManager->editorOverlaysEnabled()) {
      inputs.wireframeSettings.enabled = false;
      inputs.normalValidationSettings.enabled = false;
      inputs.geometryOverlayRequested = false;
    }
  }
  inputs.bimTechnicalElevation = {
      .enabled = p.bim.technicalElevation.enabled,
      .hiddenLineOverlay = p.bim.technicalElevation.hiddenLineOverlay,
      .depthTestLines = p.bim.technicalElevation.depthTestLines,
      .lineColor = p.bim.technicalElevation.lineColor,
      .lineWidth = p.bim.technicalElevation.lineWidth,
      .overlayIntensity = p.bim.technicalElevation.overlayIntensity};
  inputs.debugDirectionalOnly = p.debug.debugDirectionalOnly;
  inputs.debugVisualizePointLightStencil =
      p.debug.debugVisualizePointLightStencil;
  if (guiManager != nullptr && !guiManager->editorOverlaysEnabled()) {
    inputs.debugVisualizePointLightStencil = false;
  }
  inputs.tileCullPassActive = tileCullPassActive;
  inputs.tiledLightingReady = tiledLightingReady;
  inputs.depthSamplingReady =
      deferredRasterImageViewReady(p, DeferredRasterImageId::DepthSamplingView);
  inputs.tiledDescriptorSetReady =
      deferredRasterTiledLightingDescriptorSet(p) != VK_NULL_HANDLE;
  inputs.transparentDrawCommandsAvailable = hasTransparentDrawCommands(p);
  inputs.pointLightCount = pointLightCount;
  inputs.pipelines = {.directionalLight = deferredRasterPipelineReady(
                          p, DeferredRasterPipelineId::DirectionalLight),
                      .wireframeDepth = deferredRasterPipelineReady(
                          p, DeferredRasterPipelineId::WireframeDepth),
                      .wireframeNoDepth = deferredRasterPipelineReady(
                          p, DeferredRasterPipelineId::WireframeNoDepth),
                      .objectNormalDebug = deferredRasterPipelineReady(
                          p, DeferredRasterPipelineId::ObjectNormalDebug),
                      .normalValidation = deferredRasterPipelineReady(
                          p, DeferredRasterPipelineId::NormalValidation),
                      .surfaceNormalLine = deferredRasterPipelineReady(
                          p, DeferredRasterPipelineId::SurfaceNormalLine),
                      .geometryDebug = deferredRasterPipelineReady(
                          p, DeferredRasterPipelineId::GeometryDebug),
                      .tiledPointLight = deferredRasterPipelineReady(
                          p, DeferredRasterPipelineId::TiledPointLight),
                      .pointLight = deferredRasterPipelineReady(
                          p, DeferredRasterPipelineId::PointLight),
                      .pointLightStencilDebug = deferredRasterPipelineReady(
                          p, DeferredRasterPipelineId::PointLightStencilDebug),
                      .stencilVolume = deferredRasterPipelineReady(
                          p, DeferredRasterPipelineId::StencilVolume)};
  return inputs;
}

bool hasBimRenderableGeometry(const FrameRecordParams &p) {
  const auto &bimScene = p.bim.scene;
  bool hasRenderable = false;
  for (const FrameDrawLists *draws : bimSurfaceDrawListSet(p.bim)) {
    hasRenderable = hasRenderable || hasRenderableDrawCommands(*draws);
  }
  return deferredRasterBimSceneDescriptorSet(p) != VK_NULL_HANDLE &&
         bimScene.vertexSlice.buffer != VK_NULL_HANDLE &&
         bimScene.indexSlice.buffer != VK_NULL_HANDLE && hasRenderable;
}

BimPrimitivePassDrawLists primitivePassDrawLists(const FrameDrawLists &draws) {
  return {
      .opaqueDrawCommands = draws.opaqueDrawCommands,
      .opaqueSingleSidedDrawCommands = draws.opaqueSingleSidedDrawCommands,
      .opaqueWindingFlippedDrawCommands =
          draws.opaqueWindingFlippedDrawCommands,
      .opaqueDoubleSidedDrawCommands = draws.opaqueDoubleSidedDrawCommands,
      .transparentDrawCommands = draws.transparentDrawCommands,
      .transparentSingleSidedDrawCommands =
          draws.transparentSingleSidedDrawCommands,
      .transparentWindingFlippedDrawCommands =
          draws.transparentWindingFlippedDrawCommands,
      .transparentDoubleSidedDrawCommands =
          draws.transparentDoubleSidedDrawCommands,
  };
}

BimSurfaceDrawLists surfaceDrawLists(const FrameDrawLists &draws) {
  return {
      .opaqueDrawCommands = draws.opaqueDrawCommands,
      .opaqueSingleSidedDrawCommands = draws.opaqueSingleSidedDrawCommands,
      .opaqueWindingFlippedDrawCommands =
          draws.opaqueWindingFlippedDrawCommands,
      .opaqueDoubleSidedDrawCommands = draws.opaqueDoubleSidedDrawCommands,
      .transparentDrawCommands = draws.transparentDrawCommands,
      .transparentSingleSidedDrawCommands =
          draws.transparentSingleSidedDrawCommands,
      .transparentWindingFlippedDrawCommands =
          draws.transparentWindingFlippedDrawCommands,
      .transparentDoubleSidedDrawCommands =
          draws.transparentDoubleSidedDrawCommands,
  };
}

BimSurfaceFramePassDrawSources
bimSurfaceFramePassDrawSources(const FrameBimResources &bim) {
  return {.mesh = surfaceDrawLists(bim.draws),
          .pointPlaceholders = surfaceDrawLists(bim.pointDraws),
          .curvePlaceholders = surfaceDrawLists(bim.curveDraws),
          .opaqueMeshDrawsUseGpuVisibility =
              bim.opaqueMeshDrawsUseGpuVisibility,
          .transparentMeshDrawsUseGpuVisibility =
              bim.transparentMeshDrawsUseGpuVisibility};
}

BimPrimitivePassGeometryBinding
bimPrimitivePassGeometryBinding(const FrameRecordParams &p) {
  return {.sceneDescriptorSet = deferredRasterBimSceneDescriptorSet(p),
          .vertexSlice = p.bim.scene.vertexSlice,
          .indexSlice = p.bim.scene.indexSlice,
          .indexType = p.bim.scene.indexType};
}

BimSectionClipCapPassGeometryBinding
bimSectionClipCapGeometryBinding(const FrameRecordParams &p) {
  return {.sceneDescriptorSet = deferredRasterBimSceneDescriptorSet(p),
          .vertexSlice = p.bim.sectionClipCapGeometry.scene.vertexSlice,
          .indexSlice = p.bim.sectionClipCapGeometry.scene.indexSlice,
          .indexType = p.bim.sectionClipCapGeometry.scene.indexType};
}

BimPrimitiveFramePassStyle
bimPointCloudPrimitivePassStyle(const FrameRecordParams &p) {
  const auto &pass = p.bim.primitivePasses.pointCloud;
  return {.kind = BimPrimitivePassKind::Points,
          .enabled = pass.enabled,
          .depthTest = pass.depthTest,
          .placeholderRangePreviewEnabled = pass.placeholderRangePreviewEnabled,
          .nativeDrawsUseGpuVisibility = p.bim.nativePointDrawsUseGpuVisibility,
          .opacity = pass.opacity,
          .primitiveSize = pass.pointSize,
          .color = pass.color,
          .recordLineWidth = false,
          .wideLinesSupported = false};
}

BimPrimitiveFramePassStyle
bimCurvePrimitivePassStyle(const FrameRecordParams &p) {
  const auto &pass = p.bim.primitivePasses.curves;
  return {.kind = BimPrimitivePassKind::Curves,
          .enabled = pass.enabled,
          .depthTest = pass.depthTest,
          .placeholderRangePreviewEnabled = pass.placeholderRangePreviewEnabled,
          .nativeDrawsUseGpuVisibility = p.bim.nativeCurveDrawsUseGpuVisibility,
          .opacity = pass.opacity,
          .primitiveSize = pass.lineWidth,
          .color = pass.color,
          .recordLineWidth = true,
          .wideLinesSupported = p.debug.wireframeWideLinesSupported};
}

BimSectionClipCapFramePassStyle
bimSectionClipCapFramePassStyle(const FrameRecordParams &p) {
  const auto &style = p.bim.sectionClipCaps;
  const auto &technicalElevation = p.bim.technicalElevation;
  const bool technicalCapsEnabled =
      technicalElevation.enabled && technicalElevation.sectionCapsEnabled;
  // Technical elevation views reuse the section-cap renderer instead of
  // creating a drawing-specific cap pass. When the base cap preview is off,
  // the technical style can still request fill/hatch for clipped elevations.
  const bool sectionCapEnabled = style.enabled || technicalCapsEnabled;
  return {.enabled = sectionCapEnabled,
          .fillEnabled = technicalCapsEnabled
                             ? technicalElevation.capFillEnabled
                             : style.fillEnabled,
          .hatchEnabled = technicalCapsEnabled
                              ? technicalElevation.capHatchingEnabled
                              : style.hatchEnabled,
          .wideLinesSupported = p.debug.wireframeWideLinesSupported,
          .fillColor = style.fillColor,
          .hatchColor = style.hatchColor,
          .hatchLineWidth =
              technicalCapsEnabled
                  ? std::max(style.hatchLineWidth, technicalElevation.lineWidth)
                  : style.hatchLineWidth,
          .fillDrawCommands = p.bim.sectionClipCapGeometry.fillDrawCommands,
          .hatchDrawCommands = p.bim.sectionClipCapGeometry.hatchDrawCommands,
          .fillDrawStyles = p.bim.sectionClipCapGeometry.fillDrawStyles,
          .hatchDrawStyles = p.bim.sectionClipCapGeometry.hatchDrawStyles,
          .sectionMarkerLines =
              p.bim.sectionClipCapGeometry.sectionMarkerLines};
}

SceneTransparentDrawLists
sceneTransparentDrawLists(const FrameDrawLists &draws) {
  return {
      .aggregate = draws.transparentDrawCommands,
      .singleSided = draws.transparentSingleSidedDrawCommands,
      .windingFlipped = draws.transparentWindingFlippedDrawCommands,
      .doubleSided = draws.transparentDoubleSidedDrawCommands,
  };
}

DeferredDebugOverlayDrawLists
deferredDebugOverlayDrawLists(const FrameDrawLists &draws) {
  return {
      .opaqueDrawCommands = draws.opaqueDrawCommands,
      .transparentDrawCommands = draws.transparentDrawCommands,
      .opaqueSingleSidedDrawCommands = draws.opaqueSingleSidedDrawCommands,
      .transparentSingleSidedDrawCommands =
          draws.transparentSingleSidedDrawCommands,
      .opaqueWindingFlippedDrawCommands =
          draws.opaqueWindingFlippedDrawCommands,
      .transparentWindingFlippedDrawCommands =
          draws.transparentWindingFlippedDrawCommands,
      .opaqueDoubleSidedDrawCommands = draws.opaqueDoubleSidedDrawCommands,
      .transparentDoubleSidedDrawCommands =
          draws.transparentDoubleSidedDrawCommands,
  };
}

DeferredDebugOverlayFrameState
deferredDebugOverlayFrameState(const DeferredLightingFrameState &state) {
  return {.wireframeFullMode = state.wireframeFullMode,
          .wireframeOverlayMode = state.wireframeOverlayMode,
          .objectSpaceNormalsEnabled = state.objectSpaceNormalsEnabled,
          .geometryOverlayEnabled = state.geometryOverlayEnabled,
          .normalValidationEnabled = state.normalValidationEnabled,
          .surfaceNormalLinesEnabled = state.surfaceNormalLinesEnabled,
          .wireframeDepthTest = state.wireframeSettings.depthTest,
          .wireframeLineWidth = state.wireframeSettings.lineWidth,
          .surfaceNormalLineWidth = state.surfaceNormalLineWidth};
}

bool frameDebugGeometryReady(VkDescriptorSet descriptorSet,
                             const FrameSceneGeometry &scene) {
  return descriptorSet != VK_NULL_HANDLE &&
         scene.vertexSlice.buffer != VK_NULL_HANDLE &&
         scene.indexSlice.buffer != VK_NULL_HANDLE;
}

DeferredDebugOverlayInputs
deferredDebugOverlayInputs(const FrameRecordParams &p,
                           const DeferredLightingFrameState &lightingState) {
  DeferredDebugOverlayInputs inputs{};
  inputs.frameState = deferredDebugOverlayFrameState(lightingState);
  inputs.pipelines = {
      .wireframeDepth = deferredRasterPipelineReady(
          p, DeferredRasterPipelineId::WireframeDepth),
      .wireframeNoDepth = deferredRasterPipelineReady(
          p, DeferredRasterPipelineId::WireframeNoDepth),
      .wireframeDepthFrontCull = deferredRasterPipelineReady(
          p, DeferredRasterPipelineId::WireframeDepthFrontCull),
      .wireframeNoDepthFrontCull = deferredRasterPipelineReady(
          p, DeferredRasterPipelineId::WireframeNoDepthFrontCull),
      .objectNormalDebug = deferredRasterPipelineReady(
          p, DeferredRasterPipelineId::ObjectNormalDebug),
      .objectNormalDebugFrontCull = deferredRasterPipelineReady(
          p, DeferredRasterPipelineId::ObjectNormalDebugFrontCull),
      .objectNormalDebugNoCull = deferredRasterPipelineReady(
          p, DeferredRasterPipelineId::ObjectNormalDebugNoCull),
      .geometryDebug = deferredRasterPipelineReady(
          p, DeferredRasterPipelineId::GeometryDebug),
      .normalValidation = deferredRasterPipelineReady(
          p, DeferredRasterPipelineId::NormalValidation),
      .normalValidationFrontCull = deferredRasterPipelineReady(
          p, DeferredRasterPipelineId::NormalValidationFrontCull),
      .normalValidationNoCull = deferredRasterPipelineReady(
          p, DeferredRasterPipelineId::NormalValidationNoCull),
      .surfaceNormalLine = deferredRasterPipelineReady(
          p, DeferredRasterPipelineId::SurfaceNormalLine),
  };
  inputs.bindlessPushConstantsReady = p.pushConstants.bindless != nullptr;
  inputs.wireframePushConstantsReady = p.pushConstants.wireframe != nullptr;
  inputs.normalValidationPushConstantsReady =
      p.pushConstants.normalValidation != nullptr;
  inputs.surfaceNormalPushConstantsReady =
      p.pushConstants.surfaceNormal != nullptr;

  const bool sceneGeometryReady =
      frameDebugGeometryReady(deferredRasterSceneDescriptorSet(p), p.scene);
  inputs.sources[inputs.sourceCount++] = {
      .source = DeferredDebugOverlaySource::Scene,
      .geometryReady = sceneGeometryReady,
      .drawDiagnosticCube = true,
      .diagnosticCubeObjectIndex = p.scene.diagCubeObjectIndex,
      .draws = deferredDebugOverlayDrawLists(p.draws)};

  const bool bimGeometryReady =
      frameDebugGeometryReady(deferredRasterBimSceneDescriptorSet(p),
                              p.bim.scene) &&
      hasBimRenderableGeometry(p);
  const std::array<DeferredDebugOverlaySource, 3> bimSources = {
      DeferredDebugOverlaySource::BimMesh,
      DeferredDebugOverlaySource::BimPointPlaceholders,
      DeferredDebugOverlaySource::BimCurvePlaceholders};
  const auto bimDrawLists = bimSurfaceDrawListSet(p.bim);
  for (uint32_t sourceIndex = 0; sourceIndex < bimSources.size();
       ++sourceIndex) {
    inputs.sources[inputs.sourceCount++] = {
        .source = bimSources[sourceIndex],
        .geometryReady = bimGeometryReady,
        .draws = deferredDebugOverlayDrawLists(*bimDrawLists[sourceIndex])};
  }

  return inputs;
}

DeferredDebugOverlayPipelineHandles
deferredDebugOverlayPipelineHandles(const FrameRecordParams &p) {
  return {
      .wireframeDepth = deferredRasterPipelineHandle(
          p, DeferredRasterPipelineId::WireframeDepth),
      .wireframeNoDepth = deferredRasterPipelineHandle(
          p, DeferredRasterPipelineId::WireframeNoDepth),
      .wireframeDepthFrontCull = deferredRasterPipelineHandle(
          p, DeferredRasterPipelineId::WireframeDepthFrontCull),
      .wireframeNoDepthFrontCull = deferredRasterPipelineHandle(
          p, DeferredRasterPipelineId::WireframeNoDepthFrontCull),
      .objectNormalDebug = deferredRasterPipelineHandle(
          p, DeferredRasterPipelineId::ObjectNormalDebug),
      .objectNormalDebugFrontCull = deferredRasterPipelineHandle(
          p, DeferredRasterPipelineId::ObjectNormalDebugFrontCull),
      .objectNormalDebugNoCull = deferredRasterPipelineHandle(
          p, DeferredRasterPipelineId::ObjectNormalDebugNoCull),
      .geometryDebug = deferredRasterPipelineHandle(
          p, DeferredRasterPipelineId::GeometryDebug),
      .normalValidation = deferredRasterPipelineHandle(
          p, DeferredRasterPipelineId::NormalValidation),
      .normalValidationFrontCull = deferredRasterPipelineHandle(
          p, DeferredRasterPipelineId::NormalValidationFrontCull),
      .normalValidationNoCull = deferredRasterPipelineHandle(
          p, DeferredRasterPipelineId::NormalValidationNoCull),
      .surfaceNormalLine = deferredRasterPipelineHandle(
          p, DeferredRasterPipelineId::SurfaceNormalLine),
  };
}

DeferredDebugOverlayGeometryBinding
deferredDebugOverlayGeometryBinding(VkDescriptorSet descriptorSet,
                                    const FrameSceneGeometry &scene) {
  return {.descriptorSet = descriptorSet,
          .vertexSlice = scene.vertexSlice,
          .indexSlice = scene.indexSlice,
          .indexType = scene.indexType};
}

DeferredDebugOverlayDiagnosticGeometry
deferredDebugOverlayDiagnosticGeometry(const SceneController *sceneController) {
  if (sceneController == nullptr) {
    return {};
  }
  return {.vertexSlice = sceneController->diagCubeVertexSlice(),
          .indexSlice = sceneController->diagCubeIndexSlice(),
          .indexCount = sceneController->diagCubeIndexCount()};
}

BimLightingOverlayDrawLists
bimLightingOverlayDrawLists(const FrameDrawLists &draws) {
  return {
      .opaqueSingleSidedDrawCommands = draws.opaqueSingleSidedDrawCommands,
      .transparentSingleSidedDrawCommands =
          draws.transparentSingleSidedDrawCommands,
      .opaqueWindingFlippedDrawCommands =
          draws.opaqueWindingFlippedDrawCommands,
      .transparentWindingFlippedDrawCommands =
          draws.transparentWindingFlippedDrawCommands,
      .opaqueDoubleSidedDrawCommands = draws.opaqueDoubleSidedDrawCommands,
      .transparentDoubleSidedDrawCommands =
          draws.transparentDoubleSidedDrawCommands,
  };
}

BimLightingOverlayFrameStyleState
bimLightingOverlayPointFrameStyle(const FrameRecordParams &p) {
  return {.enabled = p.bim.pointCurveStyle.points.enabled,
          .depthTest = p.bim.pointCurveStyle.points.depthTest,
          .color = p.bim.pointCurveStyle.points.color,
          .opacity = p.bim.pointCurveStyle.points.opacity,
          .lineWidth = p.bim.pointCurveStyle.points.pointSize};
}

BimLightingOverlayFrameStyleState
bimLightingOverlayCurveFrameStyle(const FrameRecordParams &p) {
  return {.enabled = p.bim.pointCurveStyle.curves.enabled,
          .depthTest = p.bim.pointCurveStyle.curves.depthTest,
          .color = p.bim.pointCurveStyle.curves.color,
          .opacity = p.bim.pointCurveStyle.curves.opacity,
          .lineWidth = p.bim.pointCurveStyle.curves.lineWidth};
}

BimLightingOverlayFrameStyleState
bimLightingOverlayFloorPlanFrameStyle(const FrameRecordParams &p) {
  return {.enabled = p.bim.floorPlan.enabled,
          .depthTest = p.bim.floorPlan.depthTest,
          .color = p.bim.floorPlan.color,
          .opacity = p.bim.floorPlan.opacity,
          .lineWidth = p.bim.floorPlan.lineWidth};
}

BimLightingOverlayFrameStyleState
bimLightingOverlayIssueMarkerFrameStyle(const FrameRecordParams &p) {
  return {.enabled = p.bim.coordinationMarkers.issueMarkersEnabled,
          .depthTest = p.bim.coordinationMarkers.depthTest,
          .color = p.bim.coordinationMarkers.issueColor,
          .opacity = p.bim.coordinationMarkers.issueOpacity,
          .lineWidth = p.bim.coordinationMarkers.issueLineWidth};
}

BimLightingOverlayFrameStyleState
bimLightingOverlayClashMarkerFrameStyle(const FrameRecordParams &p) {
  return {.enabled = p.bim.coordinationMarkers.clashMarkersEnabled,
          .depthTest = p.bim.coordinationMarkers.depthTest,
          .color = p.bim.coordinationMarkers.clashColor,
          .opacity = p.bim.coordinationMarkers.clashOpacity,
          .lineWidth = p.bim.coordinationMarkers.clashLineWidth};
}

BimLightingOverlayFrameStyleState
bimLightingOverlaySectionPlaneVisualFrameStyle(const FrameRecordParams &p) {
  return {.enabled = p.bim.sectionPlaneVisual.enabled,
          .depthTest = p.bim.sectionPlaneVisual.depthTest,
          .color = p.bim.sectionPlaneVisual.color,
          .opacity = p.bim.sectionPlaneVisual.opacity,
          .lineWidth = p.bim.sectionPlaneVisual.lineWidth};
}

BimLightingOverlayFrameDrawSources
bimLightingOverlayFrameDrawSources(const FrameRecordParams &p) {
  return {
      .points = bimLightingOverlayDrawLists(p.bim.pointDraws),
      .curves = bimLightingOverlayDrawLists(p.bim.curveDraws),
      .floorPlan = p.bim.floorPlanDrawCommands,
      .sceneHover = p.draws.hoveredDrawCommands,
      .bimHover = p.bim.draws.hoveredDrawCommands,
      .sceneSelection = p.draws.selectedDrawCommands,
      .bimSelection = p.bim.draws.selectedDrawCommands,
      .nativePointHover = p.bim.nativePointDraws.hoveredDrawCommands,
      .nativeCurveHover = p.bim.nativeCurveDraws.hoveredDrawCommands,
      .nativePointSelection = p.bim.nativePointDraws.selectedDrawCommands,
      .nativeCurveSelection = p.bim.nativeCurveDraws.selectedDrawCommands,
      .coordinationIssueMarkers = p.bim.coordinationIssueMarkerDrawCommands,
      .coordinationClashMarkers = p.bim.coordinationClashMarkerDrawCommands,
      .sectionPlaneVisual = p.bim.sectionPlaneVisualDrawCommands,
  };
}

BimLightingOverlayPipelineHandles
bimLightingOverlayPipelineHandles(const FrameRecordParams &p) {
  return {
      .wireframeDepth = deferredRasterPipelineHandle(
          p, DeferredRasterPipelineId::WireframeDepth),
      .wireframeNoDepth = deferredRasterPipelineHandle(
          p, DeferredRasterPipelineId::WireframeNoDepth),
      .wireframeDepthFrontCull = deferredRasterPipelineHandle(
          p, DeferredRasterPipelineId::WireframeDepthFrontCull),
      .wireframeNoDepthFrontCull = deferredRasterPipelineHandle(
          p, DeferredRasterPipelineId::WireframeNoDepthFrontCull),
      .bimFloorPlanDepth = deferredRasterPipelineHandle(
          p, DeferredRasterPipelineId::BimFloorPlanDepth),
      .bimFloorPlanNoDepth = deferredRasterPipelineHandle(
          p, DeferredRasterPipelineId::BimFloorPlanNoDepth),
      .bimPointCloudDepth = deferredRasterPipelineHandle(
          p, DeferredRasterPipelineId::BimPointCloudDepth),
      .bimCurveDepth = deferredRasterPipelineHandle(
          p, DeferredRasterPipelineId::BimCurveDepth),
      .selectionMask = deferredRasterPipelineHandle(
          p, DeferredRasterPipelineId::SelectionMask),
      .selectionOutline = deferredRasterPipelineHandle(
          p, DeferredRasterPipelineId::SelectionOutline),
  };
}

BimLightingOverlayGeometryBinding
bimLightingOverlayGeometryBinding(VkDescriptorSet descriptorSet,
                                  const FrameSceneGeometry &scene) {
  return {.descriptorSet = descriptorSet,
          .vertexSlice = scene.vertexSlice,
          .indexSlice = scene.indexSlice,
          .indexType = scene.indexType};
}

void syncOverlaySectionPlanePushConstants(
    const FramePushConstantState &pushConstants) {
  const uint32_t enabled = pushConstants.bindless != nullptr
                               ? pushConstants.bindless->sectionPlaneEnabled
                               : 0u;
  const glm::vec4 plane = pushConstants.bindless != nullptr
                              ? pushConstants.bindless->sectionPlane
                              : glm::vec4{0.0f, 1.0f, 0.0f, 0.0f};

  if (pushConstants.wireframe != nullptr) {
    pushConstants.wireframe->sectionPlaneEnabled = enabled;
    pushConstants.wireframe->sectionPlane = plane;
  }
  if (pushConstants.normalValidation != nullptr) {
    pushConstants.normalValidation->sectionPlaneEnabled = enabled;
    pushConstants.normalValidation->sectionPlane = plane;
  }
  if (pushConstants.surfaceNormal != nullptr) {
    pushConstants.surfaceNormal->sectionPlaneEnabled = enabled;
    pushConstants.surfaceNormal->sectionPlane = plane;
  }
}

} // namespace

DeferredRasterLightingPassRecorder::DeferredRasterLightingPassRecorder(
    DeferredRasterLightingPassServices services)
    : services_(services) {}

void DeferredRasterLightingPassRecorder::record(
    VkCommandBuffer cmd, const FrameRecordParams &p, VkDescriptorSet sceneSet,
    const std::array<VkDescriptorSet, 2> &lightingDescriptorSets,
    const std::array<VkDescriptorSet, 4> &transparentDescriptorSets) const {
  const LightingManager *lightingManager = services_.lightingManager;
  const uint32_t pointLightCount =
      lightingManager
          ? static_cast<uint32_t>(lightingManager->pointLightsSsbo().size())
          : 0u;
  const auto fallbackDisplayMode = services_.fallbackDisplayMode.value_or(
      container::ui::GBufferViewMode::Overview);
  const DeferredLightingFrameInputs lightingInputs =
      deferredLightingFrameInputs(p, services_.guiManager, fallbackDisplayMode,
                                  services_.tileCullPassActive,
                                  lightingManager &&
                                      lightingManager->isTiledLightingReady(),
                                  pointLightCount);
  const DeferredLightingFrameState lightingState =
      buildDeferredLightingFrameState(lightingInputs);
  const auto &wireframeSettings = lightingState.wireframeSettings;
  const auto &normalValidationSettings = lightingState.normalValidationSettings;
  const bool transparentOitEnabled = lightingState.transparentOitEnabled;
  syncOverlaySectionPlanePushConstants(p.pushConstants);
  const DeferredDebugOverlayPlan debugOverlayPlan =
      buildDeferredDebugOverlayPlan(
          deferredDebugOverlayInputs(p, lightingState));

  const VkExtent2D lightingExtent = services_.framebufferExtent;
  const DeferredLightingPassPlan lightingPassPlan =
      buildDeferredLightingPassPlan(lightingExtent);
  const DeferredLightingDescriptorPlan lightingDescriptorPlan =
      buildDeferredLightingDescriptorPlan(
          {.lightingDescriptorSets = lightingDescriptorSets,
           .frameLightingDescriptorSet = deferredRasterDescriptorSet(
               p, DeferredRasterDescriptorSetId::FrameLighting),
           .tiledDescriptorSet = deferredRasterTiledLightingDescriptorSet(p),
           .sceneDescriptorSet = sceneSet});

  if (!recordRenderPassBeginCommands(
          cmd, {.renderPass = deferredRasterRenderPass(
                    p, DeferredRasterFramebufferId::Lighting),
                .framebuffer = deferredRasterFramebuffer(
                    p, DeferredRasterFramebufferId::Lighting),
                .renderArea = lightingPassPlan.renderArea,
                .clearValues = lightingPassPlan.clearValues})) {
    return;
  }
  recordSceneViewportAndScissor(cmd, lightingExtent);

  const DeferredDebugOverlayRecordInputs debugOverlayRecordInputs = {
      .plan = &debugOverlayPlan,
      .pipelines = deferredDebugOverlayPipelineHandles(p),
      .sceneLayout = deferredRasterPipelineLayout(
          p, DeferredRasterPipelineLayoutId::Scene),
      .wireframeLayout = deferredRasterPipelineLayout(
          p, DeferredRasterPipelineLayoutId::Wireframe),
      .normalValidationLayout = deferredRasterPipelineLayout(
          p, DeferredRasterPipelineLayoutId::NormalValidation),
      .surfaceNormalLayout = deferredRasterPipelineLayout(
          p, DeferredRasterPipelineLayoutId::SurfaceNormal),
      .scene = deferredDebugOverlayGeometryBinding(
          deferredRasterSceneDescriptorSet(p), p.scene),
      .bim = deferredDebugOverlayGeometryBinding(
          deferredRasterBimSceneDescriptorSet(p), p.bim.scene),
      .diagnostic =
          deferredDebugOverlayDiagnosticGeometry(services_.sceneController),
      .bindlessPushConstants = p.pushConstants.bindless,
      .wireframePushConstants = p.pushConstants.wireframe,
      .normalValidationPushConstants = p.pushConstants.normalValidation,
      .surfaceNormalPushConstants = p.pushConstants.surfaceNormal,
      .normalValidationSettings = normalValidationSettings,
      .wireframeColor = wireframeSettings.color,
      .wireframeIntensity = lightingState.wireframeIntensity,
      .wireframeLineWidth = wireframeSettings.lineWidth,
      .wireframeRasterModeSupported = p.debug.wireframeRasterModeSupported,
      .wireframeWideLinesSupported = p.debug.wireframeWideLinesSupported,
      .debugOverlay = services_.debugOverlay};

  if (lightingState.wireframeFullMode) {
    (void)recordDeferredDebugOverlayWireframeFullCommands(
        cmd, debugOverlayRecordInputs);
  } else if (lightingState.objectSpaceNormalsEnabled) {
    (void)recordDeferredDebugOverlayObjectNormalCommands(
        cmd, debugOverlayRecordInputs);
  } else if (lightingState.directionalLightingEnabled) {
    (void)recordDeferredDirectionalLightingCommands(
        cmd, {.pipeline = deferredRasterPipelineHandle(
                  p, DeferredRasterPipelineId::DirectionalLight),
              .pipelineLayout = deferredRasterPipelineLayout(
                  p, DeferredRasterPipelineLayoutId::Lighting),
              .descriptorSets =
                  lightingDescriptorPlan.directionalLightingDescriptorSets});
  }

  const std::vector<container::gpu::PointLightData> *pointLights =
      lightingManager ? &lightingManager->pointLightsSsbo() : nullptr;
  const uint32_t contactVisibilityEnabled =
      p.shadows.shadowSettings.localContactVisibility ? 1u : 0u;
  const uint32_t localShadowEnabled =
      (lightingManager != nullptr &&
       lightingManager->lightingData().localShadowEnabled != 0u &&
       p.shadows.localShadowData != nullptr &&
       p.shadows.localShadowData->counts.w != 0u &&
       p.shadows.localShadowLayerCount != 0u)
          ? 1u
          : 0u;
  const DeferredPointLightingDrawPlan pointLightingPlan =
      buildDeferredPointLightingDrawPlan(
          {.state = lightingState.pointLighting,
           .debugVisualizePointLightStencil =
               lightingInputs.debugVisualizePointLightStencil,
           .framebufferWidth = lightingExtent.width,
           .framebufferHeight = lightingExtent.height,
           .cameraNear = p.camera.nearPlane,
           .cameraFar = p.camera.farPlane,
           .contactVisibilityEnabled = contactVisibilityEnabled,
           .localShadowEnabled = localShadowEnabled,
           .bounceIntensity =
               lightingManager ? lightingManager->lightingData().bounceIntensity
                               : 0.0f,
           .pointLights =
               pointLights != nullptr
                   ? std::span<const container::gpu::PointLightData>(
                         pointLights->data(), pointLights->size())
                   : std::span<const container::gpu::PointLightData>{},
           .lightVolumeIndexCount =
               lightingManager ? lightingManager->lightVolumeIndexCount()
                               : 0u});

  (void)recordDeferredPointLightingCommands(
      cmd, {.plan = &pointLightingPlan,
            .tiledPointLightPipeline = deferredRasterPipelineHandle(
                p, DeferredRasterPipelineId::TiledPointLight),
            .stencilVolumePipeline = deferredRasterPipelineHandle(
                p, DeferredRasterPipelineId::StencilVolume),
            .pointLightPipeline = deferredRasterPipelineHandle(
                p, DeferredRasterPipelineId::PointLight),
            .pointLightStencilDebugPipeline = deferredRasterPipelineHandle(
                p, DeferredRasterPipelineId::PointLightStencilDebug),
            .lightingLayout = deferredRasterPipelineLayout(
                p, DeferredRasterPipelineLayoutId::Lighting),
            .tiledLightingLayout = deferredRasterPipelineLayout(
                p, DeferredRasterPipelineLayoutId::TiledLighting),
            .pointLightingDescriptorSets =
                lightingDescriptorPlan.pointLightingDescriptorSets,
            .tiledLightingDescriptorSets =
                lightingDescriptorPlan.tiledLightingDescriptorSets,
            .framebufferExtent = lightingExtent,
            .lightPushConstants = p.pushConstants.light,
            .lightingManager = lightingManager});

  if (transparentOitEnabled) {
    const bool drawBimTransparent = hasBimTransparentGeometry(p);
    const std::array<VkDescriptorSet, 1> bimDescriptorSets = {
        deferredRasterBimSceneDescriptorSet(p)};
    const BimSurfacePassPlan bimTransparentPlan = buildBimSurfaceFramePassPlan(
        {.kind = BimSurfacePassKind::TransparentLighting,
         .passReady = drawBimTransparent,
         .draws = bimSurfaceFramePassDrawSources(p.bim),
         .geometry = {.descriptorSets = bimDescriptorSets,
                      .vertexSlice = p.bim.scene.vertexSlice,
                      .indexSlice = p.bim.scene.indexSlice,
                      .indexType = p.bim.scene.indexType},
         .pipelines = {.singleSided = deferredRasterPipelineHandle(
                           p, DeferredRasterPipelineId::Transparent)},
         .pushConstants = p.pushConstants.bindless,
         .semanticColorMode = p.bim.semanticColorMode});
    const SceneTransparentDrawPlan transparentPlan =
        buildSceneTransparentDrawPlan(sceneTransparentDrawLists(p.draws));
    (void)recordDeferredTransparentOitCommands(
        cmd,
        {.scenePlan = &transparentPlan,
         .bimPlan = &bimTransparentPlan,
         .descriptorSets = transparentDescriptorSets,
         .scene = {.descriptorSet = deferredRasterSceneDescriptorSet(p),
                   .vertexSlice = p.scene.vertexSlice,
                   .indexSlice = p.scene.indexSlice,
                   .indexType = p.scene.indexType},
         .bim = {.descriptorSet = deferredRasterBimSceneDescriptorSet(p),
                 .vertexSlice = p.bim.scene.vertexSlice,
                 .indexSlice = p.bim.scene.indexSlice,
                 .indexType = p.bim.scene.indexType},
         .pipelines = {.primary = deferredRasterPipelineHandle(
                           p, DeferredRasterPipelineId::Transparent),
                       .frontCull = deferredRasterPipelineHandle(
                           p, DeferredRasterPipelineId::TransparentFrontCull),
                       .noCull = deferredRasterPipelineHandle(
                           p, DeferredRasterPipelineId::TransparentNoCull)},
         .pipelineLayout = deferredRasterPipelineLayout(
             p, DeferredRasterPipelineLayoutId::Transparent),
         .pushConstants = *p.pushConstants.bindless,
         .debugOverlay = services_.debugOverlay,
         .bimManager = p.services.bimManager});
  }

  (void)recordDeferredDebugOverlayGeometryCommands(cmd,
                                                   debugOverlayRecordInputs);
  (void)recordDeferredDebugOverlayNormalValidationCommands(
      cmd, debugOverlayRecordInputs);
  (void)recordDeferredDebugOverlaySurfaceNormalCommands(
      cmd, debugOverlayRecordInputs);
  if (lightingState.wireframeOverlayMode) {
    (void)recordDeferredDebugOverlayWireframeOverlayCommands(
        cmd, debugOverlayRecordInputs);
  }

  (void)recordBimSectionClipCapFramePassCommands(
      cmd, {.style = bimSectionClipCapFramePassStyle(p),
            .geometry = bimSectionClipCapGeometryBinding(p),
            .fillPipeline = deferredRasterPipelineHandle(
                p, DeferredRasterPipelineId::BimSectionClipCapFill),
            .hatchPipeline = deferredRasterPipelineHandle(
                p, DeferredRasterPipelineId::BimSectionClipCapHatch),
            .wireframeLayout = deferredRasterPipelineLayout(
                p, DeferredRasterPipelineLayoutId::Wireframe),
            .pushConstants = p.pushConstants.wireframe,
            .debugOverlay = services_.debugOverlay});
  (void)recordBimPrimitiveFramePassCommands(
      cmd,
      {.style = bimPointCloudPrimitivePassStyle(p),
       .placeholderDraws = primitivePassDrawLists(p.bim.pointDraws),
       .nativeDraws = primitivePassDrawLists(p.bim.nativePointDraws),
       .geometry = bimPrimitivePassGeometryBinding(p),
       .pipelines = {.depth = deferredRasterPipelineHandle(
                         p, DeferredRasterPipelineId::BimPointCloudDepth),
                     .noDepth = deferredRasterPipelineHandle(
                         p, DeferredRasterPipelineId::BimPointCloudNoDepth)},
       .wireframeLayout = deferredRasterPipelineLayout(
           p, DeferredRasterPipelineLayoutId::Wireframe),
       .pushConstants = p.pushConstants.wireframe,
       .debugOverlay = services_.debugOverlay,
       .bimManager = p.services.bimManager});
  (void)recordBimPrimitiveFramePassCommands(
      cmd, {.style = bimCurvePrimitivePassStyle(p),
            .placeholderDraws = primitivePassDrawLists(p.bim.curveDraws),
            .nativeDraws = primitivePassDrawLists(p.bim.nativeCurveDraws),
            .geometry = bimPrimitivePassGeometryBinding(p),
            .pipelines = {.depth = deferredRasterPipelineHandle(
                              p, DeferredRasterPipelineId::BimCurveDepth),
                          .noDepth = deferredRasterPipelineHandle(
                              p, DeferredRasterPipelineId::BimCurveNoDepth)},
            .wireframeLayout = deferredRasterPipelineLayout(
                p, DeferredRasterPipelineLayoutId::Wireframe),
            .pushConstants = p.pushConstants.wireframe,
            .debugOverlay = services_.debugOverlay,
            .bimManager = p.services.bimManager});

  (void)recordBimLightingOverlayFrameCommands(
      cmd,
      {.bimGeometryReady = hasBimRenderableGeometry(p),
       .framebufferExtent = lightingExtent,
       .points = bimLightingOverlayPointFrameStyle(p),
       .curves = bimLightingOverlayCurveFrameStyle(p),
       .floorPlan = bimLightingOverlayFloorPlanFrameStyle(p),
       .coordinationIssueMarkers = bimLightingOverlayIssueMarkerFrameStyle(p),
       .coordinationClashMarkers = bimLightingOverlayClashMarkerFrameStyle(p),
       .sectionPlaneVisual = bimLightingOverlaySectionPlaneVisualFrameStyle(p),
       .draws = bimLightingOverlayFrameDrawSources(p),
       .nativePointSize = p.bim.primitivePasses.pointCloud.pointSize,
       .nativeCurveLineWidth = p.bim.primitivePasses.curves.lineWidth,
       .coordinationMarkerGeometryReady =
           p.bim.coordinationMarkerScene.vertexSlice.buffer != VK_NULL_HANDLE &&
           p.bim.coordinationMarkerScene.indexSlice.buffer != VK_NULL_HANDLE,
       .sectionPlaneVisualGeometryReady =
           p.bim.sectionPlaneVisualScene.vertexSlice.buffer != VK_NULL_HANDLE &&
           p.bim.sectionPlaneVisualScene.indexSlice.buffer != VK_NULL_HANDLE,
       .pipelines = bimLightingOverlayPipelineHandles(p),
       .wireframeLayout = deferredRasterPipelineLayout(
           p, DeferredRasterPipelineLayoutId::Wireframe),
       .scene = bimLightingOverlayGeometryBinding(
           deferredRasterSceneDescriptorSet(p), p.scene),
       .bim = bimLightingOverlayGeometryBinding(
           deferredRasterBimSceneDescriptorSet(p), p.bim.scene),
       .coordinationMarkers = bimLightingOverlayGeometryBinding(
           deferredRasterBimSceneDescriptorSet(p),
           p.bim.coordinationMarkerScene),
       .sectionPlaneVisualGeometry = bimLightingOverlayGeometryBinding(
           deferredRasterBimSceneDescriptorSet(p),
           p.bim.sectionPlaneVisualScene),
       .selectionStencilClearAttachment =
           lightingPassPlan.selectionStencilClearAttachment,
       .selectionStencilClearRect = lightingPassPlan.selectionStencilClearRect,
       .wireframePushConstants = p.pushConstants.wireframe,
       .debugOverlay = services_.debugOverlay,
       .wireframeRasterModeSupported = p.debug.wireframeRasterModeSupported,
       .wireframeWideLinesSupported = p.debug.wireframeWideLinesSupported});

  static_cast<void>(recordRenderPassEndCommands(cmd));
}

} // namespace container::renderer
