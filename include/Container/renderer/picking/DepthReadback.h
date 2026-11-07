#pragma once

#include <vulkan/vulkan.h>

namespace container::renderer {

// Layout transitions must cover both packed aspects when separate depth/stencil
// layouts are disabled. The readback copy itself still selects only depth.
[[nodiscard]] constexpr VkImageAspectFlags
depthReadbackBarrierAspects(VkFormat depthFormat) {
  switch (depthFormat) {
  case VK_FORMAT_D16_UNORM_S8_UINT:
  case VK_FORMAT_D24_UNORM_S8_UINT:
  case VK_FORMAT_D32_SFLOAT_S8_UINT:
    return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
  default:
    return VK_IMAGE_ASPECT_DEPTH_BIT;
  }
}

} // namespace container::renderer
