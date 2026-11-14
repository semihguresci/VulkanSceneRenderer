#include "Container/renderer/deferred/DeferredRasterDebugOverlayRecorder.h"

#include "Container/renderer/scene/DrawCommand.h"

#include <limits>
#include <vector>

namespace container::renderer {

namespace {

[[nodiscard]] bool hasDrawCommands(const std::vector<DrawCommand> *commands) {
  return commands != nullptr && !commands->empty();
}

[[nodiscard]] bool hasPairCommands(const DeferredDebugOverlayRoute &route) {
  return hasDrawCommands(route.opaqueCommands) ||
         hasDrawCommands(route.transparentCommands);
}

[[nodiscard]] bool hasRequiredInputs(
    const DeferredDebugOverlayRecordInputs &inputs) {
  return inputs.plan != nullptr && inputs.debugOverlay != nullptr;
}

[[nodiscard]] VkPipeline choosePipeline(VkPipeline preferred,
                                        VkPipeline fallback) {
  return preferred != VK_NULL_HANDLE ? preferred : fallback;
}

[[nodiscard]] VkPipeline pipelineForDeferredDebugOverlay(
    DeferredDebugOverlayPipeline pipeline,
    const DeferredDebugOverlayPipelineHandles &pipelines) {
  switch (pipeline) {
  case DeferredDebugOverlayPipeline::WireframeDepth:
    return pipelines.wireframeDepth;
  case DeferredDebugOverlayPipeline::WireframeNoDepth:
    return pipelines.wireframeNoDepth;
  case DeferredDebugOverlayPipeline::WireframeDepthFrontCull:
    return choosePipeline(pipelines.wireframeDepthFrontCull,
                          pipelines.wireframeDepth);
  case DeferredDebugOverlayPipeline::WireframeNoDepthFrontCull:
    return choosePipeline(pipelines.wireframeNoDepthFrontCull,
                          pipelines.wireframeNoDepth);
  case DeferredDebugOverlayPipeline::ObjectNormalDebug:
    return pipelines.objectNormalDebug;
  case DeferredDebugOverlayPipeline::ObjectNormalDebugFrontCull:
    return choosePipeline(pipelines.objectNormalDebugFrontCull,
                          pipelines.objectNormalDebug);
  case DeferredDebugOverlayPipeline::ObjectNormalDebugNoCull:
    return choosePipeline(pipelines.objectNormalDebugNoCull,
                          pipelines.objectNormalDebug);
  case DeferredDebugOverlayPipeline::GeometryDebug:
    return pipelines.geometryDebug;
  case DeferredDebugOverlayPipeline::NormalValidation:
    return pipelines.normalValidation;
  case DeferredDebugOverlayPipeline::NormalValidationFrontCull:
    return choosePipeline(pipelines.normalValidationFrontCull,
                          pipelines.normalValidation);
  case DeferredDebugOverlayPipeline::NormalValidationNoCull:
    return choosePipeline(pipelines.normalValidationNoCull,
                          pipelines.normalValidation);
  case DeferredDebugOverlayPipeline::SurfaceNormalLine:
    return pipelines.surfaceNormalLine;
  }
  return VK_NULL_HANDLE;
}

[[nodiscard]] const DeferredDebugOverlayGeometryBinding &sourceGeometry(
    const DeferredDebugOverlayRecordInputs &inputs,
    DeferredDebugOverlaySource source) {
  return source == DeferredDebugOverlaySource::Scene ? inputs.scene
                                                     : inputs.bim;
}

[[nodiscard]] bool bindGeometryIfReady(
    VkCommandBuffer cmd, const DeferredDebugOverlayGeometryBinding &geometry,
    VkPipelineLayout layout) {
  if (layout == VK_NULL_HANDLE || geometry.descriptorSet == VK_NULL_HANDLE ||
      geometry.vertexSlice.buffer == VK_NULL_HANDLE ||
      geometry.indexSlice.buffer == VK_NULL_HANDLE) {
    return false;
  }

  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1,
                          &geometry.descriptorSet, 0, nullptr);
  const VkDeviceSize offsets[] = {geometry.vertexSlice.offset};
  vkCmdBindVertexBuffers(cmd, 0, 1, &geometry.vertexSlice.buffer, offsets);
  vkCmdBindIndexBuffer(cmd, geometry.indexSlice.buffer,
                       geometry.indexSlice.offset, geometry.indexType);
  return true;
}

void bindWireframePipeline(VkCommandBuffer cmd, VkPipeline pipeline,
                           float lineWidth,
                           const DeferredDebugOverlayRecordInputs &inputs) {
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  if (inputs.wireframeRasterModeSupported) {
    vkCmdSetLineWidth(cmd,
                      inputs.wireframeWideLinesSupported ? lineWidth : 1.0f);
  }
}

[[nodiscard]] bool drawDiagnosticCube(
    VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t objectIndex,
    const DeferredDebugOverlayDiagnosticGeometry &diagnostic,
    container::gpu::BindlessPushConstants pushConstants) {
  if (layout == VK_NULL_HANDLE ||
      objectIndex == std::numeric_limits<uint32_t>::max() ||
      diagnostic.vertexSlice.buffer == VK_NULL_HANDLE ||
      diagnostic.indexSlice.buffer == VK_NULL_HANDLE ||
      diagnostic.indexCount == 0u) {
    return false;
  }

  const VkDeviceSize offsets[] = {diagnostic.vertexSlice.offset};
  vkCmdBindVertexBuffers(cmd, 0, 1, &diagnostic.vertexSlice.buffer, offsets);
  vkCmdBindIndexBuffer(cmd, diagnostic.indexSlice.buffer,
                       diagnostic.indexSlice.offset, VK_INDEX_TYPE_UINT32);
  pushConstants.objectIndex = objectIndex;
  vkCmdPushConstants(cmd, layout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, sizeof(container::gpu::BindlessPushConstants),
                     &pushConstants);
  vkCmdDrawIndexed(cmd, diagnostic.indexCount, 1, 0, 0, objectIndex);
  return true;
}

[[nodiscard]] bool recordWireframeSource(
    VkCommandBuffer cmd, const DeferredDebugOverlaySourcePlan &sourcePlan,
    const DeferredDebugOverlayRecordInputs &inputs) {
  if (inputs.wireframePushConstants == nullptr ||
      !bindGeometryIfReady(cmd, sourceGeometry(inputs, sourcePlan.source),
                           inputs.wireframeLayout)) {
    return false;
  }

  bool recorded = false;
  WireframePushConstants wireframePushConstants =
      *inputs.wireframePushConstants;
  for (uint32_t routeIndex = 0u; routeIndex < sourcePlan.routeCount;
       ++routeIndex) {
    const DeferredDebugOverlayRoute &route = sourcePlan.routes[routeIndex];
    const VkPipeline pipeline =
        pipelineForDeferredDebugOverlay(route.pipeline, inputs.pipelines);
    if (!hasDrawCommands(route.commands) || pipeline == VK_NULL_HANDLE) {
      continue;
    }
    bindWireframePipeline(cmd, pipeline, route.rasterLineWidth, inputs);
    inputs.debugOverlay->drawWireframe(
        cmd, inputs.wireframeLayout, *route.commands, inputs.wireframeColor,
        inputs.wireframeIntensity, route.drawLineWidth,
        wireframePushConstants);
    recorded = true;
  }

  if (sourcePlan.drawDiagnosticCube &&
      inputs.bindlessPushConstants != nullptr) {
    const VkPipeline pipeline = pipelineForDeferredDebugOverlay(
        sourcePlan.diagnosticCubePipeline, inputs.pipelines);
    if (pipeline != VK_NULL_HANDLE) {
      bindWireframePipeline(cmd, pipeline, inputs.wireframeLineWidth, inputs);
      recorded |= drawDiagnosticCube(
          cmd, inputs.wireframeLayout, sourcePlan.diagnosticCubeObjectIndex,
          inputs.diagnostic, *inputs.bindlessPushConstants);
    }
  }
  return recorded;
}

[[nodiscard]] bool recordSceneDebugSource(
    VkCommandBuffer cmd, const DeferredDebugOverlaySourcePlan &sourcePlan,
    VkPipelineLayout layout, bool includeDiagnosticCube,
    const DeferredDebugOverlayRecordInputs &inputs) {
  if (inputs.bindlessPushConstants == nullptr ||
      !bindGeometryIfReady(cmd, sourceGeometry(inputs, sourcePlan.source),
                           layout)) {
    return false;
  }

  bool recorded = false;
  container::gpu::BindlessPushConstants bindlessPushConstants =
      *inputs.bindlessPushConstants;
  for (uint32_t routeIndex = 0u; routeIndex < sourcePlan.routeCount;
       ++routeIndex) {
    const DeferredDebugOverlayRoute &route = sourcePlan.routes[routeIndex];
    const VkPipeline pipeline =
        pipelineForDeferredDebugOverlay(route.pipeline, inputs.pipelines);
    if (!hasDrawCommands(route.commands) || pipeline == VK_NULL_HANDLE) {
      continue;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    inputs.debugOverlay->drawScene(cmd, layout, *route.commands,
                                   bindlessPushConstants);
    recorded = true;
  }

  if (includeDiagnosticCube && sourcePlan.drawDiagnosticCube) {
    const VkPipeline pipeline = pipelineForDeferredDebugOverlay(
        sourcePlan.diagnosticCubePipeline, inputs.pipelines);
    if (pipeline != VK_NULL_HANDLE) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      recorded |= drawDiagnosticCube(
          cmd, layout, sourcePlan.diagnosticCubeObjectIndex,
          inputs.diagnostic, *inputs.bindlessPushConstants);
    }
  }
  return recorded;
}

[[nodiscard]] bool recordNormalValidationSource(
    VkCommandBuffer cmd, const DeferredDebugOverlaySourcePlan &sourcePlan,
    const DeferredDebugOverlayRecordInputs &inputs) {
  if (inputs.normalValidationPushConstants == nullptr ||
      !bindGeometryIfReady(cmd, sourceGeometry(inputs, sourcePlan.source),
                           inputs.normalValidationLayout)) {
    return false;
  }

  static const std::vector<DrawCommand> emptyDrawCommands;
  bool recorded = false;
  NormalValidationPushConstants pushConstants =
      *inputs.normalValidationPushConstants;
  for (uint32_t routeIndex = 0u; routeIndex < sourcePlan.routeCount;
       ++routeIndex) {
    const DeferredDebugOverlayRoute &route = sourcePlan.routes[routeIndex];
    const VkPipeline pipeline =
        pipelineForDeferredDebugOverlay(route.pipeline, inputs.pipelines);
    if (!hasPairCommands(route) || pipeline == VK_NULL_HANDLE) {
      continue;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    inputs.debugOverlay->recordNormalValidation(
        cmd, inputs.normalValidationLayout,
        route.opaqueCommands != nullptr ? *route.opaqueCommands
                                        : emptyDrawCommands,
        route.transparentCommands != nullptr ? *route.transparentCommands
                                             : emptyDrawCommands,
        route.normalValidationFaceFlags, inputs.normalValidationSettings,
        pushConstants);
    recorded = true;
  }
  return recorded;
}

[[nodiscard]] bool recordSurfaceNormalSource(
    VkCommandBuffer cmd, const DeferredDebugOverlaySourcePlan &sourcePlan,
    const DeferredDebugOverlayRecordInputs &inputs) {
  if (inputs.surfaceNormalPushConstants == nullptr ||
      !bindGeometryIfReady(cmd, sourceGeometry(inputs, sourcePlan.source),
                           inputs.surfaceNormalLayout)) {
    return false;
  }

  static const std::vector<DrawCommand> emptyDrawCommands;
  bool recorded = false;
  SurfaceNormalPushConstants pushConstants =
      *inputs.surfaceNormalPushConstants;
  for (uint32_t routeIndex = 0u; routeIndex < sourcePlan.routeCount;
       ++routeIndex) {
    const DeferredDebugOverlayRoute &route = sourcePlan.routes[routeIndex];
    const VkPipeline pipeline =
        pipelineForDeferredDebugOverlay(route.pipeline, inputs.pipelines);
    if (!hasPairCommands(route) || pipeline == VK_NULL_HANDLE) {
      continue;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdSetLineWidth(cmd, route.rasterLineWidth);
    inputs.debugOverlay->recordSurfaceNormals(
        cmd, inputs.surfaceNormalLayout,
        route.opaqueCommands != nullptr ? *route.opaqueCommands
                                        : emptyDrawCommands,
        route.transparentCommands != nullptr ? *route.transparentCommands
                                             : emptyDrawCommands,
        inputs.normalValidationSettings, pushConstants);
    recorded = true;
  }
  return recorded;
}

} // namespace

bool recordDeferredDebugOverlayWireframeFullCommands(
    VkCommandBuffer cmd, const DeferredDebugOverlayRecordInputs &inputs) {
  if (!hasRequiredInputs(inputs)) {
    return false;
  }

  bool recorded = false;
  for (uint32_t sourceIndex = 0u;
       sourceIndex < inputs.plan->wireframeFullSourceCount; ++sourceIndex) {
    recorded |= recordWireframeSource(
        cmd, inputs.plan->wireframeFullSources[sourceIndex], inputs);
  }
  return recorded;
}

bool recordDeferredDebugOverlayObjectNormalCommands(
    VkCommandBuffer cmd, const DeferredDebugOverlayRecordInputs &inputs) {
  if (!hasRequiredInputs(inputs)) {
    return false;
  }

  bool recorded = false;
  for (uint32_t sourceIndex = 0u;
       sourceIndex < inputs.plan->objectNormalSourceCount; ++sourceIndex) {
    recorded |= recordSceneDebugSource(
        cmd, inputs.plan->objectNormalSources[sourceIndex], inputs.sceneLayout,
        true, inputs);
  }
  return recorded;
}

bool recordDeferredDebugOverlayGeometryCommands(
    VkCommandBuffer cmd, const DeferredDebugOverlayRecordInputs &inputs) {
  if (!hasRequiredInputs(inputs)) {
    return false;
  }

  bool recorded = false;
  for (uint32_t sourceIndex = 0u;
       sourceIndex < inputs.plan->geometryOverlaySourceCount; ++sourceIndex) {
    recorded |= recordSceneDebugSource(
        cmd, inputs.plan->geometryOverlaySources[sourceIndex],
        inputs.sceneLayout, false, inputs);
  }
  return recorded;
}

bool recordDeferredDebugOverlayNormalValidationCommands(
    VkCommandBuffer cmd, const DeferredDebugOverlayRecordInputs &inputs) {
  if (!hasRequiredInputs(inputs)) {
    return false;
  }

  bool recorded = false;
  for (uint32_t sourceIndex = 0u;
       sourceIndex < inputs.plan->normalValidationSourceCount; ++sourceIndex) {
    recorded |= recordNormalValidationSource(
        cmd, inputs.plan->normalValidationSources[sourceIndex], inputs);
  }
  return recorded;
}

bool recordDeferredDebugOverlaySurfaceNormalCommands(
    VkCommandBuffer cmd, const DeferredDebugOverlayRecordInputs &inputs) {
  if (!hasRequiredInputs(inputs)) {
    return false;
  }

  bool recorded = false;
  for (uint32_t sourceIndex = 0u;
       sourceIndex < inputs.plan->surfaceNormalSourceCount; ++sourceIndex) {
    recorded |= recordSurfaceNormalSource(
        cmd, inputs.plan->surfaceNormalSources[sourceIndex], inputs);
  }
  return recorded;
}

bool recordDeferredDebugOverlayWireframeOverlayCommands(
    VkCommandBuffer cmd, const DeferredDebugOverlayRecordInputs &inputs) {
  if (!hasRequiredInputs(inputs)) {
    return false;
  }

  bool recorded = false;
  for (uint32_t sourceIndex = 0u;
       sourceIndex < inputs.plan->wireframeOverlaySourceCount; ++sourceIndex) {
    recorded |= recordWireframeSource(
        cmd, inputs.plan->wireframeOverlaySources[sourceIndex], inputs);
  }
  return recorded;
}

} // namespace container::renderer
