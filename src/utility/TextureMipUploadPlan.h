#pragma once

#include "Container/utility/TextureImageData.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace container::gpu::detail {

struct TextureMipTile {
  VkBufferImageCopy copy{};
  VkDeviceSize byteCount{};
};

// Select contiguous source bytes and the corresponding texel rectangle. Only
// a full block row can be extended to additional rows within a single tile.
[[nodiscard]] inline TextureMipTile planTextureMipTile(
    material::TextureFormatBlock block, VkExtent2D extent, uint32_t destinationMip,
    VkDeviceSize sourceOffset, VkDeviceSize budget) {
  constexpr auto maxOffset = static_cast<uint32_t>((std::numeric_limits<int32_t>::max)());
  if (!block.width || !block.height || !block.bytes || !extent.width || !extent.height ||
      extent.width > maxOffset || extent.height > maxOffset)
    throw std::invalid_argument("Texture upload tile requires a supported format and dimensions");
  const auto columns = (uint64_t(extent.width) + block.width - 1) / block.width;
  const auto rows = (uint64_t(extent.height) + block.height - 1) / block.height;
  if (rows > (std::numeric_limits<VkDeviceSize>::max)() / block.bytes / columns)
    throw std::overflow_error("Texture upload tile is too large");
  const auto sourceBytes = columns * rows * block.bytes;
  if (sourceOffset > sourceBytes || sourceOffset % block.bytes)
    throw std::invalid_argument("Texture upload tile offset is outside its block grid");
  const auto maxBlocks = budget / block.bytes;
  if (sourceOffset == sourceBytes || maxBlocks == 0)
    return {};
  const auto firstBlock = sourceOffset / block.bytes;
  const auto firstColumn = firstBlock % columns;
  const auto firstRow = firstBlock / columns;
  const auto copyColumns = std::min(maxBlocks, columns - firstColumn);
  const auto copyRows = copyColumns == columns ? std::min(maxBlocks / columns, rows - firstRow) : 1;
  const auto x = firstColumn * block.width;
  const auto y = firstRow * block.height;

  TextureMipTile result;
  result.byteCount = copyColumns * copyRows * block.bytes;
  result.copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, destinationMip, 0, 1};
  result.copy.imageOffset = {static_cast<int32_t>(x), static_cast<int32_t>(y), 0};
  result.copy.imageExtent = {
      static_cast<uint32_t>(std::min(copyColumns * block.width, extent.width - x)),
      static_cast<uint32_t>(std::min(copyRows * block.height, extent.height - y)), 1};
  return result;
}

} // namespace container::gpu::detail
