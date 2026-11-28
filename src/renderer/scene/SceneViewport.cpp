#include "Container/renderer/scene/SceneViewport.h"

#include <cmath>

namespace container::renderer {

SceneViewportScissor buildSceneViewportScissor(VkExtent2D extent) {
  SceneViewportScissor state{};
  state.viewport.x = 0.0f;
  state.viewport.y = static_cast<float>(extent.height);
  state.viewport.width = static_cast<float>(extent.width);
  // Scene passes use a negative-height viewport so NDC +Y maps to the top of
  // the framebuffer while projection matrices remain glTF/right-handed.
  state.viewport.height = -static_cast<float>(extent.height);
  state.viewport.minDepth = 0.0f;
  state.viewport.maxDepth = 1.0f;
  state.scissor.offset = {0, 0};
  state.scissor.extent = extent;
  return state;
}

void recordSceneViewportAndScissor(VkCommandBuffer commandBuffer,
                                   VkExtent2D extent) {
  const SceneViewportScissor state = buildSceneViewportScissor(extent);
  vkCmdSetViewport(commandBuffer, 0, 1, &state.viewport);
  vkCmdSetScissor(commandBuffer, 0, 1, &state.scissor);
}

bool recordOverviewLitTileViewport(VkCommandBuffer commandBuffer, VkExtent2D extent) {
  constexpr float left = 0.02f + 0.006f;
  constexpr float top = 0.04f + 0.006f;
  constexpr float tileWidth = (0.98f - 0.02f) / 3.0f - 2.0f * 0.006f;
  const float x = left * static_cast<float>(extent.width);
  const float y = top * static_cast<float>(extent.height);
  const float width = tileWidth * static_cast<float>(extent.width);
  const float height = kOverviewLitTileHeight * static_cast<float>(extent.height);
  const auto firstPixel = [](float edge) { return static_cast<int32_t>(std::ceil(edge - 0.5f)); };
  const int32_t leftPixel = firstPixel(x), topPixel = firstPixel(y);
  const int32_t rightPixel = firstPixel(x + width), bottomPixel = firstPixel(y + height);
  if (rightPixel <= leftPixel || bottomPixel <= topPixel)
    return false;
  const VkViewport viewport{x, y + height, width, -height, 0.0f, 1.0f};
  const VkRect2D scissor{{leftPixel, topPixel}, {static_cast<uint32_t>(rightPixel - leftPixel),
                                                static_cast<uint32_t>(bottomPixel - topPixel)}};
  vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
  vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
  return true;
}

}  // namespace container::renderer
