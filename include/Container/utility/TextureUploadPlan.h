#pragma once

#include <Container/utility/TextureImageData.h>

namespace container::gpu {

struct TextureUploadPlan {
  VkExtent3D extent{};
  uint32_t suppliedMipLevels{};
  uint32_t allocatedMipLevels{};
  VkDeviceSize stagingBytes{};
  bool generateMipmaps{};
  std::vector<VkBufferImageCopy> copies;
};

// Validates the complete CPU mip chain before a staging allocation or Vulkan
// object is created. Authored partial pyramids retain their exact level count.
// Buffer offsets satisfy four-byte, texel-block and caller alignment together.
[[nodiscard]] TextureUploadPlan PlanTextureImageUpload(
    const container::material::TextureImageData& image,
    uint32_t maxImageDimension2D, bool canGenerateMipmaps,
    uint64_t maxStagingBytes = 256ull * 1024 * 1024,
    VkDeviceSize offsetAlignment = 4);

} // namespace container::gpu
