#include "Container/renderer/bim/BimSectionClipCapPassRecorder.h"

#include "Container/renderer/bim/BimSectionCapBuilder.h"

#include <array>

namespace container::renderer {

namespace {

[[nodiscard]] bool hasDrawCommands(const std::vector<DrawCommand> *commands) {
  return commands != nullptr && !commands->empty();
}

VkPipeline
pipelineForBimSectionClipCapPass(BimSectionClipCapPassPipeline pipeline,
                                 VkPipeline fillPipeline,
                                 VkPipeline hatchPipeline) {
  switch (pipeline) {
  case BimSectionClipCapPassPipeline::Fill:
    return fillPipeline;
  case BimSectionClipCapPassPipeline::Hatch:
    return hatchPipeline;
  }
  return VK_NULL_HANDLE;
}

void bindGeometry(VkCommandBuffer cmd, container::gpu::BufferSlice vertex,
                  container::gpu::BufferSlice index, VkIndexType indexType) {
  const VkDeviceSize offsets[] = {vertex.offset};
  vkCmdBindVertexBuffers(cmd, 0, 1, &vertex.buffer, offsets);
  vkCmdBindIndexBuffer(cmd, index.buffer, index.offset, indexType);
}

[[nodiscard]] bool
hasRequiredInputs(const BimSectionClipCapPassRecordInputs &inputs) {
  return inputs.plan != nullptr && inputs.plan->active &&
         inputs.wireframeLayout != VK_NULL_HANDLE &&
         inputs.sceneDescriptorSet != VK_NULL_HANDLE &&
         inputs.vertexSlice.buffer != VK_NULL_HANDLE &&
         inputs.indexSlice.buffer != VK_NULL_HANDLE &&
         inputs.pushConstants != nullptr && inputs.debugOverlay != nullptr;
}

[[nodiscard]] bool hasPerCommandStyles(
    const BimSectionClipCapPassRoute &route) {
  return route.commands != nullptr && route.drawStyles != nullptr &&
         route.drawStyles->size() == route.commands->size();
}

[[nodiscard]] glm::vec3 routeStyleColor(
    const BimSectionClipCapPassRoute &route,
    const BimSectionCapDrawStyle &style) {
  switch (route.pipeline) {
  case BimSectionClipCapPassPipeline::Fill:
    return style.fillColor;
  case BimSectionClipCapPassPipeline::Hatch:
    return style.hatchColor;
  }
  return route.color;
}

[[nodiscard]] float routeStyleOpacity(
    const BimSectionClipCapPassRoute &route,
    const BimSectionCapDrawStyle &style) {
  switch (route.pipeline) {
  case BimSectionClipCapPassPipeline::Fill:
    return style.fillOpacity;
  case BimSectionClipCapPassPipeline::Hatch:
    return route.opacity;
  }
  return route.opacity;
}

[[nodiscard]] float routeStyleLineWidth(
    const BimSectionClipCapPassRoute &route,
    const BimSectionCapDrawStyle &style) {
  return style.lineWidth > 0.0f ? style.lineWidth : route.drawLineWidth;
}

[[nodiscard]] bool drawStyledRouteCommands(
    VkCommandBuffer cmd, VkPipelineLayout wireframeLayout,
    const DebugOverlayRenderer &debugOverlay,
    const BimSectionClipCapPassRoute &route, WireframePushConstants &pc) {
  if (!hasPerCommandStyles(route)) {
    if (route.markerCommandsOnly) {
      return false;
    }
    debugOverlay.drawWireframe(cmd, wireframeLayout, *route.commands,
                               route.color, route.opacity,
                               route.drawLineWidth, pc);
    return true;
  }

  bool recorded = false;
  std::vector<DrawCommand> commandBatch;
  commandBatch.reserve(1u);
  for (size_t commandIndex = 0u; commandIndex < route.commands->size();
       ++commandIndex) {
    const BimSectionCapDrawStyle &style = (*route.drawStyles)[commandIndex];
    if (route.markerCommandsOnly && style.lineWidth <= 0.0f) {
      continue;
    }
    commandBatch.clear();
    commandBatch.push_back((*route.commands)[commandIndex]);
    debugOverlay.drawWireframe(cmd, wireframeLayout, commandBatch,
                               routeStyleColor(route, style),
                               routeStyleOpacity(route, style),
                               routeStyleLineWidth(route, style), pc);
    recorded = true;
  }
  return recorded;
}

} // namespace

bool hasBimSectionClipCapFramePassGeometry(
    const BimSectionClipCapPassGeometryBinding &geometry) {
  return geometry.sceneDescriptorSet != VK_NULL_HANDLE &&
         geometry.vertexSlice.buffer != VK_NULL_HANDLE &&
         geometry.indexSlice.buffer != VK_NULL_HANDLE;
}

BimSectionClipCapPassInputs buildBimSectionClipCapFramePassPlanInputs(
    const BimSectionClipCapFramePassRecordInputs &inputs) {
  return {
      .enabled = inputs.style.enabled,
      .fillEnabled = inputs.style.fillEnabled,
      .hatchEnabled = inputs.style.hatchEnabled,
      .geometryReady = hasBimSectionClipCapFramePassGeometry(inputs.geometry),
      .wireframeLayoutReady = inputs.wireframeLayout != VK_NULL_HANDLE,
      .wireframePushConstantsReady = inputs.pushConstants != nullptr,
      .wideLinesSupported = inputs.style.wideLinesSupported,
      .fillPipelineReady = inputs.fillPipeline != VK_NULL_HANDLE,
      .hatchPipelineReady = inputs.hatchPipeline != VK_NULL_HANDLE,
      .fillColor = inputs.style.fillColor,
      .hatchColor = inputs.style.hatchColor,
      .hatchLineWidth = inputs.style.hatchLineWidth,
      .fillDrawCommands = inputs.style.fillDrawCommands,
      .hatchDrawCommands = inputs.style.hatchDrawCommands,
      .fillDrawStyles = inputs.style.fillDrawStyles,
      .hatchDrawStyles = inputs.style.hatchDrawStyles,
      .sectionMarkerLines = inputs.style.sectionMarkerLines,
  };
}

bool recordBimSectionClipCapPassCommands(
    VkCommandBuffer cmd, const BimSectionClipCapPassRecordInputs &inputs) {
  if (!hasRequiredInputs(inputs)) {
    return false;
  }

  const std::array sets{inputs.sceneDescriptorSet, inputs.oitDescriptorSet};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          inputs.wireframeLayout, 0,
                          inputs.oitDescriptorSet == VK_NULL_HANDLE ? 1u : 2u,
                          sets.data(), 0, nullptr);
  bindGeometry(cmd, inputs.vertexSlice, inputs.indexSlice, inputs.indexType);

  WireframePushConstants pc = *inputs.pushConstants;
  pc.sectionPlaneEnabled = 0u;
  bool recorded = false;
  for (uint32_t routeIndex = 0u; routeIndex < inputs.plan->routeCount;
       ++routeIndex) {
    const BimSectionClipCapPassRoute &route = inputs.plan->routes[routeIndex];
    const VkPipeline pipeline = pipelineForBimSectionClipCapPass(
        route.pipeline, inputs.fillPipeline, inputs.hatchPipeline);
    if (!hasDrawCommands(route.commands) || pipeline == VK_NULL_HANDLE) {
      continue;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    if (route.rasterLineWidthApplies) {
      vkCmdSetLineWidth(cmd, route.rasterLineWidth);
    }
    const bool routeRecorded = drawStyledRouteCommands(
        cmd, inputs.wireframeLayout, *inputs.debugOverlay, route, pc);
    if (route.resetRasterLineWidth) {
      vkCmdSetLineWidth(cmd, 1.0f);
    }
    recorded = recorded || routeRecorded;
  }
  return recorded;
}

bool recordBimSectionClipCapFramePassCommands(
    VkCommandBuffer cmd, const BimSectionClipCapFramePassRecordInputs &inputs) {
  if (cmd == VK_NULL_HANDLE) {
    return false;
  }

  const BimSectionClipCapPassPlan plan = buildBimSectionClipCapPassPlan(
      buildBimSectionClipCapFramePassPlanInputs(inputs));
  if (!plan.active) {
    return false;
  }

  return recordBimSectionClipCapPassCommands(
      cmd, {.plan = &plan,
            .fillPipeline = inputs.fillPipeline,
            .hatchPipeline = inputs.hatchPipeline,
            .wireframeLayout = inputs.wireframeLayout,
            .sceneDescriptorSet = inputs.geometry.sceneDescriptorSet,
            .vertexSlice = inputs.geometry.vertexSlice,
            .indexSlice = inputs.geometry.indexSlice,
            .indexType = inputs.geometry.indexType,
            .pushConstants = inputs.pushConstants,
            .debugOverlay = inputs.debugOverlay,
            .oitDescriptorSet = inputs.oitDescriptorSet});
}

} // namespace container::renderer
