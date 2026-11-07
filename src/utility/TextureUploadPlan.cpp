#include <Container/utility/TextureUploadPlan.h>

#include <algorithm>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>

namespace container::gpu {
namespace {
[[noreturn]] void fail(const char* detail) {
  throw std::invalid_argument(std::string("texture upload: ") + detail);
}
uint64_t commonAlignment(uint64_t first, uint64_t second) {
  const auto factor = first / std::gcd(first, second);
  if (!second || factor > std::numeric_limits<uint64_t>::max() / second)
    fail("buffer offset alignment overflows");
  return factor * second;
}
} // namespace

TextureUploadPlan PlanTextureImageUpload(
    const container::material::TextureImageData& image,
    uint32_t maxImageDimension2D, bool canGenerateMipmaps,
    uint64_t maxStagingBytes, VkDeviceSize offsetAlignment) {
  if (image.mips.empty() || !maxImageDimension2D || !maxStagingBytes || !offsetAlignment)
    fail("empty image or zero limit");
  for (const auto component : {image.componentMapping.r, image.componentMapping.g,
                               image.componentMapping.b, image.componentMapping.a}) {
    switch (component) {
    case VK_COMPONENT_SWIZZLE_IDENTITY:
    case VK_COMPONENT_SWIZZLE_ZERO:
    case VK_COMPONENT_SWIZZLE_ONE:
    case VK_COMPONENT_SWIZZLE_R:
    case VK_COMPONENT_SWIZZLE_G:
    case VK_COMPONENT_SWIZZLE_B:
    case VK_COMPONENT_SWIZZLE_A:
      break;
    default:
      fail("image-view component mapping is invalid");
    }
  }
  const auto& base = image.mips.front();
  if (!base.width || !base.height || base.width > maxImageDimension2D || base.height > maxImageDimension2D)
    fail("dimensions exceed the device limit");
  uint32_t completeLevels = 1;
  for (uint32_t dimension = std::max(base.width, base.height); dimension > 1; dimension >>= 1)
    ++completeLevels;
  if (image.mips.size() > completeLevels) fail("too many mip levels for the base dimensions");
  const uint64_t blockBytes = container::material::TextureMipByteSize(image.format, 1, 1);
  if (!blockBytes) fail("unknown texture format");
  const uint64_t alignment = commonAlignment(commonAlignment(4, blockBytes), offsetAlignment);
  TextureUploadPlan plan;
  plan.extent = {base.width, base.height, 1};
  plan.suppliedMipLevels = static_cast<uint32_t>(image.mips.size());
  plan.generateMipmaps = !image.authoredMipChain && image.mips.size() == 1 &&
      canGenerateMipmaps && completeLevels > 1;
  plan.allocatedMipLevels = plan.generateMipmaps ? completeLevels : plan.suppliedMipLevels;
  plan.copies.reserve(image.mips.size());
  uint32_t width = base.width, height = base.height;
  for (size_t level = 0; level < image.mips.size(); ++level) {
    const auto& mip = image.mips[level];
    if (mip.width != width || mip.height != height) fail("mip dimensions do not follow the base image");
    const uint64_t expectedBytes = container::material::TextureMipByteSize(image.format, width, height);
    if (mip.bytes.size() != expectedBytes) fail("mip payload has an incorrect byte length");
    const uint64_t remainder = plan.stagingBytes % alignment;
    const uint64_t padding = remainder ? alignment - remainder : 0;
    if (plan.stagingBytes > maxStagingBytes || padding > maxStagingBytes - plan.stagingBytes)
      fail("staging alignment exceeds the upload budget");
    plan.stagingBytes += padding;
    if (plan.stagingBytes > std::numeric_limits<size_t>::max() ||
        expectedBytes > maxStagingBytes - plan.stagingBytes ||
        expectedBytes > std::numeric_limits<size_t>::max() - plan.stagingBytes)
      fail("mip chain exceeds the upload budget");
    VkBufferImageCopy copy{};
    copy.bufferOffset = plan.stagingBytes;
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.mipLevel = static_cast<uint32_t>(level);
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {width, height, 1};
    plan.copies.push_back(copy);
    plan.stagingBytes += expectedBytes;
    width = std::max(width >> 1, 1u);
    height = std::max(height >> 1, 1u);
  }
  return plan;
}
} // namespace container::gpu
