#pragma once

#include "Container/common/CommonVulkan.h"

#include <algorithm>

namespace container::renderer {

// Inner Lit tile of the Overview grid in post_process.slang.
inline constexpr float kOverviewLitTileHeight = (0.96f - 0.04f) / 4.0f - 2.0f * 0.006f;

[[nodiscard]] constexpr float overviewLitTileLineWidth(float lineWidth) {
  return std::max(1.0f, lineWidth * kOverviewLitTileHeight);
}

struct SceneViewportScissor {
  VkViewport viewport{};
  VkRect2D scissor{};
};

[[nodiscard]] SceneViewportScissor buildSceneViewportScissor(
    VkExtent2D extent);

void recordSceneViewportAndScissor(VkCommandBuffer commandBuffer,
                                   VkExtent2D extent);

[[nodiscard]] bool recordOverviewLitTileViewport(VkCommandBuffer commandBuffer,
                                                  VkExtent2D extent);

}  // namespace container::renderer
