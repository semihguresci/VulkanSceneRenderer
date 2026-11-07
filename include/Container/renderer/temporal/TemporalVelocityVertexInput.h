#pragma once

#include "Container/geometry/Vertex.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <array>

namespace container::renderer {

[[nodiscard]] inline std::array<VkVertexInputBindingDescription, 2>
temporalVelocityVertexBindings() {
  auto previous = container::geometry::Vertex::bindingDescription();
  previous.binding = 1;
  return {container::geometry::Vertex::bindingDescription(), previous};
}

[[nodiscard]] inline std::array<VkVertexInputAttributeDescription, 7>
temporalVelocityVertexAttributes() {
  const auto current = container::geometry::Vertex::attributeDescriptions();
  auto previousPosition = current[0];
  previousPosition.binding = 1;
  previousPosition.location = 6;
  auto previousNormal = current[3];
  previousNormal.binding = 1;
  previousNormal.location = 7;
  return {current[0], current[2], current[3], current[5], previousPosition,
          previousNormal, current[6]};
}

// Ordinary draw recorders bind only vertex binding zero. Bind history separately
// so both CPU and indirect routes use the same corresponding previous vertex.
// The callback keeps this command contract testable without a Vulkan device.
inline bool recordTemporalPreviousVertexBinding(
    VkCommandBuffer cmd, const container::gpu::BufferSlice& current,
    const container::gpu::BufferSlice& previous = {},
    PFN_vkCmdBindVertexBuffers bind = vkCmdBindVertexBuffers) {
  const auto& stream = previous.buffer != VK_NULL_HANDLE ? previous : current;
  if (cmd == VK_NULL_HANDLE || stream.buffer == VK_NULL_HANDLE || !bind)
    return false;
  bind(cmd, 1, 1, &stream.buffer, &stream.offset);
  return true;
}

} // namespace container::renderer
