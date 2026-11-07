#include "Container/renderer/debug/DebugOverlayRenderer.h"
#include "Container/renderer/debug/SelectionStrokeKernel.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace container::renderer {

using container::gpu::BindlessPushConstants;

namespace {

constexpr uint32_t kIndirectObjectIndex = std::numeric_limits<uint32_t>::max();

uint32_t drawInstanceCount(const DrawCommand& command) {
  return std::max(command.instanceCount, 1u);
}

}  // namespace

void DebugOverlayRenderer::drawScene(VkCommandBuffer cmd,
                                      VkPipelineLayout layout,
                                      const std::vector<DrawCommand>& commands,
                                      BindlessPushConstants& pc) const {
  if (commands.empty()) {
    return;
  }
  pc.objectIndex = kIndirectObjectIndex;
  vkCmdPushConstants(cmd, layout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, sizeof(BindlessPushConstants), &pc);
  for (const DrawCommand& dc : commands) {
    vkCmdDrawIndexed(cmd, dc.indexCount, drawInstanceCount(dc), dc.firstIndex,
                     0, dc.objectIndex);
  }
}

void DebugOverlayRenderer::drawWireframe(VkCommandBuffer cmd,
                                          VkPipelineLayout layout,
                                          const std::vector<DrawCommand>& commands,
                                          const glm::vec3& color,
                                          float intensity,
                                          float lineWidth,
                                          WireframePushConstants& pc) const {
  if (commands.empty()) {
    return;
  }
  pc.objectIndex = kIndirectObjectIndex;
  pc.colorIntensity = glm::vec4(color, std::clamp(intensity, 0.0f, 1.0f));
  pc.lineWidth      = std::max(lineWidth, 1.0f);
  vkCmdPushConstants(cmd, layout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, sizeof(WireframePushConstants), &pc);
  for (const DrawCommand& dc : commands) {
    vkCmdDrawIndexed(cmd, dc.indexCount, drawInstanceCount(dc), dc.firstIndex,
                     0, dc.objectIndex);
  }
}

void DebugOverlayRenderer::drawSelectionStroke(
    VkCommandBuffer cmd, VkPipelineLayout layout,
    const std::vector<DrawCommand>& commands, const glm::vec3& color,
    WireframePushConstants& pc) const {
  // Validate every object group before issuing any commands. firstInstance is
  // zero: only this shader divides the expanded instance ordinal into an
  // object ordinal and a stroke sample, without changing other draw helpers.
  for (const auto& dc : commands)
    (void)PlanSelectionStrokeInstanceBatch(dc.objectIndex, dc.instanceCount);
  pc.colorIntensity = glm::vec4(color, 1.0f);
  pc.lineWidth = float(kSelectionStrokeRadius);
  for (const auto& dc : commands) {
    uint32_t remaining = drawInstanceCount(dc);
    uint64_t base = dc.objectIndex;
    while (remaining) {
      const auto batch = PlanSelectionStrokeInstanceBatch(
          static_cast<uint32_t>(base), remaining);
      pc.objectIndex = batch.objectIndex;
      vkCmdPushConstants(cmd, layout,
          VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
          0, sizeof(WireframePushConstants), &pc);
      vkCmdDrawIndexed(cmd, dc.indexCount, batch.instanceCount,
          dc.firstIndex, 0, 0);
      base += batch.objectCount;
      remaining -= batch.objectCount;
    }
  }
}

void DebugOverlayRenderer::recordNormalValidation(
    VkCommandBuffer cmd, VkPipelineLayout layout,
    const std::vector<DrawCommand>& opaque,
    const std::vector<DrawCommand>& transparent,
    uint32_t faceClassificationFlags,
    const container::gpu::NormalValidationSettings& settings,
    NormalValidationPushConstants& pc) const {
  pc.showFaceFill = settings.showFaceFill ? 1u : 0u;
  pc.faceAlpha    = settings.faceAlpha;
  pc.faceClassificationFlags = faceClassificationFlags;

  constexpr VkShaderStageFlags kStages =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT |
      VK_SHADER_STAGE_FRAGMENT_BIT;

  if (opaque.empty() && transparent.empty()) {
    return;
  }
  pc.objectIndex = kIndirectObjectIndex;
  vkCmdPushConstants(cmd, layout, kStages, 0,
                     sizeof(NormalValidationPushConstants), &pc);

  auto draw = [&](const std::vector<DrawCommand>& cmds) {
    for (const DrawCommand& dc : cmds) {
      vkCmdDrawIndexed(cmd, dc.indexCount, drawInstanceCount(dc),
                       dc.firstIndex, 0, dc.objectIndex);
    }
  };
  draw(opaque);
  draw(transparent);
}

void DebugOverlayRenderer::recordSurfaceNormals(
    VkCommandBuffer cmd, VkPipelineLayout layout,
    const std::vector<DrawCommand>& opaque,
    const std::vector<DrawCommand>& transparent,
    const container::gpu::NormalValidationSettings& settings,
    SurfaceNormalPushConstants& pc) const {
  pc.lineLength = settings.lineLength;
  pc.lineOffset = settings.lineOffset;

  constexpr VkShaderStageFlags kStages =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT |
      VK_SHADER_STAGE_FRAGMENT_BIT;

  if (opaque.empty() && transparent.empty()) {
    return;
  }
  pc.objectIndex = kIndirectObjectIndex;
  vkCmdPushConstants(cmd, layout, kStages, 0,
                     sizeof(SurfaceNormalPushConstants), &pc);

  auto draw = [&](const std::vector<DrawCommand>& cmds) {
    for (const DrawCommand& dc : cmds) {
      vkCmdDrawIndexed(cmd, dc.indexCount, drawInstanceCount(dc),
                       dc.firstIndex, 0, dc.objectIndex);
    }
  };
  draw(opaque);
  draw(transparent);
}

}  // namespace container::renderer
