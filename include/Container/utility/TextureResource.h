#pragma once
#include <cstdint>
#include <string>

#include "Container/common/CommonVulkan.h"
#include "Container/utility/TextureAllocationIdentity.h"

namespace container::material {

struct TextureResource {
  std::string name;
  VkImage image{VK_NULL_HANDLE};
  VkImageView imageView{VK_NULL_HANDLE};
  VkSampler sampler{VK_NULL_HANDLE};
  // Canonical MaterialSamplerKey value; GPU metadata resolves it to a dense
  // descriptor slot for the current scene rather than using it as an index.
  uint32_t samplerIndex{0};
  VkFormat format{VK_FORMAT_UNDEFINED};
  uint32_t width{}, height{}, mipLevels{};
  std::string contentKey;
  // Copyable image capability, not an ownership lease. Aliases retain it
  // explicitly through AllocationManager before their readers are published.
  container::gpu::TextureAllocationIdentity allocationIdentity;
};

struct TextureArrayResource {
  std::string name;
  VkImage image{VK_NULL_HANDLE};
  VkImageView imageView{VK_NULL_HANDLE};
  uint32_t width{0};
  uint32_t height{0};
  uint32_t layerCount{0};
};

}  // namespace container::material
