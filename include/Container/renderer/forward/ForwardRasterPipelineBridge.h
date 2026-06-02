#pragma once

#include "Container/renderer/core/FrameRecorder.h"

#include <string_view>

namespace container::renderer {

enum class ForwardRasterPipelineId {
  DepthPrepass,
  DepthPrepassFrontCull,
  DepthPrepassNoCull,
  BimDepthPrepass,
  BimDepthPrepassFrontCull,
  BimDepthPrepassNoCull,
  ForwardOpaque,
  Transparent,
  TransparentFrontCull,
  TransparentNoCull,
  PostProcess,
  LightGizmo,
  LightGizmoCoverage,
  TransformGizmo,
  TransformGizmoSolid,
  TransformGizmoOverlay,
  TransformGizmoSolidOverlay,
};

[[nodiscard]] inline std::string_view
forwardRasterPipelineName(ForwardRasterPipelineId id) {
  switch (id) {
  case ForwardRasterPipelineId::DepthPrepass:
    return "depth-prepass";
  case ForwardRasterPipelineId::DepthPrepassFrontCull:
    return "depth-prepass-front-cull";
  case ForwardRasterPipelineId::DepthPrepassNoCull:
    return "depth-prepass-no-cull";
  case ForwardRasterPipelineId::BimDepthPrepass:
    return "bim-depth-prepass";
  case ForwardRasterPipelineId::BimDepthPrepassFrontCull:
    return "bim-depth-prepass-front-cull";
  case ForwardRasterPipelineId::BimDepthPrepassNoCull:
    return "bim-depth-prepass-no-cull";
  case ForwardRasterPipelineId::ForwardOpaque:
    return "forward-opaque";
  case ForwardRasterPipelineId::Transparent:
    return "forward-transparent";
  case ForwardRasterPipelineId::TransparentFrontCull:
    return "forward-transparent-front-cull";
  case ForwardRasterPipelineId::TransparentNoCull:
    return "forward-transparent-no-cull";
  case ForwardRasterPipelineId::PostProcess:
    return "post-process";
  case ForwardRasterPipelineId::LightGizmo:
    return "light-gizmo";
  case ForwardRasterPipelineId::LightGizmoCoverage:
    return "light-gizmo-coverage";
  case ForwardRasterPipelineId::TransformGizmo:
    return "transform-gizmo";
  case ForwardRasterPipelineId::TransformGizmoSolid:
    return "transform-gizmo-solid";
  case ForwardRasterPipelineId::TransformGizmoOverlay:
    return "transform-gizmo-overlay";
  case ForwardRasterPipelineId::TransformGizmoSolidOverlay:
    return "transform-gizmo-solid-overlay";
  }
  return {};
}

enum class ForwardRasterPipelineLayoutId {
  Scene,
  Transparent,
  PostProcess,
  LightGizmo,
  TransformGizmo,
};

[[nodiscard]] inline std::string_view
forwardRasterPipelineLayoutName(ForwardRasterPipelineLayoutId id) {
  switch (id) {
  case ForwardRasterPipelineLayoutId::Scene:
    return "scene";
  case ForwardRasterPipelineLayoutId::Transparent:
    return "transparent";
  case ForwardRasterPipelineLayoutId::PostProcess:
    return "post-process";
  case ForwardRasterPipelineLayoutId::LightGizmo:
    return "light-gizmo";
  case ForwardRasterPipelineLayoutId::TransformGizmo:
    return "transform-gizmo";
  }
  return {};
}

[[nodiscard]] inline VkPipeline
forwardRasterPipelineHandle(const FrameRecordParams &p,
                            ForwardRasterPipelineId id) {
  return p.pipelineHandle(RenderTechniqueId::ForwardRaster,
                          forwardRasterPipelineName(id));
}

[[nodiscard]] inline bool
forwardRasterPipelineReady(const FrameRecordParams &p,
                           ForwardRasterPipelineId id) {
  return forwardRasterPipelineHandle(p, id) != VK_NULL_HANDLE;
}

[[nodiscard]] inline VkPipelineLayout
forwardRasterPipelineLayout(const FrameRecordParams &p,
                            ForwardRasterPipelineLayoutId id) {
  return p.pipelineLayout(RenderTechniqueId::ForwardRaster,
                          forwardRasterPipelineLayoutName(id));
}

[[nodiscard]] inline bool
forwardRasterPipelineLayoutReady(const FrameRecordParams &p,
                                 ForwardRasterPipelineLayoutId id) {
  return forwardRasterPipelineLayout(p, id) != VK_NULL_HANDLE;
}

} // namespace container::renderer
