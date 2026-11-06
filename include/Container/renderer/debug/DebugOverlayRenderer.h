#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/debug/DebugOverlayPushConstants.h"
#include "Container/renderer/scene/DrawCommand.h"
#include "Container/utility/GuiManager.h"
#include "Container/utility/SceneData.h"

#include <vector>

namespace container::renderer {

// Stateless draw helpers and debug/overlay recording.
// Caller binds pipelines and descriptor sets before calling helpers where noted.
class DebugOverlayRenderer {
 public:
  DebugOverlayRenderer() = default;

  // --- Geometry draw helpers ---
  void drawScene(VkCommandBuffer cmd,
                 VkPipelineLayout layout,
                 const std::vector<DrawCommand>& commands,
                 container::gpu::BindlessPushConstants& pc) const;

  void drawWireframe(VkCommandBuffer cmd,
                     VkPipelineLayout layout,
                     const std::vector<DrawCommand>& commands,
                     const glm::vec3& color,
                     float intensity,
                     float lineWidth,
                     WireframePushConstants& pc) const;

  // Filled pixel-disk dilation of the selected stencil silhouette. Each draw
  // batches all stroke offsets as instances; its object group remains intact.
  // The caller binds the selection-outline pipeline and original stencil mask.
  void drawSelectionStroke(VkCommandBuffer cmd,
                           VkPipelineLayout layout,
                           const std::vector<DrawCommand>& commands,
                           const glm::vec3& color,
                           WireframePushConstants& pc) const;

  // --- Debug overlay passes (pipeline already bound by caller) ---
  void recordNormalValidation(
      VkCommandBuffer cmd,
      VkPipelineLayout layout,
      const std::vector<DrawCommand>& opaque,
      const std::vector<DrawCommand>& transparent,
      uint32_t faceClassificationFlags,
      const container::gpu::NormalValidationSettings& settings,
      NormalValidationPushConstants& pc) const;

  void recordSurfaceNormals(
      VkCommandBuffer cmd,
      VkPipelineLayout layout,
      const std::vector<DrawCommand>& opaque,
      const std::vector<DrawCommand>& transparent,
      const container::gpu::NormalValidationSettings& settings,
      SurfaceNormalPushConstants& pc) const;
};

}  // namespace container::renderer
