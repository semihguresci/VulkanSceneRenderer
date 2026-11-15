#pragma once

#include "Container/renderer/core/FrameRecorder.h"

#include <string_view>

namespace container::renderer {

enum class DeferredRasterPipelineId {
  DepthPrepass,
  DepthPrepassFrontCull,
  DepthPrepassNoCull,
  BimDepthPrepass,
  BimDepthPrepassFrontCull,
  BimDepthPrepassNoCull,
  GBuffer,
  GBufferFrontCull,
  GBufferNoCull,
  BimGBuffer,
  BimGBufferFrontCull,
  BimGBufferNoCull,
  LocalShadowDepth,
  LocalShadowDepthFrontCull,
  LocalShadowDepthNoCull,
  DirectionalLight,
  StencilVolume,
  PointLight,
  PointLightStencilDebug,
  TiledPointLight,
  Transparent,
  TransparentFrontCull,
  TransparentNoCull,
  TransparentPick,
  TransparentPickFrontCull,
  TransparentPickNoCull,
  PostProcess,
  GeometryDebug,
  NormalValidation,
  NormalValidationFrontCull,
  NormalValidationNoCull,
  WireframeDepth,
  WireframeDepthFrontCull,
  WireframeNoDepth,
  WireframeNoDepthFrontCull,
  SelectionMask,
  SelectionOutline,
  BimFloorPlanDepth,
  BimFloorPlanNoDepth,
  BimPointCloudDepth,
  BimPointCloudNoDepth,
  BimCurveDepth,
  BimCurveNoDepth,
  BimCurveStrokeDepth,
  BimCurveStrokeNoDepth,
  BimSectionClipCapFill,
  BimSectionClipCapHatch,
  SurfaceNormalLine,
  ObjectNormalDebug,
  ObjectNormalDebugFrontCull,
  ObjectNormalDebugNoCull,
  LightGizmo,
  LightGizmoCoverage,
  LightGizmoPick,
  TransformGizmo,
  TransformGizmoSolid,
  TransformGizmoOverlay,
  TransformGizmoSolidOverlay,
};

[[nodiscard]] inline std::string_view
deferredRasterPipelineName(DeferredRasterPipelineId id) {
  switch (id) {
  case DeferredRasterPipelineId::DepthPrepass:
    return "depth-prepass";
  case DeferredRasterPipelineId::DepthPrepassFrontCull:
    return "depth-prepass-front-cull";
  case DeferredRasterPipelineId::DepthPrepassNoCull:
    return "depth-prepass-no-cull";
  case DeferredRasterPipelineId::BimDepthPrepass:
    return "bim-depth-prepass";
  case DeferredRasterPipelineId::BimDepthPrepassFrontCull:
    return "bim-depth-prepass-front-cull";
  case DeferredRasterPipelineId::BimDepthPrepassNoCull:
    return "bim-depth-prepass-no-cull";
  case DeferredRasterPipelineId::GBuffer:
    return "gbuffer";
  case DeferredRasterPipelineId::GBufferFrontCull:
    return "gbuffer-front-cull";
  case DeferredRasterPipelineId::GBufferNoCull:
    return "gbuffer-no-cull";
  case DeferredRasterPipelineId::BimGBuffer:
    return "bim-gbuffer";
  case DeferredRasterPipelineId::BimGBufferFrontCull:
    return "bim-gbuffer-front-cull";
  case DeferredRasterPipelineId::BimGBufferNoCull:
    return "bim-gbuffer-no-cull";
  case DeferredRasterPipelineId::LocalShadowDepth:
    return "local-shadow-depth";
  case DeferredRasterPipelineId::LocalShadowDepthFrontCull:
    return "local-shadow-depth-front-cull";
  case DeferredRasterPipelineId::LocalShadowDepthNoCull:
    return "local-shadow-depth-no-cull";
  case DeferredRasterPipelineId::DirectionalLight:
    return "lighting";
  case DeferredRasterPipelineId::StencilVolume:
    return "stencil-volume";
  case DeferredRasterPipelineId::PointLight:
    return "point-light";
  case DeferredRasterPipelineId::PointLightStencilDebug:
    return "point-light-stencil-debug";
  case DeferredRasterPipelineId::TiledPointLight:
    return "tiled-point-light";
  case DeferredRasterPipelineId::Transparent:
    return "transparent";
  case DeferredRasterPipelineId::TransparentFrontCull:
    return "transparent-front-cull";
  case DeferredRasterPipelineId::TransparentNoCull:
    return "transparent-no-cull";
  case DeferredRasterPipelineId::TransparentPick:
    return "transparent-pick";
  case DeferredRasterPipelineId::TransparentPickFrontCull:
    return "transparent-pick-front-cull";
  case DeferredRasterPipelineId::TransparentPickNoCull:
    return "transparent-pick-no-cull";
  case DeferredRasterPipelineId::PostProcess:
    return "post-process";
  case DeferredRasterPipelineId::GeometryDebug:
    return "geometry-debug";
  case DeferredRasterPipelineId::NormalValidation:
    return "normal-validation";
  case DeferredRasterPipelineId::NormalValidationFrontCull:
    return "normal-validation-front-cull";
  case DeferredRasterPipelineId::NormalValidationNoCull:
    return "normal-validation-no-cull";
  case DeferredRasterPipelineId::WireframeDepth:
    return "wireframe-depth";
  case DeferredRasterPipelineId::WireframeDepthFrontCull:
    return "wireframe-depth-front-cull";
  case DeferredRasterPipelineId::WireframeNoDepth:
    return "wireframe-no-depth";
  case DeferredRasterPipelineId::WireframeNoDepthFrontCull:
    return "wireframe-no-depth-front-cull";
  case DeferredRasterPipelineId::SelectionMask:
    return "selection-mask";
  case DeferredRasterPipelineId::SelectionOutline:
    return "selection-outline";
  case DeferredRasterPipelineId::BimFloorPlanDepth:
    return "bim-floor-plan-depth";
  case DeferredRasterPipelineId::BimFloorPlanNoDepth:
    return "bim-floor-plan-no-depth";
  case DeferredRasterPipelineId::BimPointCloudDepth:
    return "bim-point-cloud-depth";
  case DeferredRasterPipelineId::BimPointCloudNoDepth:
    return "bim-point-cloud-no-depth";
  case DeferredRasterPipelineId::BimCurveDepth:
    return "bim-curve-depth";
  case DeferredRasterPipelineId::BimCurveNoDepth:
    return "bim-curve-no-depth";
  case DeferredRasterPipelineId::BimCurveStrokeDepth:
    return "bim-curve-stroke-depth";
  case DeferredRasterPipelineId::BimCurveStrokeNoDepth:
    return "bim-curve-stroke-no-depth";
  case DeferredRasterPipelineId::BimSectionClipCapFill:
    return "bim-section-clip-cap-fill";
  case DeferredRasterPipelineId::BimSectionClipCapHatch:
    return "bim-section-clip-cap-hatch";
  case DeferredRasterPipelineId::SurfaceNormalLine:
    return "surface-normal-line";
  case DeferredRasterPipelineId::ObjectNormalDebug:
    return "object-normal-debug";
  case DeferredRasterPipelineId::ObjectNormalDebugFrontCull:
    return "object-normal-debug-front-cull";
  case DeferredRasterPipelineId::ObjectNormalDebugNoCull:
    return "object-normal-debug-no-cull";
  case DeferredRasterPipelineId::LightGizmo:
    return "light-gizmo";
  case DeferredRasterPipelineId::LightGizmoCoverage:
    return "light-gizmo-coverage";
  case DeferredRasterPipelineId::LightGizmoPick:
    return "light-gizmo-pick";
  case DeferredRasterPipelineId::TransformGizmo:
    return "transform-gizmo";
  case DeferredRasterPipelineId::TransformGizmoSolid:
    return "transform-gizmo-solid";
  case DeferredRasterPipelineId::TransformGizmoOverlay:
    return "transform-gizmo-overlay";
  case DeferredRasterPipelineId::TransformGizmoSolidOverlay:
    return "transform-gizmo-solid-overlay";
  }
  return {};
}

enum class DeferredRasterPipelineLayoutId {
  Scene,
  Transparent,
  Lighting,
  LightGizmo,
  TiledLighting,
  Shadow,
  PostProcess,
  Wireframe,
  NormalValidation,
  SurfaceNormal,
  TransformGizmo,
};

[[nodiscard]] inline std::string_view
deferredRasterPipelineLayoutName(DeferredRasterPipelineLayoutId id) {
  switch (id) {
  case DeferredRasterPipelineLayoutId::Scene:
    return "scene";
  case DeferredRasterPipelineLayoutId::Transparent:
    return "transparent";
  case DeferredRasterPipelineLayoutId::Lighting:
    return "lighting";
  case DeferredRasterPipelineLayoutId::LightGizmo:
    return "light-gizmo";
  case DeferredRasterPipelineLayoutId::TiledLighting:
    return "tiled-lighting";
  case DeferredRasterPipelineLayoutId::Shadow:
    return "shadow";
  case DeferredRasterPipelineLayoutId::PostProcess:
    return "post-process";
  case DeferredRasterPipelineLayoutId::Wireframe:
    return "wireframe";
  case DeferredRasterPipelineLayoutId::NormalValidation:
    return "normal-validation";
  case DeferredRasterPipelineLayoutId::SurfaceNormal:
    return "surface-normal";
  case DeferredRasterPipelineLayoutId::TransformGizmo:
    return "transform-gizmo";
  }
  return {};
}

[[nodiscard]] inline VkPipelineLayout
deferredRasterPipelineLayout(const FrameRecordParams &p,
                             DeferredRasterPipelineLayoutId id) {
  return p.pipelineLayout(RenderTechniqueId::DeferredRaster,
                          deferredRasterPipelineLayoutName(id));
}

[[nodiscard]] inline bool
deferredRasterPipelineLayoutReady(const FrameRecordParams &p,
                                  DeferredRasterPipelineLayoutId id) {
  return deferredRasterPipelineLayout(p, id) != VK_NULL_HANDLE;
}

[[nodiscard]] inline VkPipeline
deferredRasterPipelineHandle(const FrameRecordParams &p,
                             DeferredRasterPipelineId id) {
  return p.pipelineHandle(RenderTechniqueId::DeferredRaster,
                          deferredRasterPipelineName(id));
}

[[nodiscard]] inline bool
deferredRasterPipelineReady(const FrameRecordParams &p,
                            DeferredRasterPipelineId id) {
  return deferredRasterPipelineHandle(p, id) != VK_NULL_HANDLE;
}

} // namespace container::renderer
