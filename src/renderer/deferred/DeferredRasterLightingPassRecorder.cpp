#include "Container/renderer/deferred/DeferredRasterLightingPassRecorder.h"

#include "Container/renderer/bim/BimLightingOverlayRecorder.h"
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
#include "Container/renderer/lighting/DdgiManager.h"
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

DeferredLightingFrameInputs deferredLightingFrameInputs(
    const FrameRecordParams &p, const container::ui::GuiManager *guiManager,
    container::ui::GBufferViewMode fallbackDisplayMode, bool tileCullPassActive,
    bool tiledLightingReady, bool clusteredCapacityLimited,
    uint32_t pointLightCount) {
  DeferredLightingFrameInputs inputs{};
  inputs.displayMode = deferredLightingDisplayMode(
      currentDisplayMode(guiManager, fallbackDisplayMode));
  inputs.guiAvailable = guiManager != nullptr;
  inputs.wireframeSupported = p.debug.wireframe.enabled;
  inputs.wireframeSettings = p.debug.wireframe;
  inputs.wireframeWideLinesSupported = p.debug.wireframeWideLinesSupported;
  if (guiManager != nullptr) {
    inputs.normalValidationSettings = guiManager->normalValidationSettings();
    inputs.geometryOverlayRequested = guiManager->showGeometryOverlay();
    if (!guiManager->editorOverlaysEnabled()) {
      inputs.normalValidationSettings.enabled = false;
      inputs.geometryOverlayRequested = false;
    }
  }
  // The frontend enables technical elevation for the editor or a scripted capture.
  inputs.wireframeSupported |= p.bim.technicalElevation.enabled;
  inputs.bimTechnicalElevation = {
      .enabled = p.bim.technicalElevation.enabled && !p.screenshot.sceneLinear,
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
  if (p.screenshot.sceneLinear) {
    inputs.wireframeSupported = false;
    inputs.wireframeSettings.enabled = false;
    inputs.normalValidationSettings.enabled = false;
    inputs.geometryOverlayRequested = false;
    inputs.debugVisualizePointLightStencil = false;
  }
  inputs.tileCullPassActive = tileCullPassActive;
  inputs.tiledLightingReady = tiledLightingReady;
  inputs.clusteredCapacityLimited = clusteredCapacityLimited;
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

SceneTransparentDrawLists
sceneTransparentDrawLists(const FrameDrawLists &draws) {
  return {
      .aggregate = draws.transparentDrawCommands,
      .singleSided = draws.transparentSingleSidedDrawCommands,
      .windingFlipped = draws.transparentWindingFlippedDrawCommands,
      .doubleSided = draws.transparentDoubleSidedDrawCommands,
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
      .draws = debugOverlayDrawLists(p.draws)};

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
        .draws = debugOverlayDrawLists(*bimDrawLists[sourceIndex])};
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
                                  lightingManager && lightingManager->tiledCapacityLimited(),
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
                  lightingDescriptorPlan.directionalLightingDescriptorSets,
              .ddgiDescriptorSet = p.services.ddgiManager
                  ? p.services.ddgiManager->readDescriptors(p.runtime.imageIndex)
                  : p.services.ddgiReadDescriptorSet,
              .ddgiDescriptorsRequired = p.services.ddgiManager == nullptr ||
                  p.services.ddgiManager->descriptorLayout() != VK_NULL_HANDLE});
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
            .ddgiDescriptorSet = p.services.ddgiManager
                ? p.services.ddgiManager->readDescriptors(p.runtime.imageIndex)
                : p.services.ddgiReadDescriptorSet,
            .ddgiDescriptorsRequired = p.services.ddgiManager == nullptr ||
                p.services.ddgiManager->descriptorLayout() != VK_NULL_HANDLE,
            .framebufferExtent = lightingExtent,
            .lightPushConstants = p.pushConstants.light,
            .lightingManager = lightingManager,
            .imageIndex = p.runtime.imageIndex});

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

  recordBimRasterOverlays(
      cmd, p,
      {.sceneDescriptorSet = deferredRasterSceneDescriptorSet(p),
       .bimDescriptorSet = deferredRasterBimSceneDescriptorSet(p),
       .extent = lightingExtent,
       .wireframeLayout = deferredRasterPipelineLayout(
           p, DeferredRasterPipelineLayoutId::Wireframe),
       .pipelines = bimLightingOverlayPipelineHandles(p),
       .pointCloudNoDepth = deferredRasterPipelineHandle(
           p, DeferredRasterPipelineId::BimPointCloudNoDepth),
       .curveNoDepth = deferredRasterPipelineHandle(
           p, DeferredRasterPipelineId::BimCurveNoDepth),
       .capFill = deferredRasterPipelineHandle(
           p, DeferredRasterPipelineId::BimSectionClipCapFill),
       .capHatch = deferredRasterPipelineHandle(
           p, DeferredRasterPipelineId::BimSectionClipCapHatch),
       .wireframePushConstants = p.pushConstants.wireframe,
       .debugOverlay = services_.debugOverlay});

  static_cast<void>(recordRenderPassEndCommands(cmd));
}

} // namespace container::renderer
