#include "Container/renderer/bim/BimSurfaceRasterPassRecorder.h"

#include "Container/renderer/scene/SceneViewport.h"

namespace container::renderer {

namespace {

[[nodiscard]] bool
hasRasterPassShell(VkCommandBuffer cmd,
                   const BimSurfaceRasterPassRecordInputs &inputs) {
  return cmd != VK_NULL_HANDLE && inputs.renderPass != VK_NULL_HANDLE &&
         inputs.framebuffer != VK_NULL_HANDLE && inputs.extent.width > 0u &&
         inputs.extent.height > 0u && inputs.plan != nullptr &&
         inputs.plan->active;
}

[[nodiscard]] bool
hasReadyDescriptorSets(std::span<const VkDescriptorSet> descriptorSets) {
  if (descriptorSets.empty()) {
    return false;
  }
  for (VkDescriptorSet descriptorSet : descriptorSets) {
    if (descriptorSet == VK_NULL_HANDLE) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool
hasBimSurfaceFrameGeometry(const BimSurfacePassGeometryBinding &geometry) {
  return geometry.vertexSlice.buffer != VK_NULL_HANDLE &&
         geometry.indexSlice.buffer != VK_NULL_HANDLE;
}

[[nodiscard]] bool isTransparentBimSurfaceFramePass(BimSurfacePassKind kind) {
  return kind == BimSurfacePassKind::TransparentPick ||
         kind == BimSurfacePassKind::TransparentLighting;
}

} // namespace

container::gpu::BindlessPushConstants
bimSurfaceRasterPassPushConstants(container::gpu::BindlessPushConstants base,
                                  const BimSurfacePassPlan &plan) {
  if (plan.writesSemanticColorMode) {
    base.semanticColorMode = plan.semanticColorMode;
  }
  return base;
}

BimSurfacePassInputs
buildBimSurfaceFramePassInputs(const BimSurfaceFramePassRecordInputs &inputs) {
  const bool transparentPass = isTransparentBimSurfaceFramePass(inputs.kind);
  const bool meshGpuVisibilityOwnsCpuFallback =
      transparentPass ? inputs.draws.transparentMeshDrawsUseGpuVisibility
                      : inputs.draws.opaqueMeshDrawsUseGpuVisibility;

  BimSurfacePassInputs planInputs{};
  planInputs.kind = inputs.kind;
  planInputs.passReady = inputs.passReady;
  planInputs.geometryReady = hasBimSurfaceFrameGeometry(inputs.geometry);
  planInputs.descriptorSetReady =
      hasReadyDescriptorSets(inputs.geometry.descriptorSets);
  planInputs.bindlessPushConstantsReady = inputs.pushConstants != nullptr;
  planInputs.basePipelineReady = inputs.pipelines.singleSided != VK_NULL_HANDLE;
  planInputs.semanticColorMode = inputs.semanticColorMode;
  planInputs.sources[planInputs.sourceCount++] = {
      .source = BimSurfacePassSourceKind::Mesh,
      .draws = inputs.draws.mesh,
      .gpuCompactionEligible = meshGpuVisibilityOwnsCpuFallback,
      .gpuVisibilityOwnsCpuFallback = meshGpuVisibilityOwnsCpuFallback};
  planInputs.sources[planInputs.sourceCount++] = {
      .source = BimSurfacePassSourceKind::PointPlaceholders,
      .draws = inputs.draws.pointPlaceholders};
  planInputs.sources[planInputs.sourceCount++] = {
      .source = BimSurfacePassSourceKind::CurvePlaceholders,
      .draws = inputs.draws.curvePlaceholders};
  return planInputs;
}

BimSurfacePassPlan
buildBimSurfaceFramePassPlan(const BimSurfaceFramePassRecordInputs &inputs) {
  return buildBimSurfacePassPlan(buildBimSurfaceFramePassInputs(inputs));
}

BimSurfaceFrameBinding buildBimSurfaceFrameBinding(
    const BimSurfaceFrameBindingInputs &inputs) {
  return {.draws = inputs.draws,
          .geometry = {.descriptorSets = inputs.descriptorSets,
                       .vertexSlice = inputs.vertexSlice,
                       .indexSlice = inputs.indexSlice,
                       .indexType = inputs.indexType},
          .semanticColorMode = inputs.semanticColorMode};
}

bool recordBimSurfaceRasterPassCommands(
    VkCommandBuffer cmd, const BimSurfaceRasterPassRecordInputs &inputs) {
  if (!hasRasterPassShell(cmd, inputs)) {
    return false;
  }

  RenderingBeginInfo info{};
  info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  info.renderPass = inputs.renderPass;
  info.framebuffer = inputs.framebuffer;
  info.renderArea.offset = {0, 0};
  info.renderArea.extent = inputs.extent;

  beginDynamicRendering(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
  recordSceneViewportAndScissor(cmd, inputs.extent);
  static_cast<void>(recordBimSurfacePassCommands(
      cmd, {.plan = inputs.plan,
            .geometry = inputs.geometry,
            .singleSidedPipeline = inputs.pipelines.singleSided,
            .windingFlippedPipeline = inputs.pipelines.windingFlipped,
            .doubleSidedPipeline = inputs.pipelines.doubleSided,
            .pipelineLayout = inputs.pipelineLayout,
            .pushConstants = bimSurfaceRasterPassPushConstants(
                inputs.pushConstants, *inputs.plan),
            .debugOverlay = inputs.debugOverlay,
            .bimManager = inputs.bimManager}));
  endDynamicRendering(cmd);
  return true;
}

bool recordBimSurfaceFramePassCommands(
    VkCommandBuffer cmd, const BimSurfaceFramePassRecordInputs &inputs) {
  if (cmd == VK_NULL_HANDLE || inputs.pushConstants == nullptr) {
    return false;
  }

  const BimSurfacePassPlan plan =
      buildBimSurfaceFramePassPlan(inputs);
  if (!plan.active) {
    return false;
  }

  return recordBimSurfaceRasterPassCommands(
      cmd, {.renderPass = inputs.renderPass,
            .framebuffer = inputs.framebuffer,
            .extent = inputs.extent,
            .plan = &plan,
            .geometry = inputs.geometry,
            .pipelines = inputs.pipelines,
            .pipelineLayout = inputs.pipelineLayout,
            .pushConstants = *inputs.pushConstants,
            .debugOverlay = inputs.debugOverlay,
            .bimManager = inputs.bimManager});
}

} // namespace container::renderer
