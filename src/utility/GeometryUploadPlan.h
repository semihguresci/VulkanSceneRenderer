#pragma once

#include "Container/utility/GeometryUploadBatch.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace container::gpu::detail {

constexpr VkDeviceSize geometryCopyAlignment = 4;
constexpr VkBufferUsageFlags geometryBufferUsage =
    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
constexpr VkBufferUsageFlags supportedGeometryUsage = geometryBufferUsage |
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

constexpr VkDeviceSize alignGeometryCopyDown(VkDeviceSize bytes) noexcept {
  return bytes - bytes % geometryCopyAlignment;
}

constexpr VkDeviceSize paddedGeometrySize(VkDeviceSize bytes) {
  if (bytes > alignGeometryCopyDown((std::numeric_limits<VkDeviceSize>::max)()))
    throw std::overflow_error("Geometry upload copy padding overflows the buffer size");
  return alignGeometryCopyDown(bytes + geometryCopyAlignment - 1);
}

struct GeometryUploadPlan {
  VkDeviceSize totalBytes{};
  VkDeviceSize stagingBytes{};
};

[[nodiscard]] inline GeometryUploadPlan planGeometryUpload(
    std::span<const GeometryUploadSource> sources, VkDeviceSize cap) {
  VkDeviceSize totalBytes = 0;
  VkDeviceSize totalCopyBytes = 0;
  VkDeviceSize maxTargetBytes = 0;
  for (const auto& source : sources) {
    if (source.bytes.empty())
      continue;
    if (!(source.usage & geometryBufferUsage) || (source.usage & ~supportedGeometryUsage))
      throw std::invalid_argument("Geometry upload requires vertex or index buffer usage");
    const auto size = static_cast<VkDeviceSize>(source.bytes.size());
    const auto paddedSize = paddedGeometrySize(size);
    if (paddedSize > (std::numeric_limits<VkDeviceSize>::max)() - totalCopyBytes)
      throw std::overflow_error("Geometry upload batch is too large");
    totalBytes += size;
    totalCopyBytes += paddedSize;
    maxTargetBytes = (std::max)(maxTargetBytes, paddedSize);
  }
  const auto stagingBytes = (std::min)(maxTargetBytes, alignGeometryCopyDown(cap));
  if (totalBytes && !stagingBytes)
    throw std::invalid_argument("Geometry upload requires at least four staging bytes");
  return {totalBytes, stagingBytes};
}

struct GeometryUploadChunk {
  VkDeviceSize logicalBytes{};
  VkDeviceSize copyBytes{};
};

[[nodiscard]] constexpr GeometryUploadChunk planGeometryUploadChunk(
    VkDeviceSize logicalSize, VkDeviceSize offset, VkDeviceSize stagingBytes,
    VkDeviceSize budget) {
  const auto capacity = alignGeometryCopyDown((std::min)(stagingBytes, budget));
  if (offset >= logicalSize || capacity == 0)
    return {};
  const auto logicalBytes = (std::min)(logicalSize - offset, capacity);
  return {logicalBytes, paddedGeometrySize(logicalBytes)};
}

} // namespace container::gpu::detail
