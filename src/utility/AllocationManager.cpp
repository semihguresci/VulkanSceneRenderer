#include "Container/utility/AllocationManager.h"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>

#include "stb_image.h"

#include "Container/utility/Platform.h"

namespace container::gpu {

namespace {

void EnsureArenaCapacity(std::unique_ptr<BufferArena>& arena,
                         VulkanMemoryManager& memoryManager,
                         VkDeviceSize requiredSize,
                         VkBufferUsageFlags usage,
                         VmaMemoryUsage memoryUsage,
                         VmaAllocationCreateFlags allocationFlags) {
  const VkDeviceSize safeRequiredSize = std::max<VkDeviceSize>(1, requiredSize);

  if (!arena || arena->remainingSize() < safeRequiredSize) {
    VkDeviceSize requestedSize = safeRequiredSize;
    if (arena) {
      requestedSize = std::max(safeRequiredSize, arena->totalSize() * 2);
    }
    arena = std::make_unique<BufferArena>(memoryManager, requestedSize, usage,
                                          memoryUsage, allocationFlags);
  }
}

uint32_t CalculateTextureMipLevels(uint32_t width, uint32_t height) {
  uint32_t levels = 1u;
  uint32_t dimension = std::max(width, height);
  while (dimension > 1u) {
    dimension /= 2u;
    ++levels;
  }
  return levels;
}

bool textureFormatSupportsLinearBlit(VkPhysicalDevice physicalDevice,
                                     VkFormat format) {
  VkFormatProperties properties{};
  vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &properties);
  constexpr VkFormatFeatureFlags requiredFeatures =
      VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
  return (properties.optimalTilingFeatures & requiredFeatures) ==
         requiredFeatures;
}

}  // namespace

AllocationManager::~AllocationManager() { cleanup(); }

void AllocationManager::initialize(VkInstance instance,
                                   VkPhysicalDevice physicalDevice,
                                   VkDevice device, VkQueue graphicsQueue,
                                   VkCommandPool commandPool,
                                   const container::app::AppConfig& config) {
  instance_ = instance;
  physicalDevice_ = physicalDevice;
  device_ = device;
  graphicsQueue_ = graphicsQueue;
  commandPool_ = commandPool;
  config_ = config;

  memoryManager_ = std::make_unique<VulkanMemoryManager>(
      instance_, physicalDevice_, device_);
}

void AllocationManager::cleanup() {
  resetTextureAllocations(TextureAllocationResetScope::All);
  indexArena_.reset();
  vertexArena_.reset();
  memoryManager_.reset();

  instance_ = VK_NULL_HANDLE;
  physicalDevice_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
  graphicsQueue_ = VK_NULL_HANDLE;
  commandPool_ = VK_NULL_HANDLE;
}

BufferSlice AllocationManager::uploadVertices(
    std::span<const container::geometry::Vertex> vertices) {
  if (vertices.empty()) return {};
  VkDeviceSize bufferSize = sizeof(container::geometry::Vertex) * vertices.size();

  EnsureArenaCapacity(
      vertexArena_, *memoryManager_, bufferSize,
      VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT);

  StagingBuffer stagingBuffer(*memoryManager_, bufferSize);
  stagingBuffer.upload({reinterpret_cast<const std::byte*>(vertices.data()),
                        static_cast<size_t>(bufferSize)});

  BufferSlice slice =
      vertexArena_->allocate(bufferSize, alignof(container::geometry::Vertex));

  copyBuffer(stagingBuffer.buffer().buffer, slice.buffer, bufferSize, 0,
             slice.offset);

  return slice;
}

BufferSlice AllocationManager::uploadIndices(
    std::span<const uint32_t> indices) {
  if (indices.empty()) return {};
  VkDeviceSize bufferSize = sizeof(uint32_t) * indices.size();

  EnsureArenaCapacity(
      indexArena_, *memoryManager_, bufferSize,
      VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT);

  StagingBuffer stagingBuffer(*memoryManager_, bufferSize);
  stagingBuffer.upload({reinterpret_cast<const std::byte*>(indices.data()),
                        static_cast<size_t>(bufferSize)});

  VkDeviceSize alignment = std::max<VkDeviceSize>(sizeof(uint32_t), 4);
  BufferSlice slice = indexArena_->allocate(bufferSize, alignment);

  copyBuffer(stagingBuffer.buffer().buffer, slice.buffer, bufferSize, 0,
             slice.offset);

  return slice;
}

AllocatedBuffer AllocationManager::uploadBuffer(
    std::span<const std::byte> bytes,
    VkBufferUsageFlags usage) {
  if (bytes.empty()) return {};

  const VkDeviceSize bufferSize = static_cast<VkDeviceSize>(bytes.size());
  AllocatedBuffer buffer = createBuffer(
      bufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | usage,
      VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT);

  StagingBuffer stagingBuffer(*memoryManager_, bufferSize);
  stagingBuffer.upload(bytes);
  copyBuffer(stagingBuffer.buffer().buffer, buffer.buffer, bufferSize);

  return buffer;
}

AllocatedBuffer AllocationManager::createBuffer(
    VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage,
    VmaAllocationCreateFlags allocationFlags, VkSharingMode sharingMode) {
  return memoryManager_->createBuffer(size, usage, memoryUsage, allocationFlags,
                                      sharingMode);
}

void AllocationManager::destroyBuffer(AllocatedBuffer& buffer) {
  if (memoryManager_) {
    memoryManager_->destroyBuffer(buffer);
  }
}

container::material::TextureResource AllocationManager::createTextureFromFile(
    const std::string& texturePath, VkFormat format) {
  int texWidth = 0;
  int texHeight = 0;
  int texChannels = 0;
  std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
      stbi_load(texturePath.c_str(), &texWidth, &texHeight, &texChannels,
                STBI_rgb_alpha),
      stbi_image_free);

  if (!pixels) {
    throw std::runtime_error("failed to load texture: " + texturePath);
  }

  const std::string normalizedName = container::util::pathToUtf8(
      container::util::pathFromUtf8(texturePath).lexically_normal());
  const VkDeviceSize imageSize = static_cast<VkDeviceSize>(texWidth) *
                                 static_cast<VkDeviceSize>(texHeight) * 4;
  return createTextureFromRgbaPixels(
      normalizedName,
      {reinterpret_cast<const std::byte*>(pixels.get()),
       static_cast<size_t>(imageSize)},
      static_cast<uint32_t>(texWidth), static_cast<uint32_t>(texHeight),
      format);
}

container::material::TextureResource
AllocationManager::createTextureFromEncodedBytes(
    const std::string& textureName,
    std::span<const std::byte> encodedBytes,
    VkFormat format) {
  if (encodedBytes.empty()) {
    throw std::runtime_error("texture byte payload is empty: " + textureName);
  }
  if (encodedBytes.size() >
      static_cast<size_t>(std::numeric_limits<int>::max())) {
    throw std::runtime_error("texture byte payload is too large: " +
                             textureName);
  }

  int texWidth = 0;
  int texHeight = 0;
  int texChannels = 0;
  std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
      stbi_load_from_memory(
          reinterpret_cast<const stbi_uc*>(encodedBytes.data()),
          static_cast<int>(encodedBytes.size()), &texWidth, &texHeight,
          &texChannels, STBI_rgb_alpha),
      stbi_image_free);

  if (!pixels) {
    throw std::runtime_error("failed to decode texture: " + textureName);
  }

  const VkDeviceSize imageSize = static_cast<VkDeviceSize>(texWidth) *
                                 static_cast<VkDeviceSize>(texHeight) * 4;
  return createTextureFromRgbaPixels(
      textureName,
      {reinterpret_cast<const std::byte*>(pixels.get()),
       static_cast<size_t>(imageSize)},
      static_cast<uint32_t>(texWidth), static_cast<uint32_t>(texHeight),
      format);
}

container::material::TextureResource
AllocationManager::createTextureFromRgbaPixels(
    const std::string& textureName,
    std::span<const std::byte> rgbaPixels,
    uint32_t width,
    uint32_t height,
    VkFormat format) {
  if (width == 0u || height == 0u) {
    throw std::runtime_error("texture dimensions are invalid: " + textureName);
  }

  const VkDeviceSize imageSize = static_cast<VkDeviceSize>(width) *
                                 static_cast<VkDeviceSize>(height) * 4;
  if (rgbaPixels.size() < static_cast<size_t>(imageSize)) {
    throw std::runtime_error("texture pixel payload is truncated: " +
                             textureName);
  }

  StagingBuffer stagingBuffer(*memoryManager_, imageSize);
  stagingBuffer.upload(rgbaPixels.first(static_cast<size_t>(imageSize)));

  const uint32_t mipLevels = textureFormatSupportsLinearBlit(physicalDevice_,
                                                             format)
                                 ? CalculateTextureMipLevels(width, height)
                                 : 1u;

  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.format = format;
  imageInfo.extent = {width, height, 1};
  imageInfo.mipLevels = mipLevels;
  imageInfo.arrayLayers = 1;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.usage =
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
      VK_IMAGE_USAGE_SAMPLED_BIT;
  imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VmaAllocationCreateInfo allocInfo{};
  allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

  VkImage image = VK_NULL_HANDLE;
  VmaAllocation allocation = nullptr;

  if (vmaCreateImage(memoryManager_->allocator(), &imageInfo, &allocInfo,
                     &image, &allocation, nullptr) != VK_SUCCESS) {
    throw std::runtime_error("failed to create texture image");
  }

  VkImageView imageView = VK_NULL_HANDLE;
  bool registeredTexture = false;
  try {
    transitionImageLayout(image, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, 0u,
                          mipLevels);

    copyBufferToImage(stagingBuffer.buffer().buffer, image, width, height);

    if (mipLevels > 1u) {
      generateTextureMipmaps(image, format, width, height, mipLevels);
    } else {
      transitionImageLayout(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    imageView = createImageView(image, imageInfo.format,
                                VK_IMAGE_VIEW_TYPE_2D, 1u, mipLevels);

    textureAllocations_.push_back({image, imageView, allocation});
    registeredTexture = true;
  } catch (...) {
    if (imageView != VK_NULL_HANDLE) {
      vkDestroyImageView(device_, imageView, nullptr);
    }
    if (!registeredTexture && image != VK_NULL_HANDLE) {
      vmaDestroyImage(memoryManager_->allocator(), image, allocation);
    }
    throw;
  }

  container::material::TextureResource resource{};
  resource.name = textureName;
  resource.image = image;
  resource.imageView = imageView;

  return resource;
}

container::material::TextureArrayResource
AllocationManager::createTexture2DArrayFromRgbaPixels(
    const std::string& textureName,
    std::span<const std::byte> rgbaPixels,
    uint32_t width,
    uint32_t height,
    uint32_t layerCount,
    VkFormat format,
    TextureAllocationLifetime lifetime) {
  if (width == 0u || height == 0u || layerCount == 0u) {
    throw std::runtime_error("texture array dimensions are invalid: " +
                             textureName);
  }

  const VkDeviceSize layerSize = static_cast<VkDeviceSize>(width) *
                                 static_cast<VkDeviceSize>(height) * 4;
  const VkDeviceSize imageSize =
      layerSize * static_cast<VkDeviceSize>(layerCount);
  if (rgbaPixels.size() < static_cast<size_t>(imageSize)) {
    throw std::runtime_error("texture array pixel payload is truncated: " +
                             textureName);
  }

  StagingBuffer stagingBuffer(*memoryManager_, imageSize);
  stagingBuffer.upload(rgbaPixels.first(static_cast<size_t>(imageSize)));

  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.format = format;
  imageInfo.extent = {width, height, 1};
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = layerCount;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.usage =
      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VmaAllocationCreateInfo allocInfo{};
  allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

  VkImage image = VK_NULL_HANDLE;
  VmaAllocation allocation = nullptr;
  if (vmaCreateImage(memoryManager_->allocator(), &imageInfo, &allocInfo,
                     &image, &allocation, nullptr) != VK_SUCCESS) {
    throw std::runtime_error("failed to create texture array image");
  }

  VkImageView imageView = VK_NULL_HANDLE;
  bool registeredTexture = false;
  try {
    transitionImageLayout(image, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layerCount);
    copyBufferToImage(stagingBuffer.buffer().buffer, image, width, height,
                      layerCount);
    transitionImageLayout(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          layerCount);
    imageView = createImageView(image, format, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                layerCount);

    textureAllocations_.push_back({image, imageView, allocation, lifetime});
    registeredTexture = true;
  } catch (...) {
    if (imageView != VK_NULL_HANDLE) {
      vkDestroyImageView(device_, imageView, nullptr);
    }
    if (!registeredTexture && image != VK_NULL_HANDLE) {
      vmaDestroyImage(memoryManager_->allocator(), image, allocation);
    }
    throw;
  }

  container::material::TextureArrayResource resource{};
  resource.name = textureName;
  resource.image = image;
  resource.imageView = imageView;
  resource.width = width;
  resource.height = height;
  resource.layerCount = layerCount;
  return resource;
}

void AllocationManager::resetTextureAllocations(
    TextureAllocationResetScope scope) {
  if (!memoryManager_) {
    textureAllocations_.clear();
    return;
  }

  const auto shouldReset = [scope](const TextureAllocation& texture) {
    return scope == TextureAllocationResetScope::All ||
           texture.lifetime == TextureAllocationLifetime::Scene;
  };

  for (auto& texture : textureAllocations_) {
    if (!shouldReset(texture)) {
      continue;
    }

    if (texture.imageView != VK_NULL_HANDLE) {
      vkDestroyImageView(device_, texture.imageView, nullptr);
      texture.imageView = VK_NULL_HANDLE;
    }

    if (texture.image != VK_NULL_HANDLE) {
      vmaDestroyImage(memoryManager_->allocator(), texture.image,
                      texture.allocation);
      texture.image = VK_NULL_HANDLE;
      texture.allocation = nullptr;
    }
  }

  std::erase_if(textureAllocations_, shouldReset);
}

/* ---------- Command helpers ---------- */

VkCommandBuffer AllocationManager::beginSingleTimeCommands() {
  VkCommandBufferAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocInfo.commandPool = commandPool_;
  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandBufferCount = 1;

  VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
  if (vkAllocateCommandBuffers(device_, &allocInfo, &commandBuffer) !=
      VK_SUCCESS) {
    throw std::runtime_error("failed to allocate single-use command buffer");
  }

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

  if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
    vkFreeCommandBuffers(device_, commandPool_, 1, &commandBuffer);
    throw std::runtime_error("failed to begin single-use command buffer");
  }
  return commandBuffer;
}

void AllocationManager::endSingleTimeCommands(VkCommandBuffer commandBuffer) {
  if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
    vkFreeCommandBuffers(device_, commandPool_, 1, &commandBuffer);
    throw std::runtime_error("failed to end single-use command buffer");
  }

  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &commandBuffer;

  if (vkQueueSubmit(graphicsQueue_, 1, &submitInfo, VK_NULL_HANDLE) !=
      VK_SUCCESS) {
    vkFreeCommandBuffers(device_, commandPool_, 1, &commandBuffer);
    throw std::runtime_error("failed to submit single-use command buffer");
  }
  if (vkQueueWaitIdle(graphicsQueue_) != VK_SUCCESS) {
    vkFreeCommandBuffers(device_, commandPool_, 1, &commandBuffer);
    throw std::runtime_error("failed to wait for single-use command buffer");
  }

  vkFreeCommandBuffers(device_, commandPool_, 1, &commandBuffer);
}

void AllocationManager::copyBuffer(VkBuffer src, VkBuffer dst,
                                   VkDeviceSize size, VkDeviceSize srcOffset,
                                   VkDeviceSize dstOffset) {
  VkCommandBuffer cmd = beginSingleTimeCommands();

  VkBufferCopy region{};
  region.srcOffset = srcOffset;
  region.dstOffset = dstOffset;
  region.size = size;

  vkCmdCopyBuffer(cmd, src, dst, 1, &region);

  endSingleTimeCommands(cmd);
}

void AllocationManager::transitionImageLayout(VkImage image,
                                              VkImageLayout oldLayout,
                                              VkImageLayout newLayout,
                                              uint32_t layerCount,
                                              uint32_t baseMipLevel,
                                              uint32_t levelCount) {
  VkCommandBuffer cmd = beginSingleTimeCommands();

  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.baseMipLevel = baseMipLevel;
  barrier.subresourceRange.levelCount = levelCount;
  barrier.subresourceRange.layerCount = layerCount;

  VkPipelineStageFlags srcStage;
  VkPipelineStageFlags dstStage;

  if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
      newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  } else {
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  }

  vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1,
                       &barrier);

  endSingleTimeCommands(cmd);
}

void AllocationManager::copyBufferToImage(VkBuffer buffer, VkImage image,
                                          uint32_t width, uint32_t height,
                                          uint32_t layerCount) {
  VkCommandBuffer cmd = beginSingleTimeCommands();

  VkBufferImageCopy region{};
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.layerCount = layerCount;
  region.imageExtent = {width, height, 1};

  vkCmdCopyBufferToImage(cmd, buffer, image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  endSingleTimeCommands(cmd);
}

void AllocationManager::generateTextureMipmaps(VkImage image, VkFormat format,
                                               uint32_t width, uint32_t height,
                                               uint32_t mipLevels) {
  if (mipLevels <= 1u) {
    return;
  }
  if (!textureFormatSupportsLinearBlit(physicalDevice_, format)) {
    throw std::runtime_error(
        "texture format does not support linear mipmap generation");
  }

  VkCommandBuffer cmd = beginSingleTimeCommands();

  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.image = image;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = 1;
  barrier.subresourceRange.levelCount = 1;

  int32_t mipWidth = static_cast<int32_t>(width);
  int32_t mipHeight = static_cast<int32_t>(height);

  for (uint32_t mip = 1u; mip < mipLevels; ++mip) {
    barrier.subresourceRange.baseMipLevel = mip - 1u;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);

    VkImageBlit blit{};
    blit.srcOffsets[0] = {0, 0, 0};
    blit.srcOffsets[1] = {mipWidth, mipHeight, 1};
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.mipLevel = mip - 1u;
    blit.srcSubresource.baseArrayLayer = 0;
    blit.srcSubresource.layerCount = 1;
    blit.dstOffsets[0] = {0, 0, 0};
    blit.dstOffsets[1] = {std::max(mipWidth / 2, 1),
                          std::max(mipHeight / 2, 1), 1};
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.mipLevel = mip;
    blit.dstSubresource.baseArrayLayer = 0;
    blit.dstSubresource.layerCount = 1;

    vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   VK_FILTER_LINEAR);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);

    mipWidth = std::max(mipWidth / 2, 1);
    mipHeight = std::max(mipHeight / 2, 1);
  }

  barrier.subresourceRange.baseMipLevel = mipLevels - 1u;
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &barrier);

  endSingleTimeCommands(cmd);
}

VkImageView AllocationManager::createImageView(VkImage image, VkFormat format,
                                               VkImageViewType viewType,
                                               uint32_t layerCount,
                                               uint32_t levelCount) {
  VkImageViewCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  info.image = image;
  info.viewType = viewType;
  info.format = format;
  info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  info.subresourceRange.levelCount = levelCount;
  info.subresourceRange.layerCount = layerCount;

  VkImageView view = VK_NULL_HANDLE;
  if (vkCreateImageView(device_, &info, nullptr, &view) != VK_SUCCESS) {
    throw std::runtime_error("failed to create texture image view");
  }
  return view;
}

}  // namespace container::gpu
